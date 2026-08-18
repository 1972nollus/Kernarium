// SPDX-License-Identifier: GPL-2.0-or-later

#include "native_display.hpp"

#include "scanout_framebuffer.hpp"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace uml::gpu {

namespace {

/* Seconds between native-display frame-statistics reports. Unset, zero, or
 * unparseable keeps them off. */
[[nodiscard]] double frameStatisticsInterval() noexcept
{
    static const double interval = [] {
        const char* value = std::getenv("UML_GPU_FRAME_STATS");
        if (value == nullptr)
            return 0.0;
        char* end = nullptr;
        const double seconds = std::strtod(value, &end);
        if (end == value || !(seconds > 0.0))
            return 0.0;
        return seconds;
    }();
    return interval;
}

} // namespace
namespace {

NSString* const metalShaderSource = @R"metal(
#include <metal_stdlib>
using namespace metal;

struct RasterData {
    float4 position [[position]];
    float2 textureCoordinate;
};

vertex RasterData displayVertex(unsigned int vertexId [[vertex_id]])
{
    const float2 positions[] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0),
    };
    const float2 coordinates[] = {
        float2(0.0,  1.0),
        float2(2.0,  1.0),
        float2(0.0, -1.0),
    };
    RasterData output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.textureCoordinate = coordinates[vertexId];
    return output;
}

fragment float4 displayFragment(
    RasterData input [[stage_in]],
    texture2d<float> scanout [[texture(0)]])
{
    constexpr sampler textureSampler(
        mag_filter::nearest,
        min_filter::nearest,
        address::clamp_to_edge);
    const float4 color = scanout.sample(
        textureSampler, input.textureCoordinate);
    return float4(color.rgb, 1.0);
}
)metal";

std::string description(NSError* error)
{
    if (error == nil || error.localizedDescription == nil)
        return "unknown Metal error";
    const char* text = error.localizedDescription.UTF8String;
    return text == nullptr ? "unknown Metal error" : std::string(text);
}

} // namespace

struct NativeDisplayState final :
    public std::enable_shared_from_this<NativeDisplayState> {
    NativeDisplayState(std::uint32_t width, std::uint32_t height)
        : framebuffer(width, height)
    {
        device = MTLCreateSystemDefaultDevice();
        if (device == nil)
            throw std::runtime_error("Metal has no default GPU device");
        commandQueue = [device newCommandQueue];
        if (commandQueue == nil)
            throw std::runtime_error("Metal command queue creation failed");

        NSError* error = nil;
        id<MTLLibrary> library = [device
            newLibraryWithSource:metalShaderSource
            options:nil
            error:&error];
        if (library == nil) {
            throw std::runtime_error(
                "Metal display shader compilation failed: " +
                description(error));
        }
        id<MTLFunction> vertex = [library newFunctionWithName:@"displayVertex"];
        id<MTLFunction> fragment =
            [library newFunctionWithName:@"displayFragment"];
        if (vertex == nil || fragment == nil)
            throw std::runtime_error("Metal display shader entry is missing");
        MTLRenderPipelineDescriptor* descriptor =
            [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pipeline = [device
            newRenderPipelineStateWithDescriptor:descriptor
            error:&error];
        if (pipeline == nil) {
            throw std::runtime_error(
                "Metal display pipeline creation failed: " +
                description(error));
        }

        MTLTextureDescriptor* textureDescriptor =
            [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                width:width
                height:height
                mipmapped:NO];
        textureDescriptor.storageMode = MTLStorageModeShared;
        textureDescriptor.usage = MTLTextureUsageShaderRead;
        fallbackTexture = [device newTextureWithDescriptor:textureDescriptor];
        if (fallbackTexture == nil)
            throw std::runtime_error("Metal fallback texture creation failed");
    }

    void attachView(void* nativeView);
    void requestRedraw() noexcept;
    void publishCpuFrame() noexcept;
    [[nodiscard]] PresentOutcome presentMetal(
        const MetalScanout& scanout) noexcept;
    void draw(MTKView* nativeView) noexcept;
    void finishPresentation(std::uint64_t generation) noexcept;
    void maybeReport() noexcept;
    [[nodiscard]] DisplayStatistics snapshot() const noexcept;
    void shutdown() noexcept;

    ScanoutFramebuffer framebuffer;
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLRenderPipelineState> pipeline;
    id<MTLTexture> fallbackTexture;
    id<MTLTexture> latestTexture;
    id<MTKViewDelegate> delegate;
    __weak MTKView* view;
    std::mutex presentationMutex;
    std::uint64_t latestGeneration {1};
    std::uint64_t presentedGeneration {};
    bool latestIsCpu {true};
    bool presentationInFlight {};
    std::atomic_bool running {true};
    std::atomic_bool redrawScheduled {};
    std::atomic_bool directPathReported {};
    std::atomic_bool renderFailureReported {};
    std::atomic_uint64_t guestFrames {};
    std::atomic_uint64_t presentedFrames {};
    std::atomic_uint64_t droppedFrames {};
    std::atomic_uint64_t cpuFallbackFrames {};
    std::atomic_uint64_t reportedAt {};
    std::uint64_t reportedGuestFrames {};
    std::uint64_t reportedPresentedFrames {};
    std::uint64_t reportedDroppedFrames {};
    std::uint64_t reportedCpuFallbackFrames {};
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
        state->draw(view);
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
    const auto configure = ^{
        metalView.device = state->device;
        metalView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        metalView.framebufferOnly = YES;
        metalView.enableSetNeedsDisplay = YES;
        metalView.paused = YES;
        metalView.autoResizeDrawable = NO;
        metalView.drawableSize = CGSizeMake(
            state->framebuffer.width(), state->framebuffer.height());
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

void NativeDisplayState::publishCpuFrame() noexcept
{
    {
        std::lock_guard lock(presentationMutex);
        latestTexture = nil;
        latestIsCpu = true;
        ++latestGeneration;
    }
    requestRedraw();
}

PresentOutcome NativeDisplayState::presentMetal(
    const MetalScanout& scanout) noexcept
{
    guestFrames.fetch_add(1, std::memory_order_relaxed);
    try {
        if (scanout.texture == nullptr || scanout.sourceX != 0 ||
            scanout.sourceY != 0 || scanout.width != scanout.sourceWidth ||
            scanout.height != scanout.sourceHeight ||
            scanout.width != framebuffer.width() ||
            scanout.height != framebuffer.height()) {
            return PresentOutcome::unsupported;
        }
        id<MTLTexture> texture = (__bridge id<MTLTexture>)scanout.texture;
        if (texture == nil || texture.device != device)
            return PresentOutcome::unsupported;
        bool superseded = false;
        {
            std::lock_guard lock(presentationMutex);
            /* The guest's newest frame always wins. Anything published since
             * the host last presented is retired unseen, which is the drop
             * that keeps the two timelines independent. */
            superseded = latestGeneration > presentedGeneration;
            latestTexture = texture;
            latestIsCpu = false;
            ++latestGeneration;
        }
        if (superseded)
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
        if (!directPathReported.exchange(true, std::memory_order_relaxed)) {
            std::fputs(
                "Native display: direct virgl/ANGLE/Metal presentation\n",
                stderr);
        }
        requestRedraw();
        maybeReport();
        return superseded ? PresentOutcome::dropped
                          : PresentOutcome::presented;
    } catch (...) {
        return PresentOutcome::unsupported;
    }
}

void NativeDisplayState::finishPresentation(std::uint64_t generation) noexcept
{
    bool pending = false;
    {
        std::lock_guard lock(presentationMutex);
        presentedGeneration = std::max(presentedGeneration, generation);
        presentationInFlight = false;
        pending = latestGeneration > presentedGeneration;
    }
    if (pending)
        requestRedraw();
}

void NativeDisplayState::draw(MTKView* nativeView) noexcept
{
    @autoreleasepool {
        std::uint64_t generation = 0;
        bool useCpu = false;
        id<MTLTexture> sourceTexture;
        try {
            MTLRenderPassDescriptor* pass =
                nativeView.currentRenderPassDescriptor;
            id<CAMetalDrawable> drawable = nativeView.currentDrawable;
            if (pass == nil || drawable == nil)
                return;
            {
                std::lock_guard lock(presentationMutex);
                if (presentationInFlight ||
                    latestGeneration <= presentedGeneration) {
                    return;
                }
                generation = latestGeneration;
                useCpu = latestIsCpu;
                sourceTexture = useCpu ? fallbackTexture : latestTexture;
                if (sourceTexture == nil)
                    return;
                presentationInFlight = true;
            }
            if (useCpu) {
                std::lock_guard lock(framebuffer);
                const MTLRegion region = MTLRegionMake2D(
                    0, 0, framebuffer.width(), framebuffer.height());
                [fallbackTexture
                    replaceRegion:region
                    mipmapLevel:0
                    withBytes:framebuffer.pixels().data()
                    bytesPerRow:framebuffer.rowBytes()];
            }
            id<MTLCommandBuffer> buffer = [commandQueue commandBuffer];
            id<MTLRenderCommandEncoder> encoder =
                [buffer renderCommandEncoderWithDescriptor:pass];
            if (buffer == nil || encoder == nil) {
                finishPresentation(generation);
                return;
            }
            [encoder setRenderPipelineState:pipeline];
            [encoder setFragmentTexture:sourceTexture atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:3];
            [encoder endEncoding];
            [buffer presentDrawable:drawable];
            auto state = shared_from_this();
            [buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                /* Counted here rather than in finishPresentation, which the
                 * encoder-failure paths also reach. */
                state->presentedFrames.fetch_add(
                    1, std::memory_order_relaxed);
                state->finishPresentation(generation);
            }];
            [buffer commit];
        } catch (...) {
            if (!renderFailureReported.exchange(
                    true, std::memory_order_relaxed)) {
                std::fputs("Native iOS Metal display failed\n", stderr);
            }
            if (generation != 0)
                finishPresentation(generation);
        }
    }
}

DisplayStatistics NativeDisplayState::snapshot() const noexcept
{
    return DisplayStatistics {
        .guestFrames = guestFrames.load(std::memory_order_relaxed),
        .presentedFrames = presentedFrames.load(std::memory_order_relaxed),
        .droppedFrames = droppedFrames.load(std::memory_order_relaxed),
        .cpuFallbackFrames =
            cpuFallbackFrames.load(std::memory_order_relaxed),
    };
}

void NativeDisplayState::maybeReport() noexcept
{
    const double interval = frameStatisticsInterval();
    if (interval <= 0.0)
        return;
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    auto previous = reportedAt.load(std::memory_order_relaxed);
    if (previous == 0) {
        reportedAt.compare_exchange_strong(
            previous, now, std::memory_order_relaxed);
        return;
    }
    const auto span = now - previous;
    if (static_cast<double>(span) < interval * 1e9)
        return;
    if (!reportedAt.compare_exchange_strong(
            previous, now, std::memory_order_relaxed)) {
        return;
    }
    const DisplayStatistics total = snapshot();
    const double seconds = static_cast<double>(span) / 1e9;
    const auto guest = total.guestFrames - reportedGuestFrames;
    const auto presented = total.presentedFrames - reportedPresentedFrames;
    const auto dropped = total.droppedFrames - reportedDroppedFrames;
    const auto fallback =
        total.cpuFallbackFrames - reportedCpuFallbackFrames;
    reportedGuestFrames = total.guestFrames;
    reportedPresentedFrames = total.presentedFrames;
    reportedDroppedFrames = total.droppedFrames;
    reportedCpuFallbackFrames = total.cpuFallbackFrames;
    std::fprintf(
        stderr,
        "Native display frames: guest %.1f/s presented %.1f/s dropped %.1f/s "
        "(%.1f%%) cpu-fallback %.1f/s | totals guest %llu presented %llu "
        "dropped %llu cpu-fallback %llu\n",
        static_cast<double>(guest) / seconds,
        static_cast<double>(presented) / seconds,
        static_cast<double>(dropped) / seconds,
        guest == 0 ? 0.0 : 100.0 * static_cast<double>(dropped) /
            static_cast<double>(guest),
        static_cast<double>(fallback) / seconds,
        static_cast<unsigned long long>(total.guestFrames),
        static_cast<unsigned long long>(total.presentedFrames),
        static_cast<unsigned long long>(total.droppedFrames),
        static_cast<unsigned long long>(total.cpuFallbackFrames));
}

void NativeDisplayState::shutdown() noexcept
{
    if (!running.exchange(false, std::memory_order_acq_rel))
        return;
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
    return state_->framebuffer.width();
}

std::uint32_t NativeDisplay::height() const noexcept
{
    return state_->framebuffer.height();
}

void NativeDisplay::clear() noexcept
{
    state_->framebuffer.clear();
    state_->publishCpuFrame();
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
        static_cast<std::size_t>(sourceWidth) *
            protocol::gpuBytesPerPixel,
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
    const auto rectangle = state_->framebuffer.update(
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
        state_->publishCpuFrame();
}

void NativeDisplay::fillSelfTestPattern()
{
    state_->framebuffer.fillSelfTestPattern();
    state_->publishCpuFrame();
}

PresentOutcome NativeDisplay::presentOpenGl(const OpenGlScanout&) noexcept
{
    return PresentOutcome::unsupported;
}

PresentOutcome NativeDisplay::presentMetal(
    const MetalScanout& scanout) noexcept
{
    return state_->presentMetal(scanout);
}

DisplayStatistics NativeDisplay::statistics() const noexcept
{
    return state_->snapshot();
}

void NativeDisplay::recordCpuFallbackFrame() noexcept
{
    state_->cpuFallbackFrames.fetch_add(1, std::memory_order_relaxed);
}

} // namespace uml::gpu
