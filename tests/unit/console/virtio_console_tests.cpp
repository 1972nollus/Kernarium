// SPDX-License-Identifier: GPL-2.0-or-later

#include "channel.hpp"
#include "connected_socket_channel.hpp"
#include "in_process_service.hpp"
#include "protocol.hpp"
#include "scratch_directory.hpp"
#include "unique_descriptor.hpp"
#include "unix_socket_channel.hpp"
#include "virtio_console_device.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <atomic>
#include <thread>
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

struct NamedDeviceFixture final {
    NamedDeviceFixture()
    {
        std::array<int, 2> pipeDescriptors {-1, -1};
        require(::pipe(pipeDescriptors.data()) == 0);
        channelEvent = uml::gpu::UniqueDescriptor {pipeDescriptors[0]};
        channelEventWriter = uml::gpu::UniqueDescriptor {pipeDescriptors[1]};
        auto owned = std::make_unique<TestChannel>();
        channel = owned.get();
        channel->descriptor = channelEvent.get();
        device = std::make_unique<uml::console::VirtioConsoleDevice>(
            std::move(owned), std::string(protocol::hostSerialPortName));
    }

    TestChannel* channel {};
    uml::gpu::UniqueDescriptor channelEvent;
    uml::gpu::UniqueDescriptor channelEventWriter;
    std::unique_ptr<uml::console::VirtioConsoleDevice> device;
};

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        auto pattern = uml::testing::socket_scratch_template(
            "uml-console-test.XXXXXX");
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

std::vector<std::uint8_t> controlMessage(
    std::uint32_t id,
    protocol::ControlEvent event,
    std::uint16_t value)
{
    const protocol::Control control {
        .id = id,
        .event = static_cast<std::uint16_t>(event),
        .value = value,
    };
    const auto* first = reinterpret_cast<const std::uint8_t*>(&control);
    return {first, first + sizeof(control)};
}

protocol::Control decodeControl(std::span<const std::uint8_t> message)
{
    require(message.size() >= sizeof(protocol::Control));
    protocol::Control control {};
    std::memcpy(&control, message.data(), sizeof(control));
    return control;
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

void deviceImplementsTheNamedSerialControlHandshake()
{
    NamedDeviceFixture fixture;
    require(fixture.device->features() == protocol::featureMultiport);
    require(fixture.device->queueCount() == protocol::namedSerialQueueCount);
    const auto configurationBytes = fixture.device->configuration();
    require(configurationBytes.size() == sizeof(protocol::Configuration));
    protocol::Configuration configuration {};
    std::memcpy(
        &configuration, configurationBytes.data(), sizeof(configuration));
    require(configuration.maximumPorts == 1);

    const auto empty = fixture.device->processQueue(
        protocol::controlInputQueue, 1, {}, 512);
    require(empty.action == uml::virtio::VhostUserQueueAction::retry);

    const auto ready = controlMessage(
        protocol::badPortId, protocol::ControlEvent::deviceReady, 1);
    const auto readyResult = fixture.device->processQueue(
        protocol::controlOutputQueue, 2, ready, 0);
    require(
        readyResult.action == uml::virtio::VhostUserQueueAction::complete);
    waitReadable(fixture.device->eventDescriptor());
    fixture.device->drainEventSource();

    requireThrows([&] {
        (void)fixture.device->processQueue(
            protocol::controlInputQueue, 3, {}, sizeof(protocol::Control) - 1U);
    });
    const auto add = fixture.device->processQueue(
        protocol::controlInputQueue, 4, {}, 512);
    require(add.action == uml::virtio::VhostUserQueueAction::complete);
    const auto addControl = decodeControl(add.response);
    require(addControl.id == protocol::hostSerialPortId);
    require(addControl.event ==
            static_cast<std::uint16_t>(protocol::ControlEvent::portAdd));
    require(addControl.value == 1);

    const auto portReady = controlMessage(
        protocol::hostSerialPortId, protocol::ControlEvent::portReady, 1);
    const auto portReadyResult = fixture.device->processQueue(
        protocol::controlOutputQueue, 5, portReady, 0);
    require(portReadyResult.action ==
            uml::virtio::VhostUserQueueAction::complete);
    waitReadable(fixture.device->eventDescriptor());
    fixture.device->drainEventSource();

    const auto name = fixture.device->processQueue(
        protocol::controlInputQueue, 6, {}, 512);
    require(name.action == uml::virtio::VhostUserQueueAction::complete);
    const auto nameControl = decodeControl(name.response);
    require(nameControl.id == protocol::hostSerialPortId);
    require(nameControl.event ==
            static_cast<std::uint16_t>(protocol::ControlEvent::portName));
    const std::string_view receivedName {
        reinterpret_cast<const char*>(
            name.response.data() + sizeof(protocol::Control)),
        name.response.size() - sizeof(protocol::Control)};
    require(receivedName == protocol::hostSerialPortName);

    const auto open = fixture.device->processQueue(
        protocol::controlInputQueue, 7, {}, 512);
    require(open.action == uml::virtio::VhostUserQueueAction::complete);
    const auto openControl = decodeControl(open.response);
    require(openControl.id == protocol::hostSerialPortId);
    require(openControl.event ==
            static_cast<std::uint16_t>(protocol::ControlEvent::portOpen));
    require(openControl.value == 1);

    const auto guestOpen = controlMessage(
        protocol::hostSerialPortId, protocol::ControlEvent::portOpen, 1);
    require(fixture.device
                ->processQueue(
                    protocol::controlOutputQueue, 7, guestOpen, 0)
                .action == uml::virtio::VhostUserQueueAction::complete);

    requireThrows([&] {
        const auto invalid = controlMessage(
            protocol::hostSerialPortId, protocol::ControlEvent::portName, 1);
        (void)fixture.device->processQueue(
            protocol::controlOutputQueue, 9, invalid, 0);
    });

    fixture.device->reset();
    require(fixture.channel->resetCount == 1);
    require(fixture.device
                ->processQueue(protocol::controlInputQueue, 10, {}, 512)
                .action == uml::virtio::VhostUserQueueAction::retry);

    auto invalidChannel = std::make_unique<TestChannel>();
    requireThrows([&] {
        uml::console::VirtioConsoleDevice invalid(
            std::move(invalidChannel), "invalid/port");
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

/*
 * The harness accepts the artifact socket only when it first observes mode
 * 0600.  Creating the file under the process umask and narrowing it after
 * listen() exposed a wider mode for the bind-to-chmod window, which aborted
 * guest launches on a loaded host.
 */
void unixSocketChannelNeverExposesAWiderModeWhileListening()
{
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "console.sock";
    const mode_t previous = ::umask(022);
    ::umask(previous);

    std::atomic<bool> stop {false};
    std::atomic<int> widerObservations {0};
    std::atomic<int> observations {0};
    std::thread watcher([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            struct stat status {};
            if (::lstat(path.c_str(), &status) == 0) {
                observations.fetch_add(1, std::memory_order_relaxed);
                if ((status.st_mode & 0777) != 0600) {
                    widerObservations.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });
    {
        uml::console::UnixSocketChannel channel(path);
        struct stat status {};
        require(::lstat(path.c_str(), &status) == 0);
        require((status.st_mode & 0777) == 0600);
        stop.store(true, std::memory_order_relaxed);
        watcher.join();
    }
    require(observations.load() > 0);
    require(widerObservations.load() == 0);
    const mode_t restored = ::umask(previous);
    ::umask(previous);
    require(restored == previous);
}

void unixSocketChannelRejectsAClientThatClosedBeforeAccept()
{
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "console.sock";
    uml::console::UnixSocketChannel channel(path);

    /*
     * Darwin rejects F_SETNOSIGPIPE with EINVAL on a socket whose peer already
     * closed, which a client that connects and exits immediately produces
     * routinely.  Treating that as a channel error escapes into the vhost-user
     * connection loop and takes the console device down for the rest of the
     * run, so the accept has to drop just that client.
     */
    connectClient(path).reset();
    waitReadable(channel.eventDescriptor());
    channel.drainEventSource();

    auto client = connectClient(path);
    waitReadable(channel.eventDescriptor());
    channel.drainEventSource();
    constexpr std::array<std::uint8_t, 3> reply {'o', 'k', '\n'};
    channel.write(reply);
    waitReadable(client.get());
    std::array<std::uint8_t, 16> received {};
    require(::recv(client.get(), received.data(), received.size(), 0) ==
            static_cast<ssize_t>(reply.size()));
    require(std::equal(reply.begin(), reply.end(), received.begin()));
}

void connectedSocketChannelIsNonblocking()
{
    std::array<int, 2> descriptors {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) == 0);
    uml::gpu::UniqueDescriptor peer {descriptors[1]};
    uml::console::ConnectedSocketChannel channel(descriptors[0]);

    const int flags = ::fcntl(channel.eventDescriptor(), F_GETFL);
    require(flags >= 0 && (flags & O_NONBLOCK) != 0);
    require(channel.read(16).empty());

    constexpr std::array<std::uint8_t, 5> input {'i', 'n', 'p', 'u', 't'};
    require(::send(peer.get(), input.data(), input.size(), 0) ==
            static_cast<ssize_t>(input.size()));
    require(channel.read(input.size()) ==
            std::vector<std::uint8_t>(input.begin(), input.end()));

    constexpr std::array<std::uint8_t, 6> output {'o', 'u', 't', 'p', 'u', 't'};
    channel.write(output);
    waitReadable(peer.get());
    std::array<std::uint8_t, 16> received {};
    require(::recv(peer.get(), received.data(), received.size(), 0) ==
            static_cast<ssize_t>(output.size()));
    require(std::equal(output.begin(), output.end(), received.begin()));
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

    const auto serialPath = temporary.path() / "serial.sock";
    uml::console::InProcessConsoleService serialService(
        serialPath, std::string(protocol::hostSerialPortName));
    require(
        ::lstat(serialPath.c_str(), &status) == 0 && S_ISSOCK(status.st_mode));
    const auto serialConnection = serialService.takeConnectionName();
    require(serialConnection.starts_with("fd="));
    (void)::close(std::stoi(serialConnection.substr(3)));
    serialService.stop();
    require(::lstat(serialPath.c_str(), &status) != 0 && errno == ENOENT);
}

} // namespace

int main()
{
    deviceImplementsTheTwoQueueConsoleContract();
    deviceImplementsTheNamedSerialControlHandshake();
    unixSocketChannelSupportsTailInputAndReconnect();
    unixSocketChannelNeverExposesAWiderModeWhileListening();
    unixSocketChannelRejectsAClientThatClosedBeforeAccept();
    connectedSocketChannelIsNonblocking();
    inProcessServicePublishesAndRemovesTheExternalSocket();
    return EXIT_SUCCESS;
}
