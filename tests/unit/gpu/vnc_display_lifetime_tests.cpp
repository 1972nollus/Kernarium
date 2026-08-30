// SPDX-License-Identifier: GPL-2.0-or-later

#include "scanout_framebuffer.hpp"
#include "vnc_display.hpp"
#include "vnc_display_testing.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
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

void packedBgraUpdatesRespectOffsetsAndPaddedStride()
{
    constexpr std::uint32_t sourceWidth = 5;
    constexpr std::uint32_t sourceHeight = 4;
    constexpr std::size_t sourceRowBytes = 24;
    std::array<std::uint8_t, sourceRowBytes * sourceHeight> source {};
    std::fill(source.begin(), source.end(), 0xeeU);
    for (std::uint32_t y = 0; y < sourceHeight; ++y) {
        for (std::uint32_t x = 0; x < sourceWidth; ++x) {
            const auto offset =
                static_cast<std::size_t>(y) * sourceRowBytes +
                static_cast<std::size_t>(x) *
                    uml::gpu::protocol::gpuBytesPerPixel;
            source[offset] = static_cast<std::uint8_t>(0x10U + x);
            source[offset + 1U] = static_cast<std::uint8_t>(0x20U + y);
            source[offset + 2U] =
                static_cast<std::uint8_t>(0x30U + x + y);
            source[offset + 3U] = static_cast<std::uint8_t>(0x40U + x);
        }
    }

    uml::gpu::ScanoutFramebuffer framebuffer {4, 3};
    for (const auto format : {
             uml::gpu::protocol::GpuFormat::b8g8r8a8Unorm,
             uml::gpu::protocol::GpuFormat::b8g8r8x8Unorm}) {
        framebuffer.clear();
        const auto rectangle = framebuffer.update(
            source,
            sourceWidth,
            sourceHeight,
            sourceRowBytes,
            format,
            1,
            1,
            2,
            1,
            2,
            2);
        require(rectangle.x == 2);
        require(rectangle.y == 1);
        require(rectangle.width == 2);
        require(rectangle.height == 2);

        std::lock_guard lock(framebuffer);
        const auto pixels = framebuffer.pixels();
        for (std::uint32_t y = 0; y < framebuffer.height(); ++y) {
            for (std::uint32_t x = 0; x < framebuffer.width(); ++x) {
                const auto destinationOffset =
                    (static_cast<std::size_t>(y) * framebuffer.width() + x) *
                    uml::gpu::protocol::gpuBytesPerPixel;
                if (x < 2 || y < 1) {
                    for (std::size_t byte = 0;
                         byte < uml::gpu::protocol::gpuBytesPerPixel;
                         ++byte) {
                        require(pixels[destinationOffset + byte] == 0);
                    }
                    continue;
                }
                const auto sourceX = 1U + x - 2U;
                const auto sourceY = 1U + y - 1U;
                const auto sourceOffset =
                    static_cast<std::size_t>(sourceY) * sourceRowBytes +
                    static_cast<std::size_t>(sourceX) *
                        uml::gpu::protocol::gpuBytesPerPixel;
                require(std::equal(
                    source.begin() + sourceOffset,
                    source.begin() + sourceOffset +
                        uml::gpu::protocol::gpuBytesPerPixel,
                    pixels.begin() + destinationOffset));
            }
        }
    }
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
    packedBgraUpdatesRespectOffsetsAndPaddedStride();
    dimensionsMustFitLibVncServerAbi();
    postListenThreadFailureReleasesServer();
    std::puts("VNC display lifetime tests passed");
    return 0;
}
