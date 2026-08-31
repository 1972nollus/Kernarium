// SPDX-License-Identifier: GPL-2.0-or-later

#include "display_geometry.hpp"
#include "display_resize_notifier.hpp"
#include "scanout_framebuffer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

void aspectFitCentersGuestContentWithoutDistortion()
{
    using uml::gpu::aspectFitRectangle;
    const auto landscape = aspectFitRectangle(1000, 1000, 1600, 900);
    require(std::abs(landscape.x) < 0.001,
        "landscape aspect fit was not horizontally flush");
    require(std::abs(landscape.y - 218.75) < 0.001,
        "landscape aspect fit did not center its letterbox bars");
    require(std::abs(landscape.width - 1000.0) < 0.001 &&
            std::abs(landscape.height - 562.5) < 0.001,
        "landscape aspect fit changed the guest aspect ratio");

    const auto portrait = aspectFitRectangle(1200, 600, 600, 1200);
    require(std::abs(portrait.x - 450.0) < 0.001 &&
            std::abs(portrait.y) < 0.001 &&
            std::abs(portrait.width - 300.0) < 0.001 &&
            std::abs(portrait.height - 600.0) < 0.001,
        "portrait aspect fit did not center its pillarbox bars");

    require(aspectFitRectangle(0, 100, 100, 100).width == 0,
        "invalid host geometry produced an aspect-fit viewport");
    require(aspectFitRectangle(100, 100, 0, 100).height == 0,
        "invalid guest geometry produced an aspect-fit viewport");
}

void pointerPathsClipToExactGuestEdges()
{
    using uml::gpu::AspectFitRectangle;
    using uml::gpu::DisplayPoint;
    using uml::gpu::clipDisplaySegment;
    const AspectFitRectangle display {
        .x = 10.0,
        .y = 20.0,
        .width = 100.0,
        .height = 80.0,
    };

    auto clipped = clipDisplaySegment(
        display, DisplayPoint {-10.0, 30.0}, DisplayPoint {40.0, 80.0});
    require(clipped.has_value() &&
            std::abs(clipped->entry.x - 10.0) < 0.001 &&
            std::abs(clipped->entry.y - 50.0) < 0.001 &&
            std::abs(clipped->exit.x - 40.0) < 0.001 &&
            std::abs(clipped->exit.y - 80.0) < 0.001,
        "an entering pointer path did not start at the guest edge");

    clipped = clipDisplaySegment(
        display, DisplayPoint {60.0, 60.0}, DisplayPoint {160.0, 110.0});
    require(clipped.has_value() &&
            std::abs(clipped->entry.x - 60.0) < 0.001 &&
            std::abs(clipped->entry.y - 60.0) < 0.001 &&
            std::abs(clipped->exit.x - 110.0) < 0.001 &&
            std::abs(clipped->exit.y - 85.0) < 0.001,
        "an exiting pointer path did not end at the guest edge");

    clipped = clipDisplaySegment(
        display, DisplayPoint {-10.0, 60.0}, DisplayPoint {130.0, 60.0});
    require(clipped.has_value() &&
            std::abs(clipped->entry.x - 10.0) < 0.001 &&
            std::abs(clipped->exit.x - 110.0) < 0.001,
        "a crossing pointer path did not retain both guest edges");
    require(!clipDisplaySegment(
                 display,
                 DisplayPoint {-10.0, 10.0},
                 DisplayPoint {130.0, 10.0})
                 .has_value(),
        "a pointer path outside the guest unexpectedly intersected it");
    require(!clipDisplaySegment(
                 display,
                 DisplayPoint {0.0, 30.0},
                 DisplayPoint {20.0, 10.0})
                 .has_value(),
        "a path tangent to a guest corner became a guest contact");
}

void guestModesetLatchesHostResizeUntilReset()
{
    using uml::gpu::DisplayGeometryPolicy;
    const auto requireSize = [](uml::gpu::DisplaySize size,
                                 std::uint32_t width,
                                 std::uint32_t height,
                                 const char* message) {
        require(size.width == width && size.height == height, message);
    };

    DisplayGeometryPolicy policy {800, 600, true};
    require(policy.configuredAutoResizeGuest(),
        "enabled auto resize lost its configured state");
    require(policy.autoResizeGuestEnabled(),
        "enabled auto resize did not start active");
    auto change = policy.guestScanoutChanged(800, 600);
    require(!change.changed && !change.independent,
        "the initial scanout was classified as a guest modeset");

    require(policy.hostSizeSettled(1000, 700),
        "the first settled host resize was not advertised");
    require(policy.hostSizeSettled(1200, 800),
        "the second settled host resize was not advertised");
    requireSize(policy.advertisedSize(), 1200, 800,
        "the newest settled host geometry was not advertised");

    change = policy.guestScanoutChanged(1000, 700);
    require(change.changed && !change.independent &&
            policy.autoResizeGuestEnabled(),
        "a delayed response to an earlier host target disabled auto resize");
    change = policy.guestScanoutChanged(1200, 800);
    require(change.changed && !change.independent &&
            policy.autoResizeGuestEnabled(),
        "the newest host resize response disabled auto resize");

    change = policy.guestScanoutChanged(1024, 768);
    require(change.changed && change.independent,
        "an independent guest modeset was not identified");
    require(!policy.autoResizeGuestEnabled(),
        "an independent guest modeset did not latch host resize off");
    requireSize(policy.activeGuestSize(), 1024, 768,
        "the guest modeset did not publish active presentation geometry");
    requireSize(policy.advertisedSize(), 1200, 800,
        "the guest modeset overwrote the host-advertised geometry");

    require(!policy.hostSizeSettled(1440, 900),
        "a host resize escaped while guest authority was latched");
    change = policy.guestScanoutChanged(1280, 720);
    require(change.changed && change.independent,
        "a later guest modeset lost guest-authoritative classification");
    policy.resetGuestSession({1440, 900});
    require(policy.autoResizeGuestEnabled(),
        "guest reset did not restore the configured auto-resize policy");
    requireSize(policy.advertisedSize(), 1440, 900,
        "guest reset did not seed the latest settled host geometry");
    requireSize(policy.activeGuestSize(), 1440, 900,
        "guest reset retained stale presentation geometry");
    change = policy.guestScanoutChanged(1280, 720);
    require(change.changed && !change.independent &&
            policy.autoResizeGuestEnabled(),
        "the first scanout after reset was treated as an active modeset");
}

void configuredOffAlwaysKeepsHostResizeSilent()
{
    uml::gpu::DisplayGeometryPolicy policy {800, 600, false};
    require(!policy.configuredAutoResizeGuest() &&
            !policy.autoResizeGuestEnabled(),
        "disabled auto resize did not preserve its configured state");
    auto change = policy.guestScanoutChanged(640, 480);
    require(change.changed && !change.independent,
        "the initial configured-off scanout was treated as a modeset");
    require(!policy.hostSizeSettled(1000, 800),
        "configured-off host geometry produced a guest notification");
    require(policy.advertisedSize() == uml::gpu::DisplaySize {800, 600},
        "configured-off host resize changed the running guest mode");
    change = policy.guestScanoutChanged(1024, 768);
    require(change.changed && change.independent,
        "a configured-off guest modeset was not classified independently");
    policy.resetGuestSession({1000, 800});
    require(!policy.autoResizeGuestEnabled(),
        "guest reset enabled a configured-off policy");
    require(policy.advertisedSize() == uml::gpu::DisplaySize {1000, 800} &&
            policy.activeGuestSize() == uml::gpu::DisplaySize {1000, 800},
        "guest reset did not adopt the current host boot geometry");
}

void hostTargetCorrelationHandlesBootRollbackAndDuplicates()
{
    using uml::gpu::DisplayGeometryPolicy;

    DisplayGeometryPolicy boot {800, 600, true};
    auto change = boot.guestScanoutChanged(1024, 768);
    require(change.changed && !change.independent,
        "the first temporary boot scanout was classified independently");
    change = boot.guestScanoutChanged(800, 600);
    require(change.changed && !change.independent &&
            boot.autoResizeGuestEnabled(),
        "adopting the initially advertised mode disabled auto resize");

    DisplayGeometryPolicy rollback {800, 600, true};
    (void)rollback.guestScanoutChanged(800, 600);
    require(rollback.hostSizeSettled(1000, 700),
        "the intermediate host target was not advertised");
    require(!rollback.hostSizeSettled(800, 600),
        "a host rollback to the active mode requested a redundant modeset");
    require(rollback.advertisedSize() == uml::gpu::DisplaySize {800, 600},
        "the silent host rollback did not update advertised geometry");
    change = rollback.guestScanoutChanged(1000, 700);
    require(change.changed && change.independent &&
            !rollback.autoResizeGuestEnabled(),
        "a stale rolled-back host target masked a guest modeset");

    DisplayGeometryPolicy duplicates {800, 600, true};
    (void)duplicates.guestScanoutChanged(800, 600);
    require(duplicates.hostSizeSettled(1000, 700) &&
            duplicates.hostSizeSettled(1200, 800) &&
            duplicates.hostSizeSettled(1000, 700),
        "duplicate host targets were not advertised");
    change = duplicates.guestScanoutChanged(1000, 700);
    require(change.changed && !change.independent,
        "the first duplicate host response was misclassified");
    change = duplicates.guestScanoutChanged(1200, 800);
    require(change.changed && !change.independent &&
            duplicates.autoResizeGuestEnabled(),
        "a response between duplicate host targets disabled auto resize");
}

} // namespace

int main()
{
    try {
        resizeNotificationsAreCoalescedAndBounded();
        framebufferResizePublishesOneClearedGeometry();
        aspectFitCentersGuestContentWithoutDistortion();
        pointerPathsClipToExactGuestEdges();
        guestModesetLatchesHostResizeUntilReset();
        configuredOffAlwaysKeepsHostResizeSilent();
        hostTargetCorrelationHandlesBootRollbackAndDuplicates();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "GPU display resize test failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
