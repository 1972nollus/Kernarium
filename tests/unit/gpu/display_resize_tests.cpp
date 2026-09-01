// SPDX-License-Identifier: GPL-2.0-or-later

#include "display_geometry.hpp"
#include "display_resize_notifier.hpp"
#include "scanout_framebuffer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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
    {
        std::lock_guard lock(framebuffer);
        const auto pixels = framebuffer.pixels();
        require(std::equal(
                    bluePixel.begin(),
                    bluePixel.end(),
                    pixels.end() -
                        static_cast<std::ptrdiff_t>(bluePixel.size())),
            "packed update used the pre-resize framebuffer stride");
    }
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

void hostRotationSwapsPresentationAndMapsGuestInput()
{
    using uml::gpu::DisplayPoint;
    using uml::gpu::DisplayRotation;
    using uml::gpu::DisplaySize;
    using uml::gpu::guestPointFromPresentedPoint;
    using uml::gpu::presentedDisplaySize;
    using uml::gpu::rotateDisplay;

    const DisplaySize guest {1600, 900};
    require(
        presentedDisplaySize(guest, DisplayRotation::upright) == guest &&
            presentedDisplaySize(guest, DisplayRotation::upsideDown) == guest,
        "a half-turn changed the presented display dimensions");
    const DisplaySize portrait {900, 1600};
    require(
        presentedDisplaySize(guest, DisplayRotation::clockwise90) == portrait &&
            presentedDisplaySize(
                guest, DisplayRotation::counterclockwise90) == portrait,
        "a quarter-turn did not swap the presented display dimensions");
    require(
        presentedDisplaySize(portrait, DisplayRotation::clockwise90) == guest,
        "a rotated host size did not map back to guest orientation");
    uml::gpu::DisplayGeometryPolicy policy {
        guest.width, guest.height, true};
    const auto guestOrientedWindow = presentedDisplaySize(
        portrait, DisplayRotation::clockwise90);
    require(
        !policy.hostSizeSettled(
            guestOrientedWindow.width, guestOrientedWindow.height) &&
            policy.advertisedSize() == guest &&
            policy.activeGuestSize() == guest,
        "rotating the host window produced a guest resolution change");

    require(
        rotateDisplay(DisplayRotation::upright, 1) ==
                DisplayRotation::clockwise90 &&
            rotateDisplay(DisplayRotation::upright, -1) ==
                DisplayRotation::counterclockwise90 &&
            rotateDisplay(DisplayRotation::counterclockwise90, 2) ==
                DisplayRotation::clockwise90 &&
            rotateDisplay(DisplayRotation::upright, 5) ==
                DisplayRotation::clockwise90,
        "display quarter-turn composition was incorrect");

    const auto requirePoint = [](DisplayPoint actual,
                                  double x,
                                  double y,
                                  const char* message) {
        require(
            std::abs(actual.x - x) < 0.001 &&
                std::abs(actual.y - y) < 0.001,
            message);
    };
    requirePoint(
        guestPointFromPresentedPoint(
            {0.0, 0.0}, DisplayRotation::clockwise90),
        0.0,
        1.0,
        "clockwise presentation top-left did not map to guest bottom-left");
    requirePoint(
        guestPointFromPresentedPoint(
            {1.0, 0.0}, DisplayRotation::clockwise90),
        0.0,
        0.0,
        "clockwise presentation top-right did not map to guest top-left");
    requirePoint(
        guestPointFromPresentedPoint(
            {0.0, 0.0}, DisplayRotation::counterclockwise90),
        1.0,
        0.0,
        "counterclockwise presentation top-left did not map to guest top-right");
    requirePoint(
        guestPointFromPresentedPoint(
            {0.25, 0.75}, DisplayRotation::upsideDown),
        0.75,
        0.25,
        "half-turn input did not map back to guest coordinates");
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

void guestModesetsNeverDisableHostResize()
{
    using uml::gpu::DisplayGeometryPolicy;
    const auto requireSize = [](uml::gpu::DisplaySize size,
                                 std::uint32_t width,
                                 std::uint32_t height,
                                 const char* message) {
        require(size.width == width && size.height == height, message);
    };

    DisplayGeometryPolicy policy {800, 600, true};
    require(policy.autoResizeGuestEnabled(),
        "configured auto resize did not start enabled");
    require(!policy.guestScanoutChanged(800, 600),
        "an unchanged initial scanout changed presentation geometry");

    require(policy.hostSizeSettled(1000, 700),
        "a settled host resize was not advertised");
    require(!policy.hostSizeSettled(1000, 700),
        "one settled host size requested the same guest resize twice");
    require(policy.guestScanoutChanged(996, 700),
        "the guest's rounded host response was ignored");
    requireSize(policy.activeGuestSize(), 996, 700,
        "rounded guest geometry was not published for presentation");
    require(!policy.hostSizeSettled(996, 700),
        "following the applied guest size fed a resize back to the guest");
    requireSize(policy.advertisedSize(), 996, 700,
        "following the guest did not synchronize advertised geometry");

    require(policy.guestScanoutChanged(1024, 768),
        "a guest-initiated modeset was ignored");
    require(policy.autoResizeGuestEnabled(),
        "a guest-initiated modeset disabled later host resizing");
    requireSize(policy.activeGuestSize(), 1024, 768,
        "the guest modeset did not publish active presentation geometry");

    require(policy.hostSizeSettled(1200, 800),
        "host resize stopped after an unrelated guest mode");
    require(policy.guestScanoutChanged(1280, 720),
        "a nonmatching eventual guest size was ignored");
    requireSize(policy.activeGuestSize(), 1280, 720,
        "presentation rejected a nonmatching eventual guest size");
    require(!policy.hostSizeSettled(1280, 720),
        "following a nonmatching guest result produced resize feedback");

    require(policy.hostSizeSettled(1440, 900),
        "host resize stopped after a guest-initiated modeset");
    require(policy.guestScanoutChanged(1432, 900),
        "the eventual guest size after a new host resize was ignored");
    policy.resetGuestSession({1440, 900});
    require(policy.autoResizeGuestEnabled(),
        "guest reset changed the configured auto-resize policy");
    requireSize(policy.advertisedSize(), 1440, 900,
        "guest reset did not seed the latest settled host geometry");
    requireSize(policy.activeGuestSize(), 1440, 900,
        "guest reset retained stale presentation geometry");
    require(policy.guestScanoutChanged(1280, 720),
        "the first changed scanout after reset was ignored");
}

void configuredOffAlwaysKeepsHostResizeSilent()
{
    uml::gpu::DisplayGeometryPolicy policy {800, 600, false};
    require(!policy.autoResizeGuestEnabled(),
        "configured-off auto resize started enabled");
    require(policy.guestScanoutChanged(640, 480),
        "configured-off guest geometry was not published");
    require(!policy.hostSizeSettled(1000, 800),
        "configured-off host geometry produced a guest notification");
    require(policy.advertisedSize() == uml::gpu::DisplaySize {800, 600},
        "configured-off host resize changed the running guest mode");
    require(policy.guestScanoutChanged(1024, 768),
        "a later configured-off guest modeset was ignored");
    require(policy.activeGuestSize() == uml::gpu::DisplaySize {1024, 768},
        "configured-off presentation did not follow the guest");
    policy.resetGuestSession({1000, 800});
    require(!policy.autoResizeGuestEnabled(),
        "guest reset enabled a configured-off policy");
    require(policy.advertisedSize() == uml::gpu::DisplaySize {1000, 800} &&
            policy.activeGuestSize() == uml::gpu::DisplaySize {1000, 800},
        "guest reset did not adopt the current host boot geometry");
}

void repeatedSettledResizesRequestOnceAndFollowEveryGuestResult()
{
    using uml::gpu::DisplayGeometryPolicy;

    DisplayGeometryPolicy policy {1280, 720, true};
    (void)policy.guestScanoutChanged(1280, 720);
    for (std::uint32_t index = 0; index < 32; ++index) {
        const uml::gpu::DisplaySize requested {
            .width = 900U + index * 37U,
            .height = 600U + (index % 7U) * 29U,
        };
        const uml::gpu::DisplaySize applied {
            .width = requested.width - (index % 3U) * 4U,
            .height = requested.height - (index % 2U) * 2U,
        };
        require(policy.hostSizeSettled(requested.width, requested.height),
            "a later settled host resize was ignored");
        require(!policy.hostSizeSettled(requested.width, requested.height),
            "one settled host size requested more than one guest resize");
        require(policy.guestScanoutChanged(applied.width, applied.height),
            "the eventual guest-applied size was ignored");
        require(policy.activeGuestSize() == applied,
            "presentation did not adopt the eventual guest-applied size");
        require(!policy.hostSizeSettled(applied.width, applied.height),
            "following the guest window size fed back another resize");
        require(policy.autoResizeGuestEnabled(),
            "repeated guest results disabled host-to-guest resizing");
    }
}

} // namespace

int main()
{
    try {
        resizeNotificationsAreCoalescedAndBounded();
        framebufferResizePublishesOneClearedGeometry();
        aspectFitCentersGuestContentWithoutDistortion();
        hostRotationSwapsPresentationAndMapsGuestInput();
        pointerPathsClipToExactGuestEdges();
        guestModesetsNeverDisableHostResize();
        configuredOffAlwaysKeepsHostResizeSilent();
        repeatedSettledResizesRequestOnceAndFollowEveryGuestResult();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "GPU display resize test failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
