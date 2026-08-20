// SPDX-License-Identifier: GPL-2.0-or-later

#include "native_display.hpp"

#include "metal_presentation.hpp"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

namespace uml::gpu {

struct NativeDisplayState final :
    public std::enable_shared_from_this<NativeDisplayState> {
    NativeDisplayState(std::uint32_t width, std::uint32_t height)
        : presentation(width, height)
    {
    }

    void attachView(void* nativeView);
    void requestRedraw() noexcept;
    void shutdown() noexcept;

    MetalPresentation presentation;
    id<MTKViewDelegate> delegate;
    __weak MTKView* view;
    std::atomic_bool running {true};
    std::atomic_bool redrawScheduled {};
};

} // namespace uml::gpu

@interface UmlIOSMetalViewDelegate : NSObject <MTKViewDelegate>
- (instancetype)initWithState:
    (std::weak_ptr<uml::gpu::NativeDisplayState>)state;
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
    (void)size;
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
        metalView.framebufferOnly = YES;
        metalView.enableSetNeedsDisplay = YES;
        metalView.paused = YES;
        metalView.autoResizeDrawable = NO;
        metalView.drawableSize = CGSizeMake(
            state->presentation.framebuffer().width(),
            state->presentation.framebuffer().height());
        UmlIOSMetalViewDelegate* viewDelegate =
            [[UmlIOSMetalViewDelegate alloc] initWithState:state];
        state->view = metalView;
        state->delegate = viewDelegate;
        metalView.delegate = viewDelegate;
        [metalView draw];
    };
    if ([NSThread isMainThread])
        configure();
    else
        dispatch_sync(dispatch_get_main_queue(), configure);
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
    presentation.shutdown();
    auto state = shared_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        MTKView* nativeView = state->view;
        if (nativeView.delegate == state->delegate)
            nativeView.delegate = nil;
        state->view = nil;
        state->delegate = nil;
    });
}

NativeDisplay::NativeDisplay(
    std::uint32_t width,
    std::uint32_t height,
    uml::input::vnc::VncInputSink,
    void* nativeView)
    : state_(std::make_shared<NativeDisplayState>(width, height))
{
    state_->attachView(nativeView);
}

NativeDisplay::~NativeDisplay()
{
    state_->shutdown();
}

std::uint32_t NativeDisplay::width() const noexcept
{
    return state_->presentation.framebuffer().width();
}

std::uint32_t NativeDisplay::height() const noexcept
{
    return state_->presentation.framebuffer().height();
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
