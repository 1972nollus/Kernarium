// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_readback.hpp"

#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace uml::gpu {
namespace {

struct ReadbackRequest {
    id<MTLTexture> texture;
    protocol::GpuFormat format {};
    std::uint32_t sourceX {};
    std::uint32_t sourceY {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t destinationX {};
    std::uint32_t destinationY {};
};

[[nodiscard]] std::optional<protocol::GpuFormat>
readbackFormat(MTLPixelFormat format) noexcept
{
    switch (format) {
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
        return protocol::GpuFormat::b8g8r8a8Unorm;
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
        return protocol::GpuFormat::r8g8b8a8Unorm;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool rangeFits(
    std::uint32_t origin,
    std::uint32_t extent,
    std::uint32_t limit) noexcept
{
    return origin <= limit && extent <= limit - origin;
}

[[nodiscard]] std::optional<std::size_t> alignedRowBytes(
    std::uint32_t width,
    std::size_t alignment) noexcept
{
    if (alignment == 0) {
        return std::nullopt;
    }
    const auto bytes =
        static_cast<std::size_t>(width) * protocol::gpuBytesPerPixel;
    const auto remainder = bytes % alignment;
    if (remainder == 0)
        return bytes;
    const auto padding = alignment - remainder;
    if (bytes > std::numeric_limits<std::size_t>::max() - padding)
        return std::nullopt;
    return bytes + padding;
}

} // namespace

struct MetalReadbackState final :
    public std::enable_shared_from_this<MetalReadbackState> {
    explicit MetalReadbackState(MetalReadback::PublishCallback callback)
        : publish(std::move(callback))
    {
    }

    [[nodiscard]] std::optional<ReadbackRequest> request(
        const MetalScanout& scanout) noexcept
    {
        if (scanout.texture == nullptr || scanout.sourceWidth == 0 ||
            scanout.sourceHeight == 0 || scanout.width == 0 ||
            scanout.height == 0 || scanout.damageWidth == 0 ||
            scanout.damageHeight == 0 ||
            !rangeFits(
                scanout.sourceX, scanout.width, scanout.sourceWidth) ||
            !rangeFits(
                scanout.sourceY, scanout.height, scanout.sourceHeight) ||
            scanout.damageX < scanout.sourceX ||
            scanout.damageY < scanout.sourceY ||
            !rangeFits(
                scanout.damageX, scanout.damageWidth,
                scanout.sourceX + scanout.width) ||
            !rangeFits(
                scanout.damageY, scanout.damageHeight,
                scanout.sourceY + scanout.height)) {
            return std::nullopt;
        }

        id<MTLTexture> texture = (__bridge id<MTLTexture>)scanout.texture;
        const bool supportedType = texture != nil &&
            (texture.textureType == MTLTextureType2D ||
             (texture.textureType == MTLTextureType2DArray &&
              texture.arrayLength == 1));
        if (!supportedType || texture.sampleCount != 1 ||
            texture.width < scanout.sourceWidth ||
            texture.height < scanout.sourceHeight) {
            return std::nullopt;
        }
        const auto format = readbackFormat(texture.pixelFormat);
        if (!format)
            return std::nullopt;

        return ReadbackRequest {
            .texture = texture,
            .format = *format,
            .sourceX = scanout.damageX,
            .sourceY = scanout.damageY,
            .width = scanout.damageWidth,
            .height = scanout.damageHeight,
            .destinationX = scanout.damageX - scanout.sourceX,
            .destinationY = scanout.damageY - scanout.sourceY,
        };
    }

    [[nodiscard]] bool scheduleLocked(ReadbackRequest request) noexcept
    {
        @autoreleasepool {
            try {
                id<MTLDevice> requestDevice = request.texture.device;
                if (requestDevice == nil)
                    return false;
                if (device == nil) {
                    device = requestDevice;
                    commandQueue = [device newCommandQueue];
                    if (commandQueue == nil)
                        return false;
                } else if (requestDevice != device) {
                    return false;
                }

                const auto alignment = static_cast<std::size_t>(
                    [device minimumLinearTextureAlignmentForPixelFormat:
                        request.texture.pixelFormat]);
                const auto requestedRowBytes =
                    alignedRowBytes(request.width, alignment);
                if (!requestedRowBytes ||
                    request.height >
                        std::numeric_limits<std::size_t>::max() /
                            *requestedRowBytes) {
                    return false;
                }
                const auto requestedSize =
                    *requestedRowBytes * request.height;
                if (buffer == nil || buffer.length < requestedSize) {
                    buffer = [device
                        newBufferWithLength:requestedSize
                        options:MTLResourceStorageModeShared];
                    if (buffer == nil)
                        return false;
                }

                id<MTLCommandBuffer> commands =
                    [commandQueue commandBuffer];
                id<MTLBlitCommandEncoder> encoder =
                    [commands blitCommandEncoder];
                if (commands == nil || encoder == nil)
                    return false;
                [encoder
                    copyFromTexture:request.texture
                    sourceSlice:0
                    sourceLevel:0
                    sourceOrigin:MTLOriginMake(
                        request.sourceX, request.sourceY, 0)
                    sourceSize:MTLSizeMake(
                        request.width, request.height, 1)
                    toBuffer:buffer
                    destinationOffset:0
                    destinationBytesPerRow:*requestedRowBytes
                    destinationBytesPerImage:requestedSize];
                [encoder endEncoding];

                id<MTLBuffer> completedBuffer = buffer;
                auto state = shared_from_this();
                [commands addCompletedHandler:^(id<MTLCommandBuffer> result) {
                    state->complete(
                        result,
                        completedBuffer,
                        std::move(request),
                        *requestedRowBytes,
                        requestedSize);
                }];
                activeCommands = commands;
                inFlight = true;
                [commands commit];
                return true;
            } catch (...) {
                return false;
            }
        }
    }

    void complete(
        id<MTLCommandBuffer> commands,
        id<MTLBuffer> completedBuffer,
        ReadbackRequest request,
        std::size_t rowBytes,
        std::size_t size) noexcept
    {
        @autoreleasepool {
            bool mayPublish = false;
            {
                std::lock_guard lock(mutex);
                mayPublish = running &&
                    commands.status == MTLCommandBufferStatusCompleted;
                callbackActive = mayPublish;
            }
            if (mayPublish) {
                try {
                    publish(MetalReadbackFrame {
                        .pixels = std::span<const std::uint8_t>(
                            static_cast<const std::uint8_t*>(
                                completedBuffer.contents),
                            size),
                        .rowBytes = rowBytes,
                        .format = request.format,
                        .width = request.width,
                        .height = request.height,
                        .destinationX = request.destinationX,
                        .destinationY = request.destinationY,
                    });
                    presentedFrames.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    reportFailure();
                }
            } else if (commands.status != MTLCommandBufferStatusCompleted) {
                reportFailure();
            }

            std::lock_guard lock(mutex);
            callbackActive = false;
            activeCommands = nil;
            inFlight = false;
            if (!running || !pending) {
                stateChanged.notify_all();
                return;
            }
            auto next = std::move(*pending);
            pending.reset();
            if (!scheduleLocked(std::move(next)))
                reportFailure();
            stateChanged.notify_all();
        }
    }

    PresentOutcome present(const MetalScanout& scanout) noexcept
    {
        guestFrames.fetch_add(1, std::memory_order_relaxed);
        auto next = request(scanout);
        if (!next)
            return PresentOutcome::unsupported;

        std::lock_guard lock(mutex);
        if (!running)
            return PresentOutcome::unsupported;
        if (!inFlight) {
            if (!scheduleLocked(std::move(*next)))
                return PresentOutcome::unsupported;
            if (!directPathReported.exchange(
                    true, std::memory_order_relaxed)) {
                std::fputs(
                    "VNC display: asynchronous native Metal readback\n",
                    stderr);
            }
            return PresentOutcome::presented;
        }

        const bool superseded = pending.has_value();
        pending = std::move(*next);
        if (superseded)
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return superseded ? PresentOutcome::dropped
                          : PresentOutcome::presented;
    }

    void reportFailure() noexcept
    {
        if (!failureReported.exchange(true, std::memory_order_relaxed)) {
            std::fputs("VNC Metal readback failed\n", stderr);
        }
    }

    DisplayStatistics snapshot() const noexcept
    {
        return DisplayStatistics {
            .guestFrames = guestFrames.load(std::memory_order_relaxed),
            .presentedFrames =
                presentedFrames.load(std::memory_order_relaxed),
            .droppedFrames = droppedFrames.load(std::memory_order_relaxed),
            .cpuFallbackFrames =
                cpuFallbackFrames.load(std::memory_order_relaxed),
        };
    }

    void shutdown() noexcept
    {
        id<MTLCommandBuffer> commands;
        {
            std::lock_guard lock(mutex);
            if (!running)
                return;
            running = false;
            pending.reset();
            commands = activeCommands;
        }
        [commands waitUntilCompleted];
        std::unique_lock lock(mutex);
        stateChanged.wait(lock, [this] {
            return !inFlight && !callbackActive;
        });
        publish = {};
    }

    MetalReadback::PublishCallback publish;
    mutable std::mutex mutex;
    std::condition_variable stateChanged;
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLBuffer> buffer;
    id<MTLCommandBuffer> activeCommands;
    std::optional<ReadbackRequest> pending;
    bool running {true};
    bool inFlight {};
    bool callbackActive {};
    std::atomic_bool failureReported {};
    std::atomic_bool directPathReported {};
    std::atomic_uint64_t guestFrames {};
    std::atomic_uint64_t presentedFrames {};
    std::atomic_uint64_t droppedFrames {};
    std::atomic_uint64_t cpuFallbackFrames {};
};

MetalReadback::MetalReadback(PublishCallback publish)
    : state_(std::make_shared<MetalReadbackState>(std::move(publish)))
{
}

MetalReadback::~MetalReadback()
{
    shutdown();
}

PresentOutcome MetalReadback::present(const MetalScanout& scanout) noexcept
{
    return state_->present(scanout);
}

DisplayStatistics MetalReadback::statistics() const noexcept
{
    return state_->snapshot();
}

void MetalReadback::recordCpuFallbackFrame() noexcept
{
    state_->cpuFallbackFrames.fetch_add(1, std::memory_order_relaxed);
}

void MetalReadback::shutdown() noexcept
{
    if (state_)
        state_->shutdown();
}

} // namespace uml::gpu
