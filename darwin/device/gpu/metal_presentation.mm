// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_presentation.hpp"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace uml::gpu {
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

struct MetalPresentationState final :
    public std::enable_shared_from_this<MetalPresentationState> {
    MetalPresentationState(std::uint32_t width, std::uint32_t height)
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

        fallbackTexture = makeFallbackTexture(width, height);
        if (fallbackTexture == nil)
            throw std::runtime_error("Metal fallback texture creation failed");
    }

    [[nodiscard]] id<MTLTexture> makeFallbackTexture(
        std::uint32_t width,
        std::uint32_t height) const noexcept
    {
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                width:width
                height:height
                mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageShaderRead;
        return [device newTextureWithDescriptor:descriptor];
    }

    void requestRedraw() noexcept
    {
        std::function<void()> callback;
        try {
            std::lock_guard lock(callbackMutex);
            callback = redrawCallback;
        } catch (...) {
            return;
        }
        if (callback) {
            try {
                callback();
            } catch (...) {
            }
        }
    }

    void publishCpuFrame() noexcept
    {
        {
            std::lock_guard lock(presentationMutex);
            latestTexture = nil;
            latestIsCpu = true;
            ++latestGeneration;
        }
        requestRedraw();
    }

    [[nodiscard]] bool resize(
        std::uint32_t width,
        std::uint32_t height) noexcept
    {
        @autoreleasepool {
            try {
                if (width == framebuffer.width() &&
                    height == framebuffer.height()) {
                    return false;
                }
                id<MTLTexture> replacement =
                    makeFallbackTexture(width, height);
                if (replacement == nil)
                    throw std::runtime_error(
                        "Metal fallback resize allocation failed");
                {
                    /* A CPU draw verifies and uploads its fallback texture
                     * under this same lock, so framebuffer storage and the
                     * matching Metal allocation change as one snapshot. */
                    std::lock_guard lock(presentationMutex);
                    framebuffer.resize(width, height);
                    fallbackTexture = replacement;
                    latestTexture = nil;
                    latestIsCpu = true;
                    ++latestGeneration;
                }
                requestRedraw();
                return true;
            } catch (...) {
                if (!resizeFailureReported.exchange(
                        true, std::memory_order_relaxed)) {
                    std::fputs("Native Metal display resize failed\n", stderr);
                }
                return false;
            }
        }
    }

    PresentOutcome present(const MetalScanout& scanout) noexcept
    {
        guestFrames.fetch_add(1, std::memory_order_relaxed);
        try {
            if (!running.load(std::memory_order_acquire) ||
                scanout.texture == nullptr || scanout.sourceX != 0 ||
                scanout.sourceY != 0 ||
                scanout.width != scanout.sourceWidth ||
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

    void finishPresentation(std::uint64_t generation) noexcept
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

    void draw(void* nativeView) noexcept
    {
        @autoreleasepool {
            std::uint64_t generation = 0;
            bool useCpu = false;
            id<MTLTexture> sourceTexture;
            try {
                MTKView* view = (__bridge MTKView*)nativeView;
                if (view == nil)
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
                /* Acquiring a drawable can block until the display releases
                 * one, and this runs on the thread that owns the user
                 * interface. Claim the frame first so a redraw with nothing
                 * new to show never pays for it. */
                MTLRenderPassDescriptor* pass =
                    view.currentRenderPassDescriptor;
                id<CAMetalDrawable> drawable = view.currentDrawable;
                if (pass == nil || drawable == nil) {
                    /* Release the claim without retiring the generation: the
                     * frame was not shown, and the next guest frame asks for
                     * this one again. */
                    std::lock_guard lock(presentationMutex);
                    presentationInFlight = false;
                    return;
                }
                if (useCpu) {
                    bool currentFallback = false;
                    {
                        std::lock_guard presentationLock(presentationMutex);
                        currentFallback = sourceTexture == fallbackTexture;
                        if (currentFallback) {
                            std::lock_guard framebufferLock(framebuffer);
                            const MTLRegion region = MTLRegionMake2D(
                                0,
                                0,
                                framebuffer.width(),
                                framebuffer.height());
                            [sourceTexture
                                replaceRegion:region
                                mipmapLevel:0
                                withBytes:framebuffer.pixels().data()
                                bytesPerRow:framebuffer.rowBytes()];
                        }
                    }
                    if (!currentFallback) {
                        finishPresentation(generation);
                        return;
                    }
                }
                id<MTLCommandBuffer> buffer = [commandQueue commandBuffer];
                if (buffer == nil) {
                    finishPresentation(generation);
                    return;
                }
                id<MTLRenderCommandEncoder> encoder =
                    [buffer renderCommandEncoderWithDescriptor:pass];
                if (encoder == nil) {
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
                    state->presentedFrames.fetch_add(
                        1, std::memory_order_relaxed);
                    state->finishPresentation(generation);
                }];
                [buffer commit];
            } catch (...) {
                if (!renderFailureReported.exchange(
                        true, std::memory_order_relaxed)) {
                    std::fputs("Native Metal display failed\n", stderr);
                }
                if (generation != 0)
                    finishPresentation(generation);
            }
        }
    }

    DisplayStatistics snapshot() const noexcept
    {
        return DisplayStatistics {
            .guestFrames = guestFrames.load(std::memory_order_relaxed),
            .presentedFrames = presentedFrames.load(std::memory_order_relaxed),
            .droppedFrames = droppedFrames.load(std::memory_order_relaxed),
            .cpuFallbackFrames =
                cpuFallbackFrames.load(std::memory_order_relaxed),
        };
    }

    void reportFinal() const noexcept
    {
        if (frameStatisticsInterval() <= 0.0)
            return;
        const DisplayStatistics total = snapshot();
        std::fprintf(
            stderr,
            "Native display final: guest %llu presented %llu dropped %llu "
            "cpu-fallback %llu\n",
            static_cast<unsigned long long>(total.guestFrames),
            static_cast<unsigned long long>(total.presentedFrames),
            static_cast<unsigned long long>(total.droppedFrames),
            static_cast<unsigned long long>(total.cpuFallbackFrames));
    }

    void maybeReport() noexcept
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
        if (static_cast<double>(span) < interval * 1e9 ||
            !reportedAt.compare_exchange_strong(
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
            "Native display frames: guest %.1f/s presented %.1f/s dropped "
            "%.1f/s (%.1f%%) cpu-fallback %.1f/s | totals guest %llu "
            "presented %llu dropped %llu cpu-fallback %llu\n",
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

    ScanoutFramebuffer framebuffer;
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLRenderPipelineState> pipeline;
    id<MTLTexture> fallbackTexture;
    id<MTLTexture> latestTexture;
    std::mutex callbackMutex;
    std::function<void()> redrawCallback;
    std::mutex presentationMutex;
    std::uint64_t latestGeneration {1};
    std::uint64_t presentedGeneration {};
    bool latestIsCpu {true};
    bool presentationInFlight {};
    std::atomic_bool running {true};
    std::atomic_bool directPathReported {};
    std::atomic_bool renderFailureReported {};
    std::atomic_bool resizeFailureReported {};
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

MetalPresentation::MetalPresentation(
    std::uint32_t width, std::uint32_t height)
    : state_(std::make_shared<MetalPresentationState>(width, height))
{
}

MetalPresentation::~MetalPresentation()
{
    shutdown();
}

void MetalPresentation::setRedrawCallback(std::function<void()> callback)
{
    std::lock_guard lock(state_->callbackMutex);
    state_->redrawCallback = std::move(callback);
}

void* MetalPresentation::device() const noexcept
{
    return (__bridge void*)state_->device;
}

ScanoutFramebuffer& MetalPresentation::framebuffer() noexcept
{
    return state_->framebuffer;
}

const ScanoutFramebuffer& MetalPresentation::framebuffer() const noexcept
{
    return state_->framebuffer;
}

bool MetalPresentation::resize(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    return state_->resize(width, height);
}

void MetalPresentation::publishCpuFrame() noexcept
{
    state_->publishCpuFrame();
}

PresentOutcome MetalPresentation::present(
    const MetalScanout& scanout) noexcept
{
    return state_->present(scanout);
}

void MetalPresentation::draw(void* nativeView) noexcept
{
    state_->draw(nativeView);
}

DisplayStatistics MetalPresentation::statistics() const noexcept
{
    return state_->snapshot();
}

void MetalPresentation::recordCpuFallbackFrame() noexcept
{
    state_->cpuFallbackFrames.fetch_add(1, std::memory_order_relaxed);
}

void MetalPresentation::shutdown() noexcept
{
    if (!state_ ||
        !state_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    state_->reportFinal();
    std::lock_guard lock(state_->callbackMutex);
    state_->redrawCallback = {};
}

} // namespace uml::gpu
