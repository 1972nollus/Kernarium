// SPDX-License-Identifier: GPL-2.0-or-later

#include "vnc_display.hpp"
#include "vnc_display_testing.hpp"

#import <Metal/Metal.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace uml::gpu::testing {
namespace {

constexpr std::array<std::uint8_t, 4> expectedPixel {
    0x19U, 0x38U, 0x57U, 0xffU};

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

} // namespace

void verifyPrivateMetalTextureReadback(VncDisplay& display)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "Metal device unavailable");

        const auto width = display.width();
        const auto height = display.height();
        const auto rowBytes =
            static_cast<std::size_t>(width) * protocol::gpuBytesPerPixel;
        const auto byteCount = rowBytes * height;
        id<MTLBuffer> upload = [device
            newBufferWithLength:byteCount
            options:MTLResourceStorageModeShared];
        require(upload != nil, "Metal upload buffer creation failed");
        auto* bytes = static_cast<std::uint8_t*>(upload.contents);
        for (std::size_t offset = 0; offset < byteCount; offset += 4U) {
            bytes[offset] = expectedPixel[0];
            bytes[offset + 1U] = expectedPixel[1];
            bytes[offset + 2U] = expectedPixel[2];
            bytes[offset + 3U] = expectedPixel[3];
        }

        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:width
            height:height
            mipmapped:NO];
        descriptor.textureType = MTLTextureType2DArray;
        descriptor.arrayLength = 1;
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage =
            MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> texture =
            [device newTextureWithDescriptor:descriptor];
        require(texture != nil, "private Metal texture creation failed");

        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder =
            [commands blitCommandEncoder];
        require(queue != nil && commands != nil && encoder != nil,
                "Metal upload command creation failed");
        [encoder
            copyFromBuffer:upload
            sourceOffset:0
            sourceBytesPerRow:rowBytes
            sourceBytesPerImage:byteCount
            sourceSize:MTLSizeMake(width, height, 1)
            toTexture:texture
            destinationSlice:0
            destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        require(commands.status == MTLCommandBufferStatusCompleted,
                "private Metal texture upload failed");

        constexpr std::uint32_t damageX = 7;
        constexpr std::uint32_t damageY = 11;
        constexpr std::uint32_t damageWidth = 13;
        constexpr std::uint32_t damageHeight = 9;
        require(presentAccepted(display.presentMetal(MetalScanout {
                    .texture = (__bridge void*)texture,
                    .sourceWidth = width,
                    .sourceHeight = height,
                    .sourceX = 0,
                    .sourceY = 0,
                    .width = width,
                    .height = height,
                    .damageX = damageX,
                    .damageY = damageY,
                    .damageWidth = damageWidth,
                    .damageHeight = damageHeight,
                })),
                "private Metal texture readback was rejected");

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        while (display.statistics().presentedFrames == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(display.statistics().presentedFrames == 1,
                "private Metal texture readback did not complete");
        require(display.pixelForTesting(damageX, damageY) == expectedPixel,
                "private Metal texture readback pixel mismatch");
        require(display.pixelForTesting(0, 0) ==
                    std::array<std::uint8_t, 4> {},
                "Metal readback exceeded its damage rectangle");
    }
}

} // namespace uml::gpu::testing
