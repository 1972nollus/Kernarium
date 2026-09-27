// SPDX-License-Identifier: GPL-2.0-or-later

/* The Venus scanout is a private Metal texture with no guest-visible Virgl
 * resource, so a VNC client can only ever see it through the display's native
 * readback. `gpu-vnc-lifetime` pins that readback's pixels, but it compiles
 * the display source into a test target with its own definitions, so it
 * cannot tell whether the shipped emulator contains that path at all. This
 * test links the production display library and reads the pixels back over a
 * real RFB connection, which is the contract a guest actually depends on. It
 * also pins what a client sees when it connects after scanouts were dropped
 * because nobody was watching: the newest scanout, never an older frame. */

#include "display.hpp"

#import <Metal/Metal.h>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uint32_t displayExtent = 64;
constexpr std::uint32_t cursorExtent = 32;
using Pixel = std::array<std::uint8_t, 4>;
constexpr Pixel expectedPixel {0x19U, 0x38U, 0x57U, 0xffU};
constexpr Pixel idlePixel {0x6aU, 0x21U, 0xc4U, 0xffU};
constexpr int timeoutSeconds = 10;
constexpr auto operationTimeout = std::chrono::seconds(timeoutSeconds);

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class Connection final {
public:
    explicit Connection(std::uint16_t port)
        : descriptor_(::socket(AF_INET, SOCK_STREAM, 0))
    {
        require(descriptor_ >= 0, "VNC test socket failed");
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(
            ::connect(
                descriptor_,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0,
            "VNC test connect failed");
        const timeval timeout {.tv_sec = timeoutSeconds, .tv_usec = 0};
        require(
            ::setsockopt(
                descriptor_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                sizeof(timeout)) == 0,
            "VNC test receive timeout failed");
    }

    ~Connection()
    {
        if (descriptor_ >= 0)
            ::close(descriptor_);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void send(const void* bytes, std::size_t size)
    {
        const auto* cursor = static_cast<const std::uint8_t*>(bytes);
        while (size != 0) {
            const auto written = ::send(descriptor_, cursor, size, 0);
            require(written > 0, "VNC test send failed");
            cursor += written;
            size -= static_cast<std::size_t>(written);
        }
    }

    std::vector<std::uint8_t> receive(std::size_t size)
    {
        std::vector<std::uint8_t> bytes(size);
        std::size_t filled = 0;
        while (filled < size) {
            const auto read = ::recv(
                descriptor_, bytes.data() + filled, size - filled, 0);
            require(read > 0, "VNC test receive failed");
            filled += static_cast<std::size_t>(read);
        }
        return bytes;
    }

    /* Enough of RFB 3.8 to reach pixels: no security, shared session, and the
     * little-endian 32-bit BGRX format the raw encoding then delivers. */
    void handshake()
    {
        const auto version = receive(12);
        require(
            std::memcmp(version.data(), "RFB ", 4) == 0,
            "VNC test server is not an RFB server");
        send("RFB 003.008\n", 12);
        const auto offered = receive(1);
        require(offered[0] != 0, "VNC test server refused the handshake");
        const auto types = receive(offered[0]);
        require(
            std::find(types.begin(), types.end(), std::uint8_t {1}) !=
                types.end(),
            "VNC test server requires authentication");
        const std::uint8_t none = 1;
        send(&none, 1);
        const auto result = receive(4);
        require(
            result[0] == 0 && result[1] == 0 && result[2] == 0 &&
                result[3] == 0,
            "VNC test security handshake failed");
        const std::uint8_t shared = 1;
        send(&shared, 1);
        const auto initial = receive(24);
        width_ = static_cast<std::uint32_t>(initial[0]) << 8U | initial[1];
        height_ = static_cast<std::uint32_t>(initial[2]) << 8U | initial[3];
        const auto nameLength =
            static_cast<std::size_t>(initial[20]) << 24U |
            static_cast<std::size_t>(initial[21]) << 16U |
            static_cast<std::size_t>(initial[22]) << 8U |
            static_cast<std::size_t>(initial[23]);
        (void)receive(nameLength);
        const std::array<std::uint8_t, 20> pixelFormat {
            0, 0, 0, 0, 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0,
            0, 0, 0};
        send(pixelFormat.data(), pixelFormat.size());
        const std::array<std::uint8_t, 8> encodings {
            2, 0, 0, 1, 0, 0, 0, 0};
        send(encodings.data(), encodings.size());
    }

    /* One non-incremental full-screen request, then the rectangles it
     * answers with, composited into a BGRX image of the whole framebuffer. */
    std::vector<std::uint8_t> readFramebuffer()
    {
        const std::array<std::uint8_t, 10> request {
            3,
            0,
            0,
            0,
            0,
            0,
            static_cast<std::uint8_t>(width_ >> 8U),
            static_cast<std::uint8_t>(width_),
            static_cast<std::uint8_t>(height_ >> 8U),
            static_cast<std::uint8_t>(height_),
        };
        send(request.data(), request.size());
        std::vector<std::uint8_t> image(
            static_cast<std::size_t>(width_) * height_ * 4U);
        const auto deadline =
            std::chrono::steady_clock::now() + operationTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto kind = receive(1);
            require(kind[0] == 0, "unexpected RFB server message");
            const auto header = receive(3);
            const auto rectangles =
                static_cast<std::uint32_t>(header[1]) << 8U | header[2];
            for (std::uint32_t index = 0; index < rectangles; ++index) {
                const auto descriptor = receive(12);
                const auto x = static_cast<std::uint32_t>(descriptor[0]) << 8U |
                    descriptor[1];
                const auto y = static_cast<std::uint32_t>(descriptor[2]) << 8U |
                    descriptor[3];
                const auto rectangleWidth =
                    static_cast<std::uint32_t>(descriptor[4]) << 8U |
                    descriptor[5];
                const auto rectangleHeight =
                    static_cast<std::uint32_t>(descriptor[6]) << 8U |
                    descriptor[7];
                require(
                    descriptor[8] == 0 && descriptor[9] == 0 &&
                        descriptor[10] == 0 && descriptor[11] == 0,
                    "VNC test server did not use the raw encoding");
                const auto pixels = receive(
                    static_cast<std::size_t>(rectangleWidth) *
                    rectangleHeight * 4U);
                for (std::uint32_t row = 0; row < rectangleHeight; ++row) {
                    std::memcpy(
                        image.data() +
                            ((static_cast<std::size_t>(y) + row) * width_ + x) *
                                4U,
                        pixels.data() +
                            static_cast<std::size_t>(row) * rectangleWidth * 4U,
                        static_cast<std::size_t>(rectangleWidth) * 4U);
                }
            }
            if (rectangles != 0)
                return image;
        }
        throw std::runtime_error("no VNC framebuffer update arrived");
    }

private:
    int descriptor_ {-1};
    std::uint32_t width_ {};
    std::uint32_t height_ {};
};

id<MTLTexture> makePrivateScanoutTexture(
    id<MTLDevice> device,
    const Pixel& pixel)
{
    const auto rowBytes = static_cast<std::size_t>(displayExtent) *
        uml::gpu::protocol::gpuBytesPerPixel;
    const auto byteCount = rowBytes * displayExtent;
    id<MTLBuffer> upload = [device
        newBufferWithLength:byteCount
        options:MTLResourceStorageModeShared];
    require(upload != nil, "Metal upload buffer creation failed");
    auto* bytes = static_cast<std::uint8_t*>(upload.contents);
    for (std::size_t offset = 0; offset < byteCount; offset += 4U) {
        bytes[offset] = pixel[0];
        bytes[offset + 1U] = pixel[1];
        bytes[offset + 2U] = pixel[2];
        bytes[offset + 3U] = pixel[3];
    }

    /* A Venus scanout is an optimal render target in a private Metal heap;
     * nothing on the host can read it without a blit. */
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:displayExtent
        height:displayExtent
        mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, "private Metal texture creation failed");

    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder = [commands blitCommandEncoder];
    require(
        queue != nil && commands != nil && encoder != nil,
        "Metal upload command creation failed");
    [encoder
        copyFromBuffer:upload
        sourceOffset:0
        sourceBytesPerRow:rowBytes
        sourceBytesPerImage:byteCount
        sourceSize:MTLSizeMake(displayExtent, displayExtent, 1)
        toTexture:texture
        destinationSlice:0
        destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    require(
        commands.status == MTLCommandBufferStatusCompleted,
        "private Metal texture upload failed");
    return texture;
}

[[nodiscard]] uml::gpu::MetalScanout wholeScanout(id<MTLTexture> texture)
{
    return uml::gpu::MetalScanout {
        .texture = (__bridge void*)texture,
        .sourceWidth = displayExtent,
        .sourceHeight = displayExtent,
        .sourceX = 0,
        .sourceY = 0,
        .width = displayExtent,
        .height = displayExtent,
        .damageX = 0,
        .damageY = 0,
        .damageWidth = displayExtent,
        .damageHeight = displayExtent,
    };
}

void requireImage(
    const std::vector<std::uint8_t>& image,
    const Pixel& pixel,
    const char* message)
{
    for (std::uint32_t y = 0; y < displayExtent; ++y) {
        for (std::uint32_t x = 0; x < displayExtent; ++x) {
            /* LibVNCServer composites its default cursor into the pixels it
             * sends a client that asked for no cursor encoding, so the
             * top-left corner is the server's, not the scanout's. */
            if (x < cursorExtent && y < cursorExtent)
                continue;
            const auto offset =
                (static_cast<std::size_t>(y) * displayExtent + x) * 4U;
            if (image[offset] == pixel[0] &&
                image[offset + 1U] == pixel[1] &&
                image[offset + 2U] == pixel[2]) {
                continue;
            }
            std::fprintf(
                stderr,
                "VNC pixel at %u,%u is %02x%02x%02x, expected %02x%02x%02x\n",
                x, y, image[offset], image[offset + 1U],
                image[offset + 2U], pixel[0], pixel[1], pixel[2]);
            require(false, message);
        }
    }
}

template<typename Predicate>
void waitFor(Predicate predicate, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + operationTimeout;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void shippedVncDisplayReadsBackAVenusScanout()
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "Metal device unavailable");

        auto display = uml::gpu::makeDisplay(
            uml::gpu::DisplayMode::vnc, "127.0.0.1", 0, {}, displayExtent,
            displayExtent);
        require(display != nullptr, "VNC display creation failed");
        const auto port = display->vncPort();
        require(port.has_value() && *port != 0, "VNC display has no port");

        Connection connection(*port);
        connection.handshake();
        /* The display drops a scanout while no client is watching, so wait
         * until this one is counted before presenting. */
        const auto attached =
            std::chrono::steady_clock::now() + operationTimeout;
        while (!display->needsFullCpuFallback() &&
               std::chrono::steady_clock::now() < attached) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(display->needsFullCpuFallback(), "no VNC client was counted");

        id<MTLTexture> texture =
            makePrivateScanoutTexture(device, expectedPixel);
        const auto outcome = display->presentMetal(wholeScanout(texture));
        require(
            outcome == uml::gpu::PresentOutcome::presented,
            "the shipped VNC display refused a native Metal scanout");
        require(
            display->statistics().guestFrames == 1,
            "the shipped VNC display did not count the guest frame");
        waitFor(
            [&] { return display->statistics().presentedFrames != 0; },
            "the native Metal readback never completed");
        require(
            display->statistics().presentedFrames == 1,
            "the native Metal readback completed more than once");

        requireImage(
            connection.readFramebuffer(),
            expectedPixel,
            "the VNC framebuffer did not carry the scanout's pixels");
    }
}

/* With no client the display drops each scanout without reading it back, so
 * its framebuffer keeps the last frame an earlier client saw. A client that
 * connects later must still receive the scanout current at its connection,
 * even when the guest does not flush again. */
void clientAfterAnIdleIntervalSeesTheNewestScanout()
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "Metal device unavailable");

        auto display = uml::gpu::makeDisplay(
            uml::gpu::DisplayMode::vnc, "127.0.0.1", 0, {}, displayExtent,
            displayExtent);
        require(display != nullptr, "VNC display creation failed");
        const auto port = display->vncPort();
        require(port.has_value() && *port != 0, "VNC display has no port");

        id<MTLTexture> seen = makePrivateScanoutTexture(device, expectedPixel);
        id<MTLTexture> idle = makePrivateScanoutTexture(device, idlePixel);
        {
            Connection watching(*port);
            watching.handshake();
            waitFor(
                [&] { return display->needsFullCpuFallback(); },
                "no VNC client was counted");
            require(
                display->presentMetal(wholeScanout(seen)) ==
                    uml::gpu::PresentOutcome::presented,
                "the shipped VNC display refused a native Metal scanout");
            waitFor(
                [&] { return display->statistics().presentedFrames != 0; },
                "the native Metal readback never completed");
            requireImage(
                watching.readFramebuffer(),
                expectedPixel,
                "the watching client did not see the scanout");
        }

        /* Presenting the frame the framebuffer already holds is harmless
         * while the departed client is still counted. Only a display with no
         * client drops a frame without offering it to the readback, which
         * counts every frame it is offered. */
        waitFor(
            [&] {
                const auto offered = display->statistics().guestFrames;
                return display->presentMetal(wholeScanout(seen)) ==
                        uml::gpu::PresentOutcome::dropped &&
                    display->statistics().guestFrames == offered;
            },
            "the display kept counting a departed client");
        require(
            display->presentMetal(wholeScanout(idle)) ==
                uml::gpu::PresentOutcome::dropped,
            "the display read back a scanout with no client");

        Connection late(*port);
        late.handshake();
        requireImage(
            late.readFramebuffer(),
            idlePixel,
            "a client connecting after an idle interval saw an older frame");
    }
}

} // namespace

int main()
{
    try {
        shippedVncDisplayReadsBackAVenusScanout();
        clientAfterAnIdleIntervalSeesTheNewestScanout();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "VNC product display test: %s\n", error.what());
        return 1;
    }
    std::puts("VNC product display tests passed");
    return 0;
}
