// SPDX-License-Identifier: GPL-2.0-or-later

#include "native_display.hpp"

#include "display_geometry.hpp"
#include "display_resize_notifier.hpp"
#include "metal_presentation.hpp"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace uml::gpu {
namespace {

NSString* const resizeWillBeginNotification =
    @"org.linux-um-darwin.guest-display-resize-will-begin";
NSString* const resizeDidEndNotification =
    @"org.linux-um-darwin.guest-display-resize-did-end";
constexpr std::int64_t resizeSettleNanoseconds =
    150LL * NSEC_PER_MSEC;

std::optional<std::uint32_t> displayDimension(
    CGFloat dimension,
    std::uint32_t maximum) noexcept
{
    if (!std::isfinite(dimension) || dimension < 1.0)
        return std::nullopt;
    return static_cast<std::uint32_t>(std::min<CGFloat>(
        std::floor(dimension), maximum));
}

} // namespace

struct NativeDisplayState final :
    public std::enable_shared_from_this<NativeDisplayState> {
    NativeDisplayState(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t maximumDimension,
        bool autoResizeGuest)
        : presentation(
              DisplayResizeNotifier::clampDimension(
                  width, maximumDimension),
              DisplayResizeNotifier::clampDimension(
                  height, maximumDimension))
        , resizeNotifier(width, height, maximumDimension)
        , geometryPolicy(
              DisplayResizeNotifier::clampDimension(
                  width, maximumDimension),
              DisplayResizeNotifier::clampDimension(
                  height, maximumDimension),
              autoResizeGuest)
    {
    }

    void attachView(void* nativeView);
    void scheduleResize(CGSize size) noexcept;
    void beginResize() noexcept;
    void commitResize(CGSize size) noexcept;
    [[nodiscard]] bool drainResizeEvent();
    [[nodiscard]] bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept;
    void resetGuestSession() noexcept;
    void publishGuestSize(DisplaySize size) noexcept;
    void requestRedraw() noexcept;
    void shutdown() noexcept;

    MetalPresentation presentation;
    DisplayResizeNotifier resizeNotifier;
    DisplayGeometryPolicy geometryPolicy;
    id<MTKViewDelegate> delegate;
    id resizeWillBeginObserver;
    id resizeDidEndObserver;
    __weak MTKView* view;
    std::atomic_bool running {true};
    std::atomic_bool redrawScheduled {};
    std::atomic_bool resizeInProgress {};
    std::atomic_bool resizeCommitPending {};
    std::atomic_uint64_t resizeGeneration {};
    std::mutex resizeLifecycleMutex;
    DisplaySize latestSettledHostSize;
};

} // namespace uml::gpu

@interface UmlIOSMetalViewDelegate : NSObject <MTKViewDelegate>
- (instancetype)initWithState:
    (std::weak_ptr<uml::gpu::NativeDisplayState>)state;
@end

@protocol UmlIOSGuestResizeLifecycle <NSObject>
@property(nonatomic, readonly) BOOL umlGuestResizeInProgress;
@end

@protocol UmlIOSGuestDisplayGeometry <NSObject>
- (void)umlSetGuestDisplayWidth:(uint32_t)width height:(uint32_t)height;
@end

@implementation UmlIOSMetalViewDelegate {
    std::weak_ptr<uml::gpu::NativeDisplayState> _state;
}

- (instancetype)initWithState:
    (std::weak_ptr<uml::gpu::NativeDisplayState>)state
{
    self = [super init];
    if (self != nil)
        _state = std::move(state);
    return self;
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    (void)view;
    if (auto state = _state.lock()) {
        state->presentation.drawableSizeChanged();
        state->scheduleResize(size);
    }
}

- (void)drawInMTKView:(MTKView*)view
{
    if (auto state = _state.lock())
        state->presentation.draw((__bridge void*)view);
}

@end

namespace uml::gpu {

void NativeDisplayState::attachView(void* nativeView)
{
    if (nativeView == nullptr)
        throw std::invalid_argument("the iOS native display needs an MTKView");
    MTKView* metalView = (__bridge MTKView*)nativeView;
    if (![metalView isKindOfClass:MTKView.class])
        throw std::invalid_argument("the iOS native display view is not MTKView");
    auto state = shared_from_this();
    presentation.setRedrawCallback([weak = weak_from_this()] {
        if (auto locked = weak.lock())
            locked->requestRedraw();
    });
    const auto configure = ^{
        metalView.device = (__bridge id<MTLDevice>)state->presentation.device();
        metalView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        metalView.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        metalView.framebufferOnly = YES;
        metalView.enableSetNeedsDisplay = YES;
        metalView.paused = YES;
        metalView.autoResizeDrawable = NO;
        UmlIOSMetalViewDelegate* viewDelegate =
            [[UmlIOSMetalViewDelegate alloc] initWithState:state];
        state->view = metalView;
        state->delegate = viewDelegate;
        metalView.delegate = viewDelegate;
        const auto guestSize = state->geometryPolicy.activeGuestSize();
        if ([metalView respondsToSelector:
                @selector(umlSetGuestDisplayWidth:height:)]) {
            [(id<UmlIOSGuestDisplayGeometry>)metalView
                umlSetGuestDisplayWidth:guestSize.width
                height:guestSize.height];
        }
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        auto weak = state->weak_from_this();
        state->resizeWillBeginObserver = [center
            addObserverForName:resizeWillBeginNotification
                        object:metalView
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification*) {
                        if (auto locked = weak.lock())
                            locked->beginResize();
                    }];
        state->resizeDidEndObserver = [center
            addObserverForName:resizeDidEndNotification
                        object:metalView
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification*) {
                        if (auto locked = weak.lock())
                            locked->commitResize(metalView.drawableSize);
                    }];
        const bool transitionActive =
            [metalView respondsToSelector:@selector(umlGuestResizeInProgress)] &&
            [(id<UmlIOSGuestResizeLifecycle>)metalView
                umlGuestResizeInProgress];
        if (transitionActive)
            state->beginResize();
        else
            state->commitResize(metalView.drawableSize);
        [metalView draw];
    };
    if ([NSThread isMainThread])
        configure();
    else
        dispatch_sync(dispatch_get_main_queue(), configure);
}

void NativeDisplayState::beginResize() noexcept
{
    std::lock_guard lock(resizeLifecycleMutex);
    resizeInProgress.store(true, std::memory_order_release);
    resizeGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void NativeDisplayState::commitResize(CGSize size) noexcept
{
    std::lock_guard lock(resizeLifecycleMutex);
    resizeGeneration.fetch_add(1, std::memory_order_acq_rel);
    const auto width = displayDimension(
        size.width, resizeNotifier.maximumDimension());
    const auto height = displayDimension(
        size.height, resizeNotifier.maximumDimension());
    if (width && height &&
        running.load(std::memory_order_acquire)) {
        latestSettledHostSize = {.width = *width, .height = *height};
        resizeNotifier.requestCommitted(*width, *height);
        resizeCommitPending.store(true, std::memory_order_release);
    }
    resizeInProgress.store(false, std::memory_order_release);
}

void NativeDisplayState::scheduleResize(CGSize size) noexcept
{
    const auto width = displayDimension(
        size.width, resizeNotifier.maximumDimension());
    const auto height = displayDimension(
        size.height, resizeNotifier.maximumDimension());
    if (!width || !height)
        return;
    std::uint64_t generation;
    {
        std::lock_guard lock(resizeLifecycleMutex);
        if (resizeInProgress.load(std::memory_order_acquire))
            return;
        generation =
            resizeGeneration.fetch_add(1, std::memory_order_acq_rel) + 1U;
    }
    auto weak = weak_from_this();
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, resizeSettleNanoseconds),
        dispatch_get_main_queue(),
        ^{
            if (auto state = weak.lock()) {
                std::lock_guard lock(state->resizeLifecycleMutex);
                if (state->running.load(std::memory_order_acquire) &&
                    !state->resizeInProgress.load(std::memory_order_acquire) &&
                    state->resizeGeneration.load(std::memory_order_acquire) ==
                        generation) {
                    state->latestSettledHostSize = {
                        .width = *width,
                        .height = *height,
                    };
                    state->resizeNotifier.request(*width, *height);
                }
            }
        });
}

bool NativeDisplayState::drainResizeEvent()
{
    std::lock_guard lock(resizeLifecycleMutex);
    const auto size = resizeNotifier.drain();
    if (resizeInProgress.load(std::memory_order_acquire))
        return false;
    const bool changed = geometryPolicy.hostSizeSettled(
        size.width, size.height);
    resizeCommitPending.store(false, std::memory_order_release);
    return changed;
}

void NativeDisplayState::publishGuestSize(DisplaySize size) noexcept
{
    auto weak = weak_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        if (auto state = weak.lock()) {
            if (!state->running.load(std::memory_order_acquire))
                return;
            MTKView* nativeView = state->view;
            if ([nativeView respondsToSelector:
                    @selector(umlSetGuestDisplayWidth:height:)]) {
                [(id<UmlIOSGuestDisplayGeometry>)nativeView
                    umlSetGuestDisplayWidth:size.width
                    height:size.height];
            }
        }
    });
}

bool NativeDisplayState::guestScanoutChanged(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    DisplaySize guestSize;
    GuestDisplayChange change;
    bool publishFrame = false;
    {
        std::lock_guard lock(resizeLifecycleMutex);
        const auto& framebuffer = presentation.framebuffer();
        const bool presentationChanged =
            framebuffer.width() != width || framebuffer.height() != height;
        if (presentationChanged) {
            if (!presentation.resize(width, height, false))
                return false;
            publishFrame = true;
        }
        change = geometryPolicy.guestScanoutChanged(width, height);
        guestSize = geometryPolicy.activeGuestSize();
        if (change.independent) {
            resizeGeneration.fetch_add(1, std::memory_order_acq_rel);
            resizeCommitPending.store(false, std::memory_order_release);
        }
    }
    if (change.changed)
        publishGuestSize(guestSize);
    if (change.changed || publishFrame) {
        /* The UIKit coordinate update above reaches the main queue before the
         * redraw requested here can expose the new aspect ratio. */
        presentation.publishCpuFrame();
    }
    return true;
}

void NativeDisplayState::resetGuestSession() noexcept
{
    DisplaySize guestSize;
    {
        std::lock_guard lock(resizeLifecycleMutex);
        /* Discard any pre-reset readiness byte before restoring the newest
         * settled host geometry below. Otherwise the transport could later
         * consume a stale size and advertise it as a post-reset resize. */
        (void)resizeNotifier.drain();
        if (latestSettledHostSize.width != 0 &&
            latestSettledHostSize.height != 0) {
            (void)geometryPolicy.hostSizeSettled(
                latestSettledHostSize.width,
                latestSettledHostSize.height);
        }
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
        guestSize = geometryPolicy.activeGuestSize();
        resizeGeneration.fetch_add(1, std::memory_order_acq_rel);
        resizeCommitPending.store(false, std::memory_order_release);
    }
    publishGuestSize(guestSize);
}

void NativeDisplayState::requestRedraw() noexcept
{
    if (!running.load(std::memory_order_acquire) ||
        redrawScheduled.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto state = shared_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        state->redrawScheduled.store(false, std::memory_order_release);
        if (!state->running.load(std::memory_order_acquire))
            return;
        MTKView* nativeView = state->view;
        if (nativeView != nil)
            [nativeView draw];
    });
}

void NativeDisplayState::shutdown() noexcept
{
    if (!running.exchange(false, std::memory_order_acq_rel))
        return;
    {
        std::lock_guard lock(resizeLifecycleMutex);
        resizeInProgress.store(true, std::memory_order_release);
        resizeGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    presentation.shutdown();
    auto state = shared_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        MTKView* nativeView = state->view;
        if (nativeView.delegate == state->delegate)
            nativeView.delegate = nil;
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        if (state->resizeWillBeginObserver != nil)
            [center removeObserver:state->resizeWillBeginObserver];
        if (state->resizeDidEndObserver != nil)
            [center removeObserver:state->resizeDidEndObserver];
        state->view = nil;
        state->delegate = nil;
        state->resizeWillBeginObserver = nil;
        state->resizeDidEndObserver = nil;
    });
}

NativeDisplay::NativeDisplay(
    std::uint32_t width,
    std::uint32_t height,
    uml::input::vnc::VncInputSink,
    void* nativeView,
    uml::input::vnc::PointerMode,
    std::uint32_t maximumDimension,
    bool autoResizeGuest)
    : state_(std::make_shared<NativeDisplayState>(
          width, height, maximumDimension, autoResizeGuest))
{
    state_->attachView(nativeView);
}

NativeDisplay::~NativeDisplay()
{
    state_->shutdown();
}

std::uint32_t NativeDisplay::width() const noexcept
{
    return state_->geometryPolicy.advertisedSize().width;
}

std::uint32_t NativeDisplay::height() const noexcept
{
    return state_->geometryPolicy.advertisedSize().height;
}

int NativeDisplay::resizeEventDescriptor() const noexcept
{
    return state_->resizeNotifier.descriptor();
}

bool NativeDisplay::drainResizeEvent()
{
    return state_->drainResizeEvent();
}

bool NativeDisplay::guestScanoutChanged(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    return state_->guestScanoutChanged(width, height);
}

void NativeDisplay::resetGuestSession() noexcept
{
    state_->resetGuestSession();
}

bool NativeDisplay::claimResizeNotification() noexcept
{
    state_->resizeLifecycleMutex.lock();
    if (!state_->geometryPolicy.autoResizeGuestEnabled() ||
        state_->resizeInProgress.load(std::memory_order_acquire) ||
        state_->resizeCommitPending.load(std::memory_order_acquire)) {
        state_->resizeLifecycleMutex.unlock();
        return false;
    }
    return true;
}

void NativeDisplay::releaseResizeNotification() noexcept
{
    state_->resizeLifecycleMutex.unlock();
}

void NativeDisplay::clear() noexcept
{
    state_->presentation.framebuffer().clear();
    state_->presentation.publishCpuFrame();
}

void NativeDisplay::update(
    const std::vector<std::uint8_t>& source,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    protocol::GpuFormat sourceFormat,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height)
{
    update(
        std::span<const std::uint8_t>(source),
        sourceWidth,
        sourceHeight,
        static_cast<std::size_t>(sourceWidth) * protocol::gpuBytesPerPixel,
        sourceFormat,
        x,
        y,
        x,
        y,
        width,
        height);
}

void NativeDisplay::update(
    std::span<const std::uint8_t> source,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::size_t sourceRowBytes,
    protocol::GpuFormat sourceFormat,
    std::uint32_t sourceX,
    std::uint32_t sourceY,
    std::uint32_t destinationX,
    std::uint32_t destinationY,
    std::uint32_t width,
    std::uint32_t height)
{
    const auto rectangle = state_->presentation.framebuffer().update(
        source,
        sourceWidth,
        sourceHeight,
        sourceRowBytes,
        sourceFormat,
        sourceX,
        sourceY,
        destinationX,
        destinationY,
        width,
        height);
    if (rectangle.width != 0 && rectangle.height != 0)
        state_->presentation.publishCpuFrame();
}

void NativeDisplay::fillSelfTestPattern()
{
    state_->presentation.framebuffer().fillSelfTestPattern();
    state_->presentation.publishCpuFrame();
}

PresentOutcome NativeDisplay::presentMetal(
    const MetalScanout& scanout) noexcept
{
    return state_->presentation.present(scanout);
}

DisplayStatistics NativeDisplay::statistics() const noexcept
{
    return state_->presentation.statistics();
}

void NativeDisplay::recordCpuFallbackFrame() noexcept
{
    state_->presentation.recordCpuFallbackFrame();
}

} // namespace uml::gpu
