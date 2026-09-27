// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_presentation.hpp"

#include "display_activity_log.hpp"
#include "display_geometry.hpp"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
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

// Where the presenter samples the scanout: origin + point * extent, clamped
// to the centres of the scanout rectangle's edge texels, maps the guest's
// top-left-origin unit square onto the rectangle in its texture, with a
// bottom-up texture's flip folded into a negative vertical extent.
struct ScanoutSampling {
    float2 origin;
    float2 extent;
    float2 minimum;
    float2 maximum;
    uint rotation;
};

// Rotation turns the guest image onto the display, so a display point maps
// back through the inverse turn into the guest's unit square.
float2 scanoutCoordinate(float2 coordinate, constant ScanoutSampling& sampling)
{
    switch (sampling.rotation & 3u) {
    case 1u:
        coordinate = float2(coordinate.y, 1.0 - coordinate.x);
        break;
    case 2u:
        coordinate = float2(1.0 - coordinate.x, 1.0 - coordinate.y);
        break;
    case 3u:
        coordinate = float2(1.0 - coordinate.y, coordinate.x);
        break;
    default:
        break;
    }
    return clamp(
        sampling.origin + coordinate * sampling.extent,
        sampling.minimum,
        sampling.maximum);
}

// A guest pixel magnified onto the display stays a sharp block, so text and
// pixel art keep their edges at Retina and integer scales. A scanout larger
// than its viewport is minified bilinearly, which blends each display pixel's
// nearest 2x2 texels instead of dropping whole rows and columns. The guest's
// textures have no mip chain, so a reduction beyond 2:1 still skips texels.
constexpr sampler scanoutSampler(
    mag_filter::nearest,
    min_filter::linear,
    address::clamp_to_edge);

fragment float4 displayFragment(
    RasterData input [[stage_in]],
    texture2d<float> scanout [[texture(0)]],
    constant ScanoutSampling& sampling [[buffer(0)]])
{
    const float4 color = scanout.sample(
        scanoutSampler,
        scanoutCoordinate(input.textureCoordinate, sampling));
    return float4(color.rgb, 1.0);
}

fragment float4 displayArrayFragment(
    RasterData input [[stage_in]],
    texture2d_array<float> scanout [[texture(0)]],
    constant ScanoutSampling& sampling [[buffer(0)]])
{
    const float4 color = scanout.sample(
        scanoutSampler,
        scanoutCoordinate(input.textureCoordinate, sampling),
        0);
    return float4(color.rgb, 1.0);
}
)metal";

/* The shader's ScanoutSampling: float2 members are 8-byte aligned, and the
 * trailing uint pads the struct to a multiple of that alignment. */
struct ShaderScanoutSampling final {
    float origin[2];
    float extent[2];
    float minimum[2];
    float maximum[2];
    std::uint32_t rotation;
    std::uint32_t padding;
};
static_assert(sizeof(ShaderScanoutSampling) == 40);

[[nodiscard]] ShaderScanoutSampling shaderSampling(
    const ScanoutSampling& sampling,
    DisplayRotation rotation) noexcept
{
    return {
        .origin = {
            static_cast<float>(sampling.origin.x),
            static_cast<float>(sampling.origin.y)},
        .extent = {
            static_cast<float>(sampling.extent.x),
            static_cast<float>(sampling.extent.y)},
        .minimum = {
            static_cast<float>(sampling.minimum.x),
            static_cast<float>(sampling.minimum.y)},
        .maximum = {
            static_cast<float>(sampling.maximum.x),
            static_cast<float>(sampling.maximum.y)},
        .rotation = static_cast<std::uint32_t>(rotation),
        .padding = 0,
    };
}

/* The CPU fallback texture holds exactly the framebuffer, top row first. */
[[nodiscard]] std::optional<ScanoutSampling> framebufferSampling(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    return scanoutSampling({
        .texture = {.width = width, .height = height},
        .source = {.width = width, .height = height},
        .width = width,
        .height = height,
    });
}

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
        if (value == nullptr) {
            /* The display activity log wants the same counters, and asking
             * for it is an explicit opt-in already. */
            return displayActivityLog().enabled() ? 1.0 : 0.0;
        }
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
        , contentWidth(width)
        , contentHeight(height)
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
        id<MTLFunction> arrayFragment =
            [library newFunctionWithName:@"displayArrayFragment"];
        if (vertex == nil || fragment == nil || arrayFragment == nil)
            throw std::runtime_error("Metal display shader entry is missing");
        const auto makePipeline = [&](id<MTLFunction> selectedFragment) {
            MTLRenderPipelineDescriptor* descriptor =
                [[MTLRenderPipelineDescriptor alloc] init];
            descriptor.vertexFunction = vertex;
            descriptor.fragmentFunction = selectedFragment;
            descriptor.colorAttachments[0].pixelFormat =
                MTLPixelFormatBGRA8Unorm;
            return [device
                newRenderPipelineStateWithDescriptor:descriptor
                error:&error];
        };
        pipeline = makePipeline(fragment);
        arrayPipeline = makePipeline(arrayFragment);
        if (pipeline == nil || arrayPipeline == nil)
            throw std::runtime_error(
                "Metal display pipeline creation failed: " +
                description(error));

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
            ++latestContentGeneration;
            ++latestPresentationGeneration;
        }
        requestRedraw();
    }

    void drawableSizeChanged() noexcept
    {
        {
            std::lock_guard lock(presentationMutex);
            ++latestPresentationGeneration;
        }
        requestRedraw();
    }

    [[nodiscard]] DisplayRotation currentRotation() const noexcept
    {
        std::lock_guard lock(presentationMutex);
        return displayRotation;
    }

    [[nodiscard]] bool setRotation(DisplayRotation rotation) noexcept
    {
        {
            std::lock_guard lock(presentationMutex);
            if (displayRotation == rotation)
                return false;
            displayRotation = rotation;
            ++latestPresentationGeneration;
        }
        requestRedraw();
        return true;
    }

    [[nodiscard]] bool resize(
        std::uint32_t width,
        std::uint32_t height,
        bool shouldRequestRedraw) noexcept
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
                    contentWidth = width;
                    contentHeight = height;
                    latestTexture = nil;
                    latestIsCpu = true;
                    ++latestContentGeneration;
                    ++latestPresentationGeneration;
                }
                if (shouldRequestRedraw)
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
                scanout.texture == nullptr) {
                return PresentOutcome::unsupported;
            }
            id<MTLTexture> texture = (__bridge id<MTLTexture>)scanout.texture;
            const bool supportedType = texture != nil &&
                (texture.textureType == MTLTextureType2D ||
                 (texture.textureType == MTLTextureType2DArray &&
                  texture.arrayLength == 1));
            if (!supportedType || texture.device != device ||
                texture.width < scanout.sourceWidth ||
                texture.height < scanout.sourceHeight) {
                displayActivityLog().record(
                    "present display=native reject=texture handle=%p "
                    "type=%lu layers=%lu size=%lux%lu need=%ux%u "
                    "same-device=%d",
                    scanout.texture,
                    static_cast<unsigned long>(texture.textureType),
                    static_cast<unsigned long>(texture.arrayLength),
                    static_cast<unsigned long>(texture.width),
                    static_cast<unsigned long>(texture.height),
                    scanout.sourceWidth,
                    scanout.sourceHeight,
                    texture.device == device ? 1 : 0);
                return PresentOutcome::unsupported;
            }
            /* The pass scales the scanout rectangle into the drawable at the
             * rectangle's own aspect, whatever size the framebuffer has. */
            const auto sampling = scanoutSampling({
                .texture = {
                    .width = static_cast<std::uint32_t>(texture.width),
                    .height = static_cast<std::uint32_t>(texture.height)},
                .source = {
                    .width = scanout.sourceWidth,
                    .height = scanout.sourceHeight},
                .x = scanout.sourceX,
                .y = scanout.sourceY,
                .width = scanout.width,
                .height = scanout.height,
                .flipped = scanout.flipped,
            });
            if (!sampling) {
                displayActivityLog().record(
                    "present display=native reject=geometry texture=%p "
                    "source=%ux%u view=%ux%u+%u+%u",
                    scanout.texture,
                    scanout.sourceWidth,
                    scanout.sourceHeight,
                    scanout.width,
                    scanout.height,
                    scanout.sourceX,
                    scanout.sourceY);
                return PresentOutcome::unsupported;
            }
            bool superseded = false;
            {
                std::lock_guard lock(presentationMutex);
                superseded = latestContentGeneration >
                    presentedContentGeneration;
                latestTexture = texture;
                latestSampling = *sampling;
                latestIsCpu = false;
                ++latestContentGeneration;
                ++latestPresentationGeneration;
            }
            if (superseded)
                droppedFrames.fetch_add(1, std::memory_order_relaxed);
            if (!directPathReported.exchange(true, std::memory_order_relaxed)) {
                std::fputs(
                    "Native display: direct Metal texture presentation\n",
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

    void finishPresentation(
        std::uint64_t contentGeneration,
        std::uint64_t presentationGeneration) noexcept
    {
        bool pending = false;
        {
            std::lock_guard lock(presentationMutex);
            presentedContentGeneration = std::max(
                presentedContentGeneration, contentGeneration);
            presentedPresentationGeneration = std::max(
                presentedPresentationGeneration, presentationGeneration);
            presentationInFlight = false;
            pending = latestPresentationGeneration >
                presentedPresentationGeneration;
        }
        if (pending)
            requestRedraw();
    }

    void draw(void* nativeView) noexcept
    {
        @autoreleasepool {
            std::uint64_t contentGeneration = 0;
            std::uint64_t presentationGeneration = 0;
            std::optional<ScanoutSampling> activeSampling;
            DisplayRotation activeRotation = DisplayRotation::upright;
            bool useCpu = false;
            id<MTLTexture> sourceTexture;
            try {
                MTKView* view = (__bridge MTKView*)nativeView;
                if (view == nil)
                    return;
                {
                    std::lock_guard lock(presentationMutex);
                    if (presentationInFlight ||
                        latestPresentationGeneration <=
                            presentedPresentationGeneration) {
                        return;
                    }
                    contentGeneration = latestContentGeneration;
                    presentationGeneration = latestPresentationGeneration;
                    activeRotation = displayRotation;
                    useCpu = latestIsCpu;
                    activeSampling = useCpu ?
                        framebufferSampling(contentWidth, contentHeight) :
                        std::optional<ScanoutSampling> {latestSampling};
                    sourceTexture = useCpu ? fallbackTexture : latestTexture;
                    if (sourceTexture == nil || !activeSampling)
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
                        finishPresentation(
                            contentGeneration, presentationGeneration);
                        return;
                    }
                }
                const auto presented = presentedDisplaySize(
                    activeSampling->content, activeRotation);
                const auto fitted = aspectFitRectangle(
                    static_cast<double>(drawable.texture.width),
                    static_cast<double>(drawable.texture.height),
                    static_cast<double>(presented.width),
                    static_cast<double>(presented.height));
                if (fitted.width <= 0.0 || fitted.height <= 0.0) {
                    finishPresentation(
                        contentGeneration, presentationGeneration);
                    return;
                }
                id<MTLCommandBuffer> buffer = [commandQueue commandBuffer];
                if (buffer == nil) {
                    finishPresentation(
                        contentGeneration, presentationGeneration);
                    return;
                }
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].clearColor = view.clearColor;
                id<MTLRenderCommandEncoder> encoder =
                    [buffer renderCommandEncoderWithDescriptor:pass];
                if (encoder == nil) {
                    finishPresentation(
                        contentGeneration, presentationGeneration);
                    return;
                }
                const MTLViewport viewport {
                    .originX = fitted.x,
                    .originY = fitted.y,
                    .width = fitted.width,
                    .height = fitted.height,
                    .znear = 0.0,
                    .zfar = 1.0,
                };
                [encoder setRenderPipelineState:pipeline];
                if (sourceTexture.textureType == MTLTextureType2DArray)
                    [encoder setRenderPipelineState:arrayPipeline];
                [encoder setViewport:viewport];
                [encoder setFragmentTexture:sourceTexture atIndex:0];
                const auto sampling =
                    shaderSampling(*activeSampling, activeRotation);
                [encoder setFragmentBytes:&sampling
                                    length:sizeof(sampling)
                                   atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:0
                            vertexCount:3];
                [encoder endEncoding];
                [buffer presentDrawable:drawable];
                auto state = shared_from_this();
                [buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                    state->presentedFrames.fetch_add(
                        1, std::memory_order_relaxed);
                    state->finishPresentation(
                        contentGeneration, presentationGeneration);
                }];
                [buffer commit];
            } catch (...) {
                if (!renderFailureReported.exchange(
                        true, std::memory_order_relaxed)) {
                    std::fputs("Native Metal display failed\n", stderr);
                }
                if (presentationGeneration != 0) {
                    finishPresentation(
                        contentGeneration, presentationGeneration);
                }
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
        displayActivityLog().record(
            "stats display=native final guest=%llu presented=%llu "
            "dropped=%llu cpu-fallback=%llu",
            static_cast<unsigned long long>(total.guestFrames),
            static_cast<unsigned long long>(total.presentedFrames),
            static_cast<unsigned long long>(total.droppedFrames),
            static_cast<unsigned long long>(total.cpuFallbackFrames));
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
        displayActivityLog().record(
            "stats display=native guest=%llu presented=%llu dropped=%llu "
            "cpu-fallback=%llu",
            static_cast<unsigned long long>(total.guestFrames),
            static_cast<unsigned long long>(total.presentedFrames),
            static_cast<unsigned long long>(total.droppedFrames),
            static_cast<unsigned long long>(total.cpuFallbackFrames));
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
    id<MTLRenderPipelineState> arrayPipeline;
    id<MTLTexture> fallbackTexture;
    id<MTLTexture> latestTexture;
    ScanoutSampling latestSampling;
    std::mutex callbackMutex;
    std::function<void()> redrawCallback;
    mutable std::mutex presentationMutex;
    std::uint64_t latestContentGeneration {1};
    std::uint64_t presentedContentGeneration {};
    std::uint64_t latestPresentationGeneration {1};
    std::uint64_t presentedPresentationGeneration {};
    std::uint32_t contentWidth;
    std::uint32_t contentHeight;
    DisplayRotation displayRotation {DisplayRotation::upright};
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
    std::uint32_t height,
    bool requestRedraw) noexcept
{
    return state_->resize(width, height, requestRedraw);
}

DisplayRotation MetalPresentation::rotation() const noexcept
{
    return state_->currentRotation();
}

bool MetalPresentation::setRotation(DisplayRotation rotation) noexcept
{
    return state_->setRotation(rotation);
}

void MetalPresentation::drawableSizeChanged() noexcept
{
    state_->drawableSizeChanged();
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
