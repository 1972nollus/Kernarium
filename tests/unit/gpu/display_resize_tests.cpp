// SPDX-License-Identifier: GPL-2.0-or-later

#include "display_resize_notifier.hpp"
#include "scanout_framebuffer.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <poll.h>
#include <stdexcept>

namespace {

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void resizeNotificationsAreCoalescedAndBounded()
{
    using uml::gpu::DisplayResizeNotifier;
    DisplayResizeNotifier initiallyBounded {
        9000, 1, uml::gpu::protocol::maximumEdidDimension};
    const auto initialSize = initiallyBounded.drain();
    require(
        initialSize.width == uml::gpu::protocol::maximumEdidDimension &&
            initialSize.height == DisplayResizeNotifier::minimumDimension,
        "initial display geometry did not enforce the guest mode bounds");

    DisplayResizeNotifier notifier {
        640, 480, uml::gpu::protocol::maximumEdidDimension};
    pollfd readiness {.fd = notifier.descriptor(), .events = POLLIN};

    notifier.request(640, 480);
    notifier.request(0, 480);
    require(::poll(&readiness, 1, 0) == 0,
        "duplicate or zero display geometry produced a resize event");

    notifier.request(800, 600);
    notifier.request(1024, 768);
    require(::poll(&readiness, 1, 1000) == 1,
        "display resize did not wake its GPU-thread consumer");
    auto size = notifier.drain();
    require(size.width == 1024 && size.height == 768,
        "display resize did not coalesce to the newest geometry");

    notifier.request(9000, 1);
    require(::poll(&readiness, 1, 1000) == 1,
        "bounded display resize did not produce an event");
    size = notifier.drain();
    require(
        size.width == uml::gpu::protocol::maximumEdidDimension &&
            size.height == DisplayResizeNotifier::minimumDimension,
        "display resize did not enforce the guest mode bounds");

    notifier.requestCommitted(size.width, size.height);
    require(::poll(&readiness, 1, 1000) == 1,
        "a settled resize could not rearm a previously suppressed geometry");
    size = notifier.drain();
    require(size.width == uml::gpu::protocol::maximumEdidDimension &&
            size.height == DisplayResizeNotifier::minimumDimension,
        "rearmed resize changed the settled geometry");
}

void framebufferResizePublishesOneClearedGeometry()
{
    uml::gpu::ScanoutFramebuffer framebuffer {64, 48};
    framebuffer.fillSelfTestPattern();
    framebuffer.resize(96, 72);
    require(framebuffer.width() == 96 && framebuffer.height() == 72,
        "framebuffer resize did not publish its new dimensions");
    require(
        framebuffer.rowBytes() ==
            96U * uml::gpu::protocol::gpuBytesPerPixel,
        "framebuffer resize retained the old row stride");
    {
        std::lock_guard lock(framebuffer);
        require(std::ranges::all_of(
                    framebuffer.pixels(),
                    [](std::uint8_t value) { return value == 0; }),
            "framebuffer resize exposed pixels from the old geometry");
    }

    const std::array<std::uint8_t, 4> bluePixel {0xff, 0, 0, 0xff};
    const auto changed = framebuffer.update(
        bluePixel,
        1,
        1,
        bluePixel.size(),
        uml::gpu::protocol::GpuFormat::b8g8r8a8Unorm,
        0,
        0,
        95,
        71,
        1,
        1);
    require(changed.width == 1 && changed.height == 1,
        "resized framebuffer rejected its new lower-right pixel");
}

} // namespace

int main()
{
    try {
        resizeNotificationsAreCoalescedAndBounded();
        framebufferResizePublishesOneClearedGeometry();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "GPU display resize test failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
