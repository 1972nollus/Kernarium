// SPDX-License-Identifier: GPL-2.0-or-later
/* Test support: a Virtio balloon backend that raises a config change every
 * few milliseconds. Each change is a request on the vhost-user backend
 * channel that waits for the guest's acknowledgement, and each one makes the
 * guest's balloon worker read the config again over the main socket, so the
 * two cross constantly.
 *
 * The target stays at zero, so the guest never inflates: this process cannot
 * serve a virtqueue. The guest RAM the emulator names in SET_MEM_TABLE is an
 * identity token that only the emulator process resolves
 * (darwin/runtime/bridge.cpp), so a backend in another process maps zeros and
 * would never see an inflate request. Config reads and writes travel in
 * messages and need no guest memory.
 *
 *   uml-balloon-churn-server --fd N [--period-ms 2]
 *
 * It exits when the guest closes the connection and prints one summary line.
 */

#include "balloon/protocol.hpp"
#include "unique_descriptor.hpp"
#include "vhost_user_device.hpp"
#include "vhost_user_server.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sys/event.h>

namespace {

namespace protocol = uml::balloon::protocol;

std::atomic_bool stopRequested {};

void requestStop(int) noexcept
{
    stopRequested.store(true, std::memory_order_release);
}

struct Counts final {
    std::uint64_t changes {};
    std::uint64_t reads {};
};

/* A balloon whose target never moves but which reports a config change on
 * every timer tick, as a host whose target estimate keeps being revised. */
class ChurningBalloon final : public uml::virtio::VhostUserDevice {
public:
    ChurningBalloon(std::uint64_t periodMilliseconds, Counts& counts)
        : counts_(counts)
        , timer_(::kqueue())
    {
        if (timer_.get() < 0)
            throw std::runtime_error(
                "kqueue failed: " + std::string(std::strerror(errno)));
        struct kevent timer {};
        EV_SET(&timer, 1, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0,
            static_cast<std::intptr_t>(periodMilliseconds), nullptr);
        if (::kevent(timer_.get(), &timer, 1, nullptr, 0, nullptr) != 0)
            throw std::runtime_error(
                "arming the timer failed: " +
                std::string(std::strerror(errno)));
    }

    [[nodiscard]] const char* name() const noexcept override
    {
        return "Churning Virtio balloon";
    }

    [[nodiscard]] std::uint64_t features() const noexcept override
    {
        return protocol::featureMustTellHost;
    }

    [[nodiscard]] std::size_t queueCount() const noexcept override
    {
        return protocol::queueCount;
    }

    [[nodiscard]] std::vector<std::uint8_t> configuration() const override
    {
        const protocol::Configuration configuration {
            .numPages = 0,
            .actual = actual_,
            .freePageHintCommandId = 0,
            .poisonValue = 0,
        };
        std::vector<std::uint8_t> bytes(sizeof(configuration));
        std::memcpy(bytes.data(), &configuration, sizeof(configuration));
        return bytes;
    }

    void configurationRead(std::uint32_t, std::uint32_t) noexcept override
    {
        ++counts_.reads;
    }

    void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t,
        std::span<const std::uint8_t> bytes) override
    {
        if (offset == protocol::actualOffset &&
            bytes.size() == sizeof(actual_))
            std::memcpy(&actual_, bytes.data(), sizeof(actual_));
    }

    /* The guest never inflates a balloon whose target is zero. */
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t,
        std::uint64_t,
        std::span<const std::uint8_t>,
        std::size_t) override
    {
        return {.action = uml::virtio::VhostUserQueueAction::complete};
    }

    void reset() noexcept override
    {
        actual_ = 0;
        pending_ = false;
    }

    [[nodiscard]] int eventDescriptor() const noexcept override
    {
        return timer_.get();
    }

    void drainEventSource() override
    {
        std::array<struct kevent, 4> events {};
        const timespec immediately {};
        int count;
        do {
            count = ::kevent(timer_.get(), nullptr, 0, events.data(),
                static_cast<int>(events.size()), &immediately);
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            throw std::runtime_error("draining the timer failed");
        if (count > 0 && !pending_) {
            pending_ = true;
            ++counts_.changes;
        }
    }

    [[nodiscard]] bool configChangePending() const noexcept override
    {
        return pending_;
    }

    void clearConfigChange() noexcept override
    {
        pending_ = false;
    }

private:
    Counts& counts_;
    uml::gpu::UniqueDescriptor timer_;
    std::uint32_t actual_ {};
    bool pending_ {};
};

[[noreturn]] void usage()
{
    std::fputs(
        "usage: uml-balloon-churn-server --fd N [--period-ms MS]\n", stderr);
    std::exit(2);
}

std::uint64_t number(const char* text)
{
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0')
        usage();
    return value;
}

} // namespace

int main(int argc, char** argv)
{
    int descriptor = -1;
    std::uint64_t periodMilliseconds = 2;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option {argv[index]};
        if (index + 1 >= argc)
            usage();
        const char* value = argv[++index];
        if (option == "--fd")
            descriptor = static_cast<int>(number(value));
        else if (option == "--period-ms")
            periodMilliseconds = number(value);
        else
            usage();
    }
    if (descriptor < 0 || periodMilliseconds == 0)
        usage();

    struct sigaction action {};
    action.sa_handler = requestStop;
    sigemptyset(&action.sa_mask);
    (void)::sigaction(SIGTERM, &action, nullptr);
    (void)::sigaction(SIGINT, &action, nullptr);

    Counts counts;
    try {
        uml::gpu::VhostUserServer server {
            std::make_unique<ChurningBalloon>(periodMilliseconds, counts)};
        server.runConnected(descriptor, stopRequested);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "uml-balloon-churn-server: %s\n", error.what());
        return 1;
    }
    std::printf("balloon churn: %llu config changes raised, %llu config "
                "reads answered\n",
        static_cast<unsigned long long>(counts.changes),
        static_cast<unsigned long long>(counts.reads));
    return 0;
}
