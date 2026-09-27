// SPDX-License-Identifier: GPL-2.0-or-later

/* A guest mode need not match the display's framebuffer: glmark2 sets
 * 4096x2160 on a 1280x720 display, and a scanout can show a rectangle of a
 * larger source. The native presenter scales such a scanout in its own Metal
 * pass instead of refusing it into the CPU fallback. Each case presents a
 * texture with one colour per quadrant and reads back the drawable that the
 * presenter drew into. */

#include "metal_presentation.hpp"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <thread>
#include <vector>

using uml::gpu::DisplayRotation;
using uml::gpu::MetalPresentation;
using uml::gpu::MetalScanout;
using uml::gpu::PresentOutcome;

@interface PresentationTestDelegate : NSObject <MTKViewDelegate>
@property(nonatomic) MetalPresentation* presentation;
@property(nonatomic, strong) id<CAMetalDrawable> drawable;
@end

@implementation PresentationTestDelegate
- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    (void)view;
    (void)size;
}

- (void)drawInMTKView:(MTKView*)view
{
    self.drawable = view.currentDrawable;
    self.presentation->draw((__bridge void*)view);
}
@end

namespace {

constexpr int skipped = 77;
constexpr std::uint32_t framebufferWidth = 64;
constexpr std::uint32_t framebufferHeight = 36;
constexpr std::uint32_t sourceWidth = 128;
constexpr std::uint32_t sourceHeight = 72;

using Pixel = std::array<std::uint8_t, 4>;
/* BGRA, one colour per quadrant of the source. */
constexpr Pixel red {0, 0, 255, 255};
constexpr Pixel green {0, 255, 0, 255};
constexpr Pixel blue {255, 0, 0, 255};
constexpr Pixel white {255, 255, 255, 255};
constexpr Pixel black {0, 0, 0, 255};

void require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "metal-presentation: %s\n", message);
        std::_Exit(1);
    }
}

id<MTLTexture> makeQuadrantTexture(id<MTLDevice> device)
{
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:sourceWidth
        height:sourceHeight
        mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, "could not create the source texture");
    std::vector<std::uint8_t> pixels(sourceWidth * sourceHeight * 4);
    for (std::uint32_t y = 0; y < sourceHeight; ++y) {
        for (std::uint32_t x = 0; x < sourceWidth; ++x) {
            const bool right = x >= sourceWidth / 2;
            const bool bottom = y >= sourceHeight / 2;
            const Pixel& colour = bottom ? (right ? white : blue) :
                                           (right ? green : red);
            std::memcpy(&pixels[(y * sourceWidth + x) * 4], colour.data(), 4);
        }
    }
    [texture replaceRegion:MTLRegionMake2D(0, 0, sourceWidth, sourceHeight)
               mipmapLevel:0
                 withBytes:pixels.data()
               bytesPerRow:sourceWidth * 4];
    return texture;
}

class Harness final {
public:
    Harness(MetalPresentation& presentation, id<MTLTexture> source)
        : presentation_(presentation)
        , source_(source)
    {
        id<MTLDevice> device =
            (__bridge id<MTLDevice>)presentation.device();
        queue_ = [device newCommandQueue];
        view_ = [[MTKView alloc]
            initWithFrame:NSMakeRect(0, 0, framebufferWidth, framebufferHeight)
            device:device];
        view_.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        view_.framebufferOnly = NO;
        view_.paused = YES;
        view_.enableSetNeedsDisplay = NO;
        view_.autoResizeDrawable = NO;
        view_.drawableSize = CGSizeMake(framebufferWidth, framebufferHeight);
        view_.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        delegate_ = [[PresentationTestDelegate alloc] init];
        delegate_.presentation = &presentation;
        view_.delegate = delegate_;
    }

    [[nodiscard]] PresentOutcome present(MetalScanout scanout)
    {
        scanout.texture = (__bridge void*)source_;
        scanout.sourceWidth = scanout.sourceWidth == 0 ?
            sourceWidth : scanout.sourceWidth;
        scanout.sourceHeight = scanout.sourceHeight == 0 ?
            sourceHeight : scanout.sourceHeight;
        scanout.damageWidth = scanout.width;
        scanout.damageHeight = scanout.height;
        return presentation_.present(scanout);
    }

    /* Draws the newest frame and returns the drawable's pixels, or nothing
     * when this host gives an offscreen view no drawables. A draw can find
     * the previous frame still retiring, so it is retried until a new frame
     * completes. */
    [[nodiscard]] bool draw(std::vector<std::uint8_t>& pixels)
    {
        const auto target = presentation_.statistics().presentedFrames + 1;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            delegate_.drawable = nil;
            [view_ draw];
            if (delegate_.drawable == nil)
                return false;
            const auto frameDeadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(1);
            while (presentation_.statistics().presentedFrames < target &&
                   std::chrono::steady_clock::now() < frameDeadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (presentation_.statistics().presentedFrames >= target) {
                readBack(delegate_.drawable.texture, pixels);
                return true;
            }
        }
        require(false, "the presenter never completed a frame");
        return false;
    }

private:
    void readBack(id<MTLTexture> texture, std::vector<std::uint8_t>& pixels)
    {
        require(texture.width == framebufferWidth &&
                texture.height == framebufferHeight,
            "the drawable is not the requested size");
        const NSUInteger rowBytes = framebufferWidth * 4;
        id<MTLBuffer> buffer = [queue_.device
            newBufferWithLength:rowBytes * framebufferHeight
            options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> commands = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        [blit copyFromTexture:texture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(
                       framebufferWidth, framebufferHeight, 1)
                     toBuffer:buffer
            destinationOffset:0
       destinationBytesPerRow:rowBytes
     destinationBytesPerImage:rowBytes * framebufferHeight];
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        require(commands.status == MTLCommandBufferStatusCompleted,
            "the drawable readback failed");
        const auto* bytes = static_cast<const std::uint8_t*>(buffer.contents);
        pixels.assign(bytes, bytes + rowBytes * framebufferHeight);
    }

    MetalPresentation& presentation_;
    id<MTLTexture> source_;
    id<MTLCommandQueue> queue_;
    MTKView* view_;
    PresentationTestDelegate* delegate_;
};

[[nodiscard]] bool pixelIs(
    const std::vector<std::uint8_t>& pixels,
    std::uint32_t x,
    std::uint32_t y,
    const Pixel& expected)
{
    const auto* pixel = &pixels[(y * framebufferWidth + x) * 4];
    for (std::size_t channel = 0; channel < expected.size(); ++channel) {
        const int difference = static_cast<int>(pixel[channel]) -
            static_cast<int>(expected[channel]);
        if (difference < -2 || difference > 2) {
            std::fprintf(stderr,
                "metal-presentation: pixel %u,%u is %u,%u,%u,%u, expected "
                "%u,%u,%u,%u\n",
                x, y, pixel[0], pixel[1], pixel[2], pixel[3],
                expected[0], expected[1], expected[2], expected[3]);
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool accepted(PresentOutcome outcome)
{
    return outcome != PresentOutcome::unsupported;
}

int run()
{
    std::unique_ptr<MetalPresentation> presentation;
    try {
        presentation = std::make_unique<MetalPresentation>(
            framebufferWidth, framebufferHeight);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "metal-presentation: skipped: %s\n", error.what());
        return skipped;
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)presentation->device();
    Harness harness {*presentation, makeQuadrantTexture(device)};

    /* Rejected before any pixel check so a host without offscreen drawables
     * still answers whether the presenter takes the frame. */
    require(accepted(harness.present({
                .width = sourceWidth, .height = sourceHeight})),
        "a scanout larger than the framebuffer was refused");
    require(!accepted(harness.present({
                .sourceX = 100, .width = 64, .height = 36})),
        "a scanout reaching past its source was accepted");
    require(!accepted(harness.present({
                .sourceWidth = sourceWidth * 2,
                .sourceHeight = sourceHeight * 2,
                .width = sourceWidth * 2,
                .height = sourceHeight * 2})),
        "a texture smaller than its source was accepted");

    std::vector<std::uint8_t> pixels;
    require(accepted(harness.present({
                .width = sourceWidth, .height = sourceHeight})),
        "a scanout larger than the framebuffer was refused");
    if (!harness.draw(pixels)) {
        std::fputs(
            "metal-presentation: skipped pixel checks: no offscreen drawable\n",
            stderr);
        return skipped;
    }
    require(pixelIs(pixels, 16, 9, red) && pixelIs(pixels, 48, 9, green) &&
            pixelIs(pixels, 16, 27, blue) && pixelIs(pixels, 48, 27, white),
        "a scanout larger than the framebuffer was not scaled into it");

    /* Columns 16-79 of the top half: the red-green boundary at column 64
     * lands three quarters of the way across. */
    require(accepted(harness.present({
                .sourceX = 16,
                .width = sourceWidth / 2,
                .height = sourceHeight / 2})),
        "a scanout rectangle inside its source was refused");
    require(harness.draw(pixels), "the sub-rectangle frame was not drawn");
    require(pixelIs(pixels, 0, 0, red) && pixelIs(pixels, 38, 18, red) &&
            pixelIs(pixels, 56, 18, green) && pixelIs(pixels, 63, 35, green),
        "a scanout rectangle did not show only its own pixels");

    require(accepted(harness.present({
                .width = sourceWidth,
                .height = sourceHeight,
                .flipped = true})),
        "a bottom-up scanout larger than the framebuffer was refused");
    require(harness.draw(pixels), "the bottom-up frame was not drawn");
    require(pixelIs(pixels, 16, 9, blue) && pixelIs(pixels, 48, 9, white) &&
            pixelIs(pixels, 16, 27, red) && pixelIs(pixels, 48, 27, green),
        "a bottom-up scanout was not turned the right way up");

    /* The guest's top-left quadrant of a bottom-up source is stored in the
     * texture's bottom half. */
    require(accepted(harness.present({
                .width = sourceWidth / 2,
                .height = sourceHeight / 2,
                .flipped = true})),
        "a bottom-up scanout rectangle was refused");
    require(harness.draw(pixels), "the bottom-up rectangle was not drawn");
    require(pixelIs(pixels, 0, 0, blue) && pixelIs(pixels, 63, 35, blue),
        "a bottom-up scanout rectangle showed the wrong texels");

    /* A rectangle keeps its own aspect: the source's top half letterboxes
     * into the framebuffer's 16:9 drawable rather than stretching over it. */
    require(accepted(harness.present({
                .width = sourceWidth, .height = sourceHeight / 2})),
        "a scanout with the framebuffer's width and half its aspect was "
        "refused");
    require(harness.draw(pixels), "the letterboxed frame was not drawn");
    require(pixelIs(pixels, 16, 4, black) && pixelIs(pixels, 16, 31, black) &&
            pixelIs(pixels, 16, 18, red) && pixelIs(pixels, 48, 18, green),
        "a scanout rectangle was not aspect-fitted at its own aspect");

    /* A clockwise quarter-turn shows the guest's top-left at the display's
     * top-right, pillarboxed at the rotated aspect. */
    require(presentation->setRotation(DisplayRotation::clockwise90),
        "rotation did not change");
    require(accepted(harness.present({
                .width = sourceWidth, .height = sourceHeight})),
        "a rotated scanout was refused");
    require(harness.draw(pixels), "the rotated frame was not drawn");
    require(pixelIs(pixels, 2, 18, black) && pixelIs(pixels, 61, 18, black) &&
            pixelIs(pixels, 24, 2, blue) && pixelIs(pixels, 40, 2, red) &&
            pixelIs(pixels, 24, 33, white) && pixelIs(pixels, 40, 33, green),
        "a rotated scanout was not scaled at its rotated aspect");

    presentation->shutdown();
    return EXIT_SUCCESS;
}

} // namespace

int main()
{
    @autoreleasepool {
        return run();
    }
}
