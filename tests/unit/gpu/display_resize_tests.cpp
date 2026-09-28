// SPDX-License-Identifier: GPL-2.0-or-later

#include "display_geometry.hpp"
#include "display_resize_notifier.hpp"
#include "scanout_framebuffer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

[[nodiscard]] bool framebufferHasStorage(
    uml::gpu::ScanoutFramebuffer& framebuffer)
{
    std::lock_guard lock(framebuffer);
    return framebuffer.hasStorage();
}

void framebufferResizePublishesOneClearedGeometry()
{
    uml::gpu::ScanoutFramebuffer framebuffer {64, 48};
    require(!framebufferHasStorage(framebuffer),
        "a new framebuffer held pixels before anything was written");
    framebuffer.fillSelfTestPattern();
    require(framebufferHasStorage(framebuffer),
        "the self-test pattern did not allocate the framebuffer");
    framebuffer.resize(96, 72);
    require(framebuffer.width() == 96 && framebuffer.height() == 72,
        "framebuffer resize did not publish its new dimensions");
    require(
        framebuffer.rowBytes() ==
            96U * uml::gpu::protocol::gpuBytesPerPixel,
        "framebuffer resize retained the old row stride");
    {
        /* A new mode is black until something is written to it, and holds
         * no pixels until then. */
        std::lock_guard lock(framebuffer);
        require(!framebuffer.hasStorage() && framebuffer.pixels().empty() &&
                framebuffer.data() == nullptr,
            "framebuffer resize kept storage for the new geometry");
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
        require(pixels.size() ==
                96U * 72U * uml::gpu::protocol::gpuBytesPerPixel,
            "a partial update did not allocate the whole framebuffer");
        require(std::equal(
                    bluePixel.begin(),
                    bluePixel.end(),
                    pixels.end() -
                        static_cast<std::ptrdiff_t>(bluePixel.size())),
            "packed update used the pre-resize framebuffer stride");
        require(std::all_of(
                    pixels.begin(),
                    pixels.end() -
                        static_cast<std::ptrdiff_t>(bluePixel.size()),
                    [](std::uint8_t value) { return value == 0; }),
            "a partial update exposed pixels from the old geometry");
    }

    /* Clearing releases the pixels too, and the next write starts again
     * from black. */
    framebuffer.fillSelfTestPattern();
    framebuffer.clear();
    require(!framebufferHasStorage(framebuffer),
        "clearing the framebuffer kept its storage");
    require(framebuffer.width() == 96 && framebuffer.height() == 72,
        "clearing the framebuffer changed the applied mode");
    (void)framebuffer.update(
        bluePixel,
        1,
        1,
        bluePixel.size(),
        uml::gpu::protocol::GpuFormat::b8g8r8a8Unorm,
        0,
        0,
        0,
        0,
        1,
        1);
    {
        std::lock_guard lock(framebuffer);
        const auto pixels = framebuffer.pixels();
        require(std::equal(bluePixel.begin(), bluePixel.end(),
                    pixels.begin()) &&
                std::all_of(
                    pixels.begin() +
                        static_cast<std::ptrdiff_t>(bluePixel.size()),
                    pixels.end(),
                    [](std::uint8_t value) { return value == 0; }),
            "an update after a clear exposed the cleared pixels");
    }
}

/* LibVNCServer reads a retained framebuffer through the pointer it was given
 * at construction, so clearing zeroes that storage in place. */
void retainedFramebufferKeepsItsStorage()
{
    uml::gpu::ScanoutFramebuffer framebuffer {
        8, 4, uml::gpu::FramebufferStorage::retained};
    std::uint8_t* storage = nullptr;
    {
        std::lock_guard lock(framebuffer);
        storage = framebuffer.data();
        require(storage != nullptr &&
                std::ranges::all_of(
                    framebuffer.pixels(),
                    [](std::uint8_t value) { return value == 0; }),
            "a retained framebuffer did not start with zeroed storage");
    }
    framebuffer.fillSelfTestPattern();
    {
        std::lock_guard lock(framebuffer);
        framebuffer.clearLocked();
        require(framebuffer.data() == storage &&
                std::ranges::all_of(
                    framebuffer.pixels(),
                    [](std::uint8_t value) { return value == 0; }),
            "clearing a retained framebuffer moved or kept its pixels");
    }
    framebuffer.clear();
    require(framebufferHasStorage(framebuffer),
        "clearing a retained framebuffer released its storage");
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

[[nodiscard]] bool near(double actual, double expected)
{
    return std::abs(actual - expected) < 1e-12;
}

void scanoutRectanglesSampleOnlyTheirOwnTexels()
{
    using uml::gpu::DisplaySize;
    using uml::gpu::scanoutSampling;

    /* A guest mode larger than the display is sampled whole and scaled by
     * the presenter; its size is what the presenter aspect-fits. */
    const auto whole = scanoutSampling({
        .texture = {.width = 4096, .height = 2160},
        .source = {.width = 4096, .height = 2160},
        .width = 4096,
        .height = 2160,
    });
    require(whole.has_value() &&
            whole->content == DisplaySize {.width = 4096, .height = 2160},
        "a scanout larger than the display was not sampled at its own size");
    require(near(whole->origin.x, 0.0) && near(whole->origin.y, 0.0) &&
            near(whole->extent.x, 1.0) && near(whole->extent.y, 1.0),
        "a whole scanout did not span its texture");
    require(near(whole->minimum.x, 0.5 / 4096.0) &&
            near(whole->minimum.y, 0.5 / 2160.0) &&
            near(whole->maximum.x, 4095.5 / 4096.0) &&
            near(whole->maximum.y, 2159.5 / 2160.0),
        "a whole scanout was not clamped to its edge texel centres");

    /* A rectangle inside a larger source, stored in a larger texture,
     * samples only its own texels. */
    const auto panned = scanoutSampling({
        .texture = {.width = 256, .height = 128},
        .source = {.width = 200, .height = 100},
        .x = 40,
        .y = 20,
        .width = 80,
        .height = 60,
    });
    require(panned.has_value() &&
            panned->content == DisplaySize {.width = 80, .height = 60},
        "a scanout rectangle was not presented at its own size");
    require(near(panned->origin.x, 40.0 / 256.0) &&
            near(panned->origin.y, 20.0 / 128.0) &&
            near(panned->extent.x, 80.0 / 256.0) &&
            near(panned->extent.y, 60.0 / 128.0),
        "a scanout rectangle did not map onto its texels");
    require(near(panned->minimum.x, 40.5 / 256.0) &&
            near(panned->minimum.y, 20.5 / 128.0) &&
            near(panned->maximum.x, 119.5 / 256.0) &&
            near(panned->maximum.y, 79.5 / 128.0),
        "a scanout rectangle could sample texels outside it");

    /* Stored bottom row first, the rectangle's top guest row is texture row
     * source height - 1 - y, and the presenter walks it downwards. */
    const auto flipped = scanoutSampling({
        .texture = {.width = 256, .height = 128},
        .source = {.width = 200, .height = 100},
        .x = 40,
        .y = 20,
        .width = 80,
        .height = 60,
        .flipped = true,
    });
    require(flipped.has_value() &&
            near(flipped->origin.y, 80.0 / 128.0) &&
            near(flipped->extent.y, -60.0 / 128.0) &&
            near(flipped->origin.x, panned->origin.x) &&
            near(flipped->extent.x, panned->extent.x),
        "a bottom-up scanout rectangle was not turned the right way up");
    const double topRowCentre =
        flipped->origin.y + (0.5 / 60.0) * flipped->extent.y;
    require(near(topRowCentre, 79.5 / 128.0),
        "a bottom-up scanout's top row was not texture row 79");
    require(near(flipped->minimum.y, 20.5 / 128.0) &&
            near(flipped->maximum.y, 79.5 / 128.0),
        "a bottom-up scanout rectangle could sample texels outside it");

    const uml::gpu::ScanoutRegion valid {
        .texture = {.width = 64, .height = 64},
        .source = {.width = 64, .height = 64},
        .width = 64,
        .height = 64,
    };
    auto invalid = valid;
    invalid.width = 0;
    require(!scanoutSampling(invalid), "an empty scanout was sampled");
    invalid = valid;
    invalid.x = 1;
    require(!scanoutSampling(invalid),
        "a scanout reaching past its source was sampled");
    invalid = valid;
    invalid.y = UINT32_MAX;
    invalid.height = 2;
    require(!scanoutSampling(invalid),
        "a scanout whose rectangle overflows was sampled");
    invalid = valid;
    invalid.texture.height = 63;
    require(!scanoutSampling(invalid),
        "a texture smaller than its source was sampled");
    invalid = valid;
    invalid.source = {};
    invalid.width = 0;
    invalid.height = 0;
    require(!scanoutSampling(invalid), "an empty source was sampled");
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

void fixedFramebufferPlacementsUseWholePixels()
{
    using uml::gpu::DisplayRectangle;
    using uml::gpu::aspectFitPixels;
    const auto place = [](std::uint32_t width, std::uint32_t height) {
        return aspectFitPixels({1280, 720}, {width, height});
    };
    require(place(1280, 720) == DisplayRectangle {0, 0, 1280, 720},
        "the preferred mode did not fill the framebuffer");
    /* glmark2's no-EDID 4096x2160 mode scales by 5/16 to 1280x675, and the
     * 22.5-pixel bars round to 23 above and 22 below. */
    require(place(4096, 2160) == DisplayRectangle {0, 23, 1280, 675},
        "an oversized mode was not letterboxed in whole pixels");
    require(place(640, 480) == DisplayRectangle {160, 0, 960, 720},
        "a smaller 4:3 mode was not magnified into a pillarbox");
    require(place(1280, 600) == DisplayRectangle {0, 60, 1280, 600},
        "a mode that fits at its own size was scaled");
    require(place(8192, 1) == DisplayRectangle {0, 360, 1280, 1},
        "a very wide mode rounded to an empty rectangle");
    require(place(0, 720) == DisplayRectangle {},
        "an empty mode produced a placement");
}

void fittedMousePointersKeepTheNativeBarRules()
{
    using uml::gpu::AspectFitRectangle;
    using uml::gpu::DisplayPoint;
    using uml::gpu::FittedPointer;
    const AspectFitRectangle content {
        .x = 0.0, .y = 23.0, .width = 1280.0, .height = 675.0};
    FittedPointer pointer;

    auto samples = pointer.mouse(content, {640.0, 360.5}, 0);
    require(samples.count == 1 && near(samples.samples[0].point.x, 0.5) &&
            near(samples.samples[0].point.y, 0.5) &&
            samples.samples[0].buttons == 0,
        "hover inside the guest did not map through the fitted rectangle");
    require(pointer.mouse(content, {640.0, 10.0}, 0).count == 0,
        "hover in a letterbox bar reached the guest");

    /* A press in the bar belongs to the host until every button is up. */
    require(pointer.mouse(content, {640.0, 10.0}, 1).count == 0,
        "a press in a letterbox bar reached the guest");
    require(pointer.mouse(content, {640.0, 360.5}, 1).count == 0,
        "a drag begun in a letterbox bar reached the guest");
    require(pointer.mouse(content, {640.0, 360.5}, 0).count == 0,
        "the release of a bar gesture reached the guest");
    require(pointer.mouse(content, {640.0, 360.5}, 0).count == 1,
        "the guest stayed suppressed after the bar gesture ended");

    /* A drag begun inside follows the pointer into the bar, clamped. */
    samples = pointer.mouse(content, {100.0, 100.0}, 1);
    require(samples.count == 1 && samples.samples[0].buttons == 1,
        "a press inside the guest was not delivered");
    samples = pointer.mouse(content, {100.0, 5.0}, 1);
    require(samples.count == 1 && near(samples.samples[0].point.y, 0.0) &&
            samples.samples[0].buttons == 1,
        "a drag into a bar was not clamped to the guest edge");
    samples = pointer.mouse(content, {100.0, 5.0}, 0);
    require(samples.count == 1 && samples.samples[0].buttons == 0,
        "releasing a drag in a bar did not reach the guest");
    require(pointer.mouse(content, {100.0, 5.0}, 0).count == 0,
        "hover in a bar reached the guest after a drag ended");

    /* A mode change releases held buttons and keeps the rest of the
     * gesture out of the guest. */
    require(pointer.mouse(content, {100.0, 100.0}, 4).count == 1,
        "a secondary press inside the guest was not delivered");
    require(pointer.release(),
        "releasing held buttons did not report them");
    require(pointer.mouse(content, {100.0, 100.0}, 4).count == 0,
        "a gesture continued into the guest after a mode change");
    require(pointer.mouse(content, {100.0, 100.0}, 0).count == 0 &&
            pointer.mouse(content, {100.0, 100.0}, 0).count == 1,
        "the guest stayed suppressed after the released gesture ended");
    require(!pointer.release(), "releasing an idle pointer reported buttons");
}

void fittedTouchesEnterAndLeaveAtTheGuestEdge()
{
    using uml::gpu::AspectFitRectangle;
    using uml::gpu::DisplayPoint;
    using uml::gpu::FittedPointer;
    const AspectFitRectangle content {
        .x = 0.0, .y = 23.0, .width = 1280.0, .height = 675.0};
    FittedPointer pointer;

    require(pointer.touch(content, {640.0, 10.0}, true).count == 0,
        "a touch that began in a letterbox bar reached the guest");
    auto samples = pointer.touch(content, {640.0, 360.5}, true);
    require(samples.count == 2 && near(samples.samples[0].point.y, 0.0) &&
            samples.samples[0].buttons == 1 &&
            near(samples.samples[1].point.y, 0.5) &&
            samples.samples[1].buttons == 1,
        "a touch entering the guest did not begin at its edge");
    samples = pointer.touch(content, {640.0, 715.0}, true);
    require(samples.count == 1 && near(samples.samples[0].point.y, 1.0) &&
            samples.samples[0].buttons == 0,
        "a touch leaving the guest did not end at its edge");
    require(pointer.touch(content, {640.0, 716.0}, true).count == 0,
        "a touch in a bar kept a guest contact");
    require(pointer.touch(content, {640.0, 716.0}, false).count == 0,
        "lifting a touch in a bar reached the guest");

    /* A tap that stays inside the guest is one contact. */
    samples = pointer.touch(content, {10.0, 100.0}, true);
    require(samples.count == 1 && samples.samples[0].buttons == 1,
        "a touch inside the guest did not begin a contact");
    samples = pointer.touch(content, {10.0, 100.0}, false);
    require(samples.count == 1 && samples.samples[0].buttons == 0,
        "lifting a touch inside the guest did not end its contact");

    samples = pointer.touch(content, {10.0, 100.0}, true);
    require(samples.count == 1, "a second touch was not delivered");
    require(pointer.release(), "releasing a contact did not report it");
    require(pointer.touch(content, {20.0, 100.0}, true).count == 0,
        "a touch continued into the guest after a mode change");
    require(pointer.touch(content, {20.0, 100.0}, false).count == 0 &&
            pointer.touch(content, {20.0, 100.0}, true).count == 1,
        "the guest stayed suppressed after the released touch ended");
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
        retainedFramebufferKeepsItsStorage();
        aspectFitCentersGuestContentWithoutDistortion();
        scanoutRectanglesSampleOnlyTheirOwnTexels();
        hostRotationSwapsPresentationAndMapsGuestInput();
        pointerPathsClipToExactGuestEdges();
        fixedFramebufferPlacementsUseWholePixels();
        fittedMousePointersKeepTheNativeBarRules();
        fittedTouchesEnterAndLeaveAtTheGuestEdge();
        guestModesetsNeverDisableHostResize();
        configuredOffAlwaysKeepsHostResizeSilent();
        repeatedSettledResizesRequestOnceAndFollowEveryGuestResult();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "GPU display resize test failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
