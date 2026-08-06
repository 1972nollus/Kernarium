// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"
#include "in_process_service.hpp"
#include "vhost_user_message_testing.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <sys/wait.h>

namespace uml::gpu::testing {
namespace {

[[noreturn]] void runPartialFrameChild(bool partialPayload)
{
    resetStopWaitObserver();
    InProcessGpuOptions options;
    options.vncPort = 0;
    options.width = 64;
    options.height = 64;
    InProcessGpuService service {std::move(options)};
    const int transport = descriptorFromName(service.takeConnectionName());

    const protocol::VhostHeader header {
        .request = static_cast<std::uint32_t>(
            protocol::VhostRequest::setFeatures),
        .flags = protocol::vhostUserVersion,
        .size = partialPayload ?
            static_cast<std::uint32_t>(sizeof(std::uint64_t)) : 0U,
    };
    if (partialPayload) {
        std::array<
            std::uint8_t,
            sizeof(header) + sizeof(std::uint32_t)> fragment {};
        std::memcpy(fragment.data(), &header, sizeof(header));
        require(
            ::write(transport, fragment.data(), fragment.size()) ==
            static_cast<ssize_t>(fragment.size()));
    } else {
        require(
            ::write(transport, &header, sizeof(header) / 2U) ==
            static_cast<ssize_t>(sizeof(header) / 2U));
    }

    require(waitForStopWaitCount(1, 1000));
    service.stop();
    require(::close(transport) == 0);
    std::_Exit(EXIT_SUCCESS);
}

void partialFrameUsesWake(bool partialPayload)
{
    const auto before = descriptorCount();
    const pid_t child = ::fork();
    require(child >= 0);
    if (child == 0)
        runPartialFrameChild(partialPayload);

    // The linked test-only transport polls every five seconds. The child can
    // finish inside this two-second watchdog only if InProcessGpuService::stop
    // interrupts the blocked endpoint through its separately owned duplicate.
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds {2};
    int status = 0;
    for (;;) {
        const auto result = ::waitpid(child, &status, WNOHANG);
        if (result < 0 && errno == EINTR)
            continue;
        require(result >= 0);
        if (result == child)
            break;
        if (std::chrono::steady_clock::now() >= deadline) {
            require(::kill(child, SIGKILL) == 0);
            require(::waitpid(child, &status, 0) == child);
            require(false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
    }
    require(WIFEXITED(status));
    require(WEXITSTATUS(status) == EXIT_SUCCESS);
    require(descriptorCount() == before);
}

} // namespace
} // namespace uml::gpu::testing

int main()
{
    uml::gpu::testing::partialFrameUsesWake(false);
    uml::gpu::testing::partialFrameUsesWake(true);
    std::puts("GPU in-process stop wake tests passed");
    return 0;
}
