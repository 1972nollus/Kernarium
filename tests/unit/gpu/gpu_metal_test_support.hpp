// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "require.hpp"

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <source_location>
#include <span>
#include <vector>

/* Metal texture helpers for the Objective-C++ GPU tests. Images are BGRA8 with
 * tightly packed rows, and every failure aborts at the caller's line. */
namespace uml::gpu::testing {

/* A width x height image of one pixel. */
[[nodiscard]] inline std::vector<std::uint8_t> solidImage(
    const std::array<std::uint8_t, 4>& pixel,
    std::uint32_t width,
    std::uint32_t height)
{
    std::vector<std::uint8_t> image(
        static_cast<std::size_t>(width) * height * pixel.size());
    for (std::size_t offset = 0; offset < image.size(); offset += pixel.size())
        std::copy(pixel.begin(), pixel.end(), image.begin() + offset);
    return image;
}

/* A private texture holding `image`, which nothing on the host can read
 * without a blit, as a Venus scanout is. With `arrayTexture` it is a 2D array
 * of one layer. */
[[nodiscard]] inline id<MTLTexture> uploadPrivateTexture(
    id<MTLDevice> device,
    std::span<const std::uint8_t> image,
    std::uint32_t width,
    std::uint32_t height,
    bool arrayTexture = false,
    const std::source_location location = std::source_location::current())
{
    using ::uml::testing::require;
    const auto rowBytes = static_cast<std::size_t>(width) * 4U;
    require(image.size() == rowBytes * height,
        "the upload image does not match the texture", location);
    id<MTLBuffer> upload = [device
        newBufferWithBytes:image.data()
        length:image.size()
        options:MTLResourceStorageModeShared];
    require(upload != nil, "Metal upload buffer creation failed", location);

    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:width
        height:height
        mipmapped:NO];
    if (arrayTexture) {
        descriptor.textureType = MTLTextureType2DArray;
        descriptor.arrayLength = 1;
    }
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, "private Metal texture creation failed", location);

    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder = [commands blitCommandEncoder];
    require(
        queue != nil && commands != nil && encoder != nil,
        "Metal upload command creation failed",
        location);
    [encoder
        copyFromBuffer:upload
        sourceOffset:0
        sourceBytesPerRow:rowBytes
        sourceBytesPerImage:image.size()
        sourceSize:MTLSizeMake(width, height, 1)
        toTexture:texture
        destinationSlice:0
        destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    require(
        commands.status == MTLCommandBufferStatusCompleted,
        "private Metal texture upload failed",
        location);
    return texture;
}

/* Copies the top-left width x height texels of `texture`'s first layer into
 * rows of `rowBytes` and returns them. */
[[nodiscard]] inline std::vector<std::uint8_t> readTexture(
    id<MTLCommandQueue> queue,
    id<MTLTexture> texture,
    std::uint32_t width,
    std::uint32_t height,
    std::size_t rowBytes,
    const std::source_location location = std::source_location::current())
{
    using ::uml::testing::require;
    const auto byteCount = rowBytes * height;
    id<MTLBuffer> buffer = [queue.device
        newBufferWithLength:byteCount
        options:MTLResourceStorageModeShared];
    require(
        queue != nil && buffer != nil,
        "the Metal readback could not be prepared",
        location);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:texture
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(width, height, 1)
                   toBuffer:buffer
          destinationOffset:0
     destinationBytesPerRow:rowBytes
   destinationBytesPerImage:byteCount];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    require(
        commands.status == MTLCommandBufferStatusCompleted,
        "the Metal readback failed",
        location);
    const auto* bytes = static_cast<const std::uint8_t*>(buffer.contents);
    return {bytes, bytes + byteCount};
}

} // namespace uml::gpu::testing
