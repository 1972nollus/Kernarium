// SPDX-License-Identifier: GPL-2.0-or-later

#include "vnc_display.hpp"
#include "vnc_display_testing.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <source_location>
#include <stdexcept>

namespace {

void require(
    const bool condition,
    const std::source_location location = std::source_location::current())
{
    if (condition)
        return;
    std::fprintf(
        stderr,
        "require failed at %s:%u\n",
        location.file_name(),
        location.line());
    std::abort();
}

[[nodiscard]] std::size_t descriptorCount()
{
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator {"/dev/fd"}) {
        (void)entry;
        ++count;
    }
    return count;
}

void postListenThreadFailureReleasesServer()
{
    // Initialize process-lifetime library state before measuring ownership.
    {
        uml::gpu::VncDisplay warm {"127.0.0.1", 0, {}, 64, 64};
    }

    for (unsigned iteration = 0; iteration < 16; ++iteration) {
        const auto before = descriptorCount();
        uml::gpu::testing::failNextEventThreadStart();
        bool failed = false;
        try {
            uml::gpu::VncDisplay display {"127.0.0.1", 0, {}, 64, 64};
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed);
        const auto listenedPort =
            uml::gpu::testing::failedEventThreadPort();
        require(listenedPort != 0);
        require(descriptorCount() == before);
        {
            uml::gpu::VncDisplay retry {
                "127.0.0.1", listenedPort, {}, 64, 64};
            require(retry.port() == listenedPort);
        }
        require(descriptorCount() == before);
    }
}

void dimensionsMustFitLibVncServerAbi()
{
    const auto before = descriptorCount();
    bool rejected = false;
    try {
        uml::gpu::VncDisplay display {
            "127.0.0.1",
            0,
            {},
            static_cast<std::uint32_t>(
                std::numeric_limits<int>::max()) + 1U,
            1};
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected);
    require(descriptorCount() == before);
}

} // namespace

int main()
{
    dimensionsMustFitLibVncServerAbi();
    postListenThreadFailureReleasesServer();
    std::puts("VNC display lifetime tests passed");
    return 0;
}
