// SPDX-License-Identifier: GPL-2.0-or-later

#include "offscreen_display.hpp"

#include "display_geometry.hpp"
#include "frame_tap.hpp"

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>

namespace uml::gpu {
namespace {

/* The mirror's receiver: the frame is borrowed for the call and this keeps
 * none, so the pool recycles its buffers as fast as it makes them. */
void dropMirroredFrame(void*, void*) {}

} // namespace

struct OffscreenDisplayState final :
    public NativeDisplayStateCore,
    public std::enable_shared_from_this<OffscreenDisplayState> {
    OffscreenDisplayState(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t maximumDimension,
        const OffscreenDisplayOptions& displayOptions)
        /* No host window ever resizes, so the guest keeps its own mode. */
        : NativeDisplayStateCore(width, height, maximumDimension, false)
        , options(displayOptions)
    {
    }

    void start(std::uint32_t maximumDimension);
    void requestRedraw() noexcept;
    [[nodiscard]] bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept override;
    void resetGuestSession() noexcept override;
    void shutdown() noexcept override;

    OffscreenDisplayOptions options;
    CAMetalLayer* layer {};
    dispatch_queue_t queue {};
    std::atomic_bool running {true};
    std::atomic_bool redrawScheduled {};
    bool mirrorStarted {};
};

void OffscreenDisplayState::start(std::uint32_t maximumDimension)
{
    const auto drawable = offscreenDrawableSize(
        options, geometryPolicy.advertisedSize(), maximumDimension);

    layer = [CAMetalLayer layer];
    layer.device = (__bridge id<MTLDevice>)presentation.device();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.maximumDrawableCount = 3;
    layer.drawableSize = CGSizeMake(drawable.width, drawable.height);
    layer.opaque = YES;

    dispatch_queue_attr_t attributes = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);
    queue = dispatch_queue_create(
        "org.linux-um-darwin.offscreen-display", attributes);

    presentation.setRedrawCallback([weak = weak_from_this()] {
        if (auto locked = weak.lock())
            locked->requestRedraw();
    });
    if (options.mirrorWidth != 0 && options.mirrorFramesPerSecond != 0) {
        setFrameTap(
            dropMirroredFrame,
            nullptr,
            {.maximumWidth = options.mirrorWidth,
             .framesPerSecond = options.mirrorFramesPerSecond});
        mirrorStarted = true;
    }
    std::fprintf(
        stderr,
        "Offscreen display: %ux%u drawables, frame mirror %s\n",
        drawable.width,
        drawable.height,
        mirrorStarted ? "on" : "off");
}

void OffscreenDisplayState::requestRedraw() noexcept
{
    if (!running.load(std::memory_order_acquire) ||
        redrawScheduled.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto state = shared_from_this();
    dispatch_async(queue, ^{
        /* Cleared before the draw, so a frame that arrives during it asks
         * for another. */
        state->redrawScheduled.store(false, std::memory_order_release);
        if (state->running.load(std::memory_order_acquire)) {
            state->presentation.drawLayer(
                (__bridge void*)state->layer);
        }
    });
}

bool OffscreenDisplayState::guestScanoutChanged(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    bool publishFrame = false;
    {
        std::lock_guard lock(resizeLifecycleMutex);
        const auto& framebuffer = presentation.framebuffer();
        if (framebuffer.width() != width || framebuffer.height() != height) {
            if (!presentation.resize(width, height, false))
                return false;
            publishFrame = true;
        }
        if (geometryPolicy.guestScanoutChanged(width, height))
            publishFrame = true;
    }
    if (publishFrame)
        presentation.publishCpuFrame();
    return true;
}

void OffscreenDisplayState::resetGuestSession() noexcept
{
    std::lock_guard lock(resizeLifecycleMutex);
    const auto latestHost = resizeNotifier.drain();
    (void)geometryPolicy.hostSizeSettled(
        latestHost.width, latestHost.height);
    const auto restored = geometryPolicy.settledHostSize();
    const auto& framebuffer = presentation.framebuffer();
    const DisplaySize presented {
        .width = framebuffer.width(),
        .height = framebuffer.height(),
    };
    const bool presentationReady = presented == restored ||
        presentation.resize(restored.width, restored.height, false);
    geometryPolicy.resetGuestSession(
        presentationReady ? restored : presented);
    resizeCommitPending.store(false, std::memory_order_release);
}

void OffscreenDisplayState::shutdown() noexcept
{
    if (!running.exchange(false, std::memory_order_acq_rel))
        return;
    if (mirrorStarted)
        setFrameTap(nullptr, nullptr);
    presentation.shutdown();
    /* A draw that was already queued is over once this returns; one that is
     * queued after it sees the display stopped and draws nothing. The queue
     * and the layer stay until the state is destroyed, because a redraw
     * request that passed the running check before this ran may still be on
     * its way to the queue. */
    if (queue != nullptr)
        dispatch_sync(queue, ^{});
}

std::shared_ptr<NativeDisplayStateCore> makeOffscreenDisplayState(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t maximumDimension,
    const OffscreenDisplayOptions& options)
{
    auto state = std::make_shared<OffscreenDisplayState>(
        width, height, maximumDimension, options);
    state->start(maximumDimension);
    return state;
}

} // namespace uml::gpu
