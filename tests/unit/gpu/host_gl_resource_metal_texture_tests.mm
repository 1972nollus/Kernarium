// SPDX-License-Identifier: GPL-2.0-or-later

// The native display presents a Virgl scanout by sampling the Metal texture
// that stores its GL texture, instead of reading the resource back into CPU
// memory. That is only correct if the renderer names the right texture: the
// one holding this resource at the resource's size and format, on the Metal
// device the display renders with, holding the pixels the guest stored once
// the guest's fence has retired, and stored in the orientation the renderer
// reports.
//
// The guest orders presentation with fences alone, so this test does too: it
// writes a pattern, waits for a fence, and then copies the named texture on a
// Metal queue of its own, as the display's draw does.

#include "virgl_renderer.hpp"

#import <Metal/Metal.h>

#include <sys/uio.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <source_location>
#include <vector>

namespace {

// virgl_hw.h and p_defines.h values the guest sends on the wire.
constexpr std::uint32_t pipeTexture2d = 2;
constexpr std::uint32_t virglFormatR8g8b8a8Unorm = 67;
constexpr std::uint32_t virglBindRenderTarget = 1U << 1U;
constexpr std::uint32_t virglBindSamplerView = 1U << 3U;
constexpr std::uint32_t virglResourceY0Top = 1U << 0U;

// Not square, so a transposed or reversed copy cannot look right.
constexpr std::uint32_t width = 16;
constexpr std::uint32_t height = 8;
constexpr std::size_t bytesPerPixel = 4;
constexpr std::size_t rowBytes = width * bytesPerPixel;

void require(
    bool condition,
    const char* description,
    const std::source_location location = std::source_location::current())
{
    if (condition)
        return;
    std::fprintf(
        stderr,
        "Resource Metal texture: %s (line %u)\n",
        description,
        location.line());
    std::exit(1);
}

[[nodiscard]] std::array<std::uint8_t, bytesPerPixel> guestPixel(
    std::uint32_t x,
    std::uint32_t y) noexcept
{
    return {
        static_cast<std::uint8_t>(x * 16U),
        static_cast<std::uint8_t>(y * 32U),
        static_cast<std::uint8_t>(0x80U + x + y),
        0xffU,
    };
}

void checkResource(
    uml::gpu::VirglRenderer& renderer,
    id<MTLDevice> device,
    std::uint32_t resourceId,
    bool y0Top,
    std::uint64_t fenceId)
{
    require(renderer.createResource3d(uml::gpu::VirglRenderer::Resource3d {
                .resourceId = resourceId,
                .target = pipeTexture2d,
                .format = virglFormatR8g8b8a8Unorm,
                .bind = virglBindRenderTarget | virglBindSamplerView,
                .width = width,
                .height = height,
                .depth = 1,
                .arraySize = 1,
                .flags = y0Top ? virglResourceY0Top : 0U,
            }) == 0,
        "the texture resource could not be created");
    std::vector<std::uint8_t> backing(rowBytes * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto pixel = guestPixel(x, y);
            std::memcpy(
                backing.data() + y * rowBytes + x * bytesPerPixel,
                pixel.data(),
                pixel.size());
        }
    }
    std::array<iovec, 1> vectors {
        iovec {.iov_base = backing.data(), .iov_len = backing.size()},
    };
    require(renderer.attachBacking(resourceId, vectors) == 0,
        "the guest backing could not be attached");
    require(renderer.transferResource(
                true,
                resourceId,
                0,
                0,
                0,
                0,
                uml::gpu::VirglRenderer::Box {
                    .width = width,
                    .height = height,
                    .depth = 1,
                },
                0) == 0,
        "the guest pixels could not be stored");
    require(renderer.createFenceAndWait(0, 0, fenceId, nullptr) == 0,
        "the fence after the store did not retire");

    uml::gpu::VirglRenderer::NativeTexture native;
    require(renderer.resourceMetalTexture(resourceId, native) == 0,
        "the renderer named no Metal texture for a GL texture resource");
    require(native.texture != nullptr, "the named texture is null");
    require(native.width == width && native.height == height,
        "the named texture does not have the resource's size");
    require(native.format == virglFormatR8g8b8a8Unorm,
        "the named texture does not have the resource's format");
    require(native.bottomUp == y0Top,
        "the reported orientation is not the resource's");

    id<MTLTexture> texture = (__bridge id<MTLTexture>)native.texture;
    require(texture.device == device,
        "the texture is not on the device the display renders with");
    // The display samples a 2D texture, or a 2D array of one layer, which
    // is what Zink asks KosmicKrisp for when a GL texture might gain layers.
    require((texture.textureType == MTLTextureType2D ||
             (texture.textureType == MTLTextureType2DArray &&
              texture.arrayLength == 1)) &&
            texture.sampleCount == 1,
        "the texture is not a single-sample, single-layer 2D texture");
    require(texture.width == width && texture.height == height,
        "the Metal texture does not have the resource's size");
    const bool bgra = texture.pixelFormat == MTLPixelFormatBGRA8Unorm;
    require(bgra || texture.pixelFormat == MTLPixelFormatRGBA8Unorm,
        "the texture does not store 8-bit RGBA channels");

    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> copy = [device newBufferWithLength:backing.size()
                                             options:MTLResourceStorageModeShared];
    require(queue != nil && copy != nil, "the copy could not be prepared");
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:texture
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(width, height, 1)
                   toBuffer:copy
          destinationOffset:0
     destinationBytesPerRow:rowBytes
   destinationBytesPerImage:backing.size()];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    require(commands.status == MTLCommandBufferStatusCompleted,
        "the texture copy failed");

    const auto* stored = static_cast<const std::uint8_t*>(copy.contents);
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint32_t y = y0Top ? height - 1U - row : row;
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto expected = guestPixel(x, y);
            const std::uint8_t* actual =
                stored + row * rowBytes + x * bytesPerPixel;
            const bool same = bgra
                ? actual[0] == expected[2] && actual[1] == expected[1] &&
                    actual[2] == expected[0] && actual[3] == expected[3]
                : std::memcmp(actual, expected.data(), expected.size()) == 0;
            if (!same) {
                std::fprintf(
                    stderr,
                    "Resource Metal texture: row %u column %u holds "
                    "%02x%02x%02x%02x, not image row %u\n",
                    row,
                    x,
                    actual[0],
                    actual[1],
                    actual[2],
                    actual[3],
                    y);
                std::exit(1);
            }
        }
    }

    renderer.detachBacking(resourceId);
    renderer.unrefResource(resourceId);
}

} // namespace

int main()
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            std::fputs("no Metal device; skipping\n", stderr);
            return 0;
        }
        std::unique_ptr<uml::gpu::VirglRenderer> renderer;
        try {
            renderer = std::make_unique<uml::gpu::VirglRenderer>();
        } catch (const std::exception& error) {
            std::fprintf(
                stderr,
                "Resource Metal texture: renderer unavailable: %s\n",
                error.what());
            return 0;
        }
        checkResource(*renderer, device, 1, false, 1);
        checkResource(*renderer, device, 2, true, 2);
        std::fputs("Resource Metal texture: ok\n", stderr);
    }
    return 0;
}
