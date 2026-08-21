// SPDX-License-Identifier: GPL-2.0-or-later

#include "channel.hpp"
#include "in_process_service.hpp"
#include "protocol.hpp"
#include "unique_descriptor.hpp"
#include "unix_socket_channel.hpp"
#include "virtio_console_device.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

namespace protocol = uml::console::protocol;

void require(bool condition)
{
    if (!condition)
        std::abort();
}

template<typename Operation>
void requireThrows(Operation&& operation)
{
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    std::abort();
}

class TestChannel final : public uml::console::ConsoleChannel {
public:
    std::vector<std::uint8_t> read(std::size_t maximumBytes) override
    {
        const auto count = std::min(maximumBytes, input.size());
        std::vector<std::uint8_t> result;
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(input.front());
            input.pop_front();
        }
        return result;
    }

    void write(std::span<const std::uint8_t> bytes) noexcept override
    {
        output.insert(output.end(), bytes.begin(), bytes.end());
    }

    int eventDescriptor() const noexcept override { return descriptor; }
    void drainEventSource() override { ++drainCount; }
    void reset() noexcept override
    {
        ++resetCount;
        input.clear();
        output.clear();
    }

    std::deque<std::uint8_t> input;
    std::vector<std::uint8_t> output;
    int descriptor {73};
    unsigned drainCount {};
    unsigned resetCount {};
};

struct DeviceFixture final {
    DeviceFixture()
    {
        auto owned = std::make_unique<TestChannel>();
        channel = owned.get();
        device = std::make_unique<uml::console::VirtioConsoleDevice>(
            std::move(owned));
    }

    TestChannel* channel {};
    std::unique_ptr<uml::console::VirtioConsoleDevice> device;
};

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        std::array<char, 64> pattern {};
        constexpr std::string_view prefix{"/tmp/uml-console-test.XXXXXX"};
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        char* created = ::mkdtemp(pattern.data());
        require(created != nullptr);
        path_ = created;
    }

    ~TemporaryDirectory() { (void)::rmdir(path_.c_str()); }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void waitReadable(int descriptor)
{
    pollfd readiness {
        .fd = descriptor,
        .events = POLLIN,
        .revents = 0,
    };
    require(::poll(&readiness, 1, 1000) == 1);
}

uml::gpu::UniqueDescriptor connectClient(
    const std::filesystem::path& path)
{
    uml::gpu::UniqueDescriptor client {::socket(AF_UNIX, SOCK_STREAM, 0)};
    require(client.get() >= 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    require(path.string().size() < sizeof(address.sun_path));
    std::memcpy(
        address.sun_path, path.c_str(), path.string().size() + 1U);
    require(::connect(
                client.get(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0);
    return client;
}

void deviceImplementsTheTwoQueueConsoleContract()
{
    DeviceFixture fixture;
    require(fixture.device->features() == 0);
    require(fixture.device->queueCount() == protocol::queueCount);
    require(fixture.device->configuration().size() ==
            sizeof(protocol::Configuration));

    const auto empty = fixture.device->processQueue(
        protocol::inputQueue, 1, {}, 16);
    require(empty.action == uml::virtio::VhostUserQueueAction::retry);

    constexpr std::array<std::uint8_t, 5> input {'l', 'o', 'g', '\n', 'x'};
    fixture.channel->input.insert(
        fixture.channel->input.end(), input.begin(), input.end());
    const auto received = fixture.device->processQueue(
        protocol::inputQueue, 2, {}, 4);
    require(received.action == uml::virtio::VhostUserQueueAction::complete);
    require(received.response ==
            std::vector<std::uint8_t>(input.begin(), input.begin() + 4));

    constexpr std::array<std::uint8_t, 4> output {'o', 'k', '\r', '\n'};
    const auto sent = fixture.device->processQueue(
        protocol::outputQueue, 3, output, 0);
    require(sent.action == uml::virtio::VhostUserQueueAction::complete);
    require(sent.response.empty());
    require(fixture.channel->output ==
            std::vector<std::uint8_t>(output.begin(), output.end()));

    require(fixture.device->eventDescriptor() == fixture.channel->descriptor);
    fixture.device->drainEventSource();
    require(fixture.channel->drainCount == 1);
    fixture.device->reset();
    require(fixture.channel->resetCount == 1);

    requireThrows([&] {
        (void)fixture.device->processQueue(
            protocol::inputQueue, 4, output, 4);
    });
    requireThrows([&] {
        (void)fixture.device->processQueue(
            protocol::outputQueue, 5, output, 1);
    });
    requireThrows([&] {
        (void)fixture.device->processQueue(2, 6, {}, 1);
    });
    requireThrows([&] {
        fixture.device->writeConfiguration(0, 0, output);
    });
}

void unixSocketChannelSupportsTailInputAndReconnect()
{
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "console.sock";
    {
        uml::console::UnixSocketChannel channel(path);
        struct stat status {};
        require(::lstat(path.c_str(), &status) == 0);
        require(S_ISSOCK(status.st_mode));
        require((status.st_mode & 0777) == 0600);

        constexpr std::array<std::uint8_t, 5> boot {'b', 'o', 'o', 't', '\n'};
        channel.write(boot);
        auto client = connectClient(path);
        waitReadable(channel.eventDescriptor());
        channel.drainEventSource();
        waitReadable(client.get());
        std::array<std::uint8_t, 16> received {};
        require(::recv(client.get(), received.data(), received.size(), 0) ==
                static_cast<ssize_t>(boot.size()));
        require(std::equal(boot.begin(), boot.end(), received.begin()));

        constexpr std::array<std::uint8_t, 7> command {
            'l', 'o', 'g', 'c', 'a', 't', '\n'};
        require(::send(client.get(), command.data(), command.size(), 0) ==
                static_cast<ssize_t>(command.size()));
        waitReadable(channel.eventDescriptor());
        channel.drainEventSource();
        require(channel.read(4) ==
                std::vector<std::uint8_t>(command.begin(), command.begin() + 4));
        require(channel.read(16) ==
                std::vector<std::uint8_t>(command.begin() + 4, command.end()));

        constexpr std::array<std::uint8_t, 3> reply {'o', 'k', '\n'};
        channel.write(reply);
        waitReadable(client.get());
        require(::recv(client.get(), received.data(), received.size(), 0) ==
                static_cast<ssize_t>(reply.size()));
        require(std::equal(reply.begin(), reply.end(), received.begin()));

        client.reset();
        waitReadable(channel.eventDescriptor());
        channel.drainEventSource();
        auto reconnected = connectClient(path);
        waitReadable(channel.eventDescriptor());
        channel.drainEventSource();
        channel.write(reply);
        waitReadable(reconnected.get());
        require(::recv(
                    reconnected.get(), received.data(), received.size(), 0) ==
                static_cast<ssize_t>(reply.size()));
    }
    struct stat removed {};
    require(::lstat(path.c_str(), &removed) != 0 && errno == ENOENT);
}

void inProcessServicePublishesAndRemovesTheExternalSocket()
{
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "service.sock";
    uml::console::InProcessConsoleService service(path);
    struct stat status {};
    require(::lstat(path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode));
    const auto connection = service.takeConnectionName();
    require(connection.starts_with("fd="));
    const int descriptor = std::stoi(connection.substr(3));
    require(descriptor >= 0);
    (void)::close(descriptor);
    service.stop();
    require(::lstat(path.c_str(), &status) != 0 && errno == ENOENT);
}

} // namespace

int main()
{
    deviceImplementsTheTwoQueueConsoleContract();
    unixSocketChannelSupportsTailInputAndReconnect();
    inProcessServicePublishesAndRemovesTheExternalSocket();
    return EXIT_SUCCESS;
}
