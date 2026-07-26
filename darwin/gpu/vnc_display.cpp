// SPDX-License-Identifier: GPL-2.0-or-later

#include "vnc_display.hpp"

#include <Accelerate/Accelerate.h>
#include <arm_neon.h>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cstring>
#include <limits>
#include <pthread.h>
#include <stdexcept>
#include <utility>

extern "C" {
#include <rfb/rfb.h>
}

namespace uml::gpu {
namespace {

constexpr std::uint8_t opaqueAlpha = 0xffU;

std::array<std::uint8_t, 4> channelPermutation(protocol::GpuFormat format)
{
    switch (format) {
    case protocol::GpuFormat::b8g8r8a8Unorm:
    case protocol::GpuFormat::b8g8r8x8Unorm:
        return {0, 1, 2, 3};
    case protocol::GpuFormat::a8r8g8b8Unorm:
    case protocol::GpuFormat::x8r8g8b8Unorm:
        return {3, 2, 1, 0};
    case protocol::GpuFormat::r8g8b8a8Unorm:
    case protocol::GpuFormat::r8g8b8x8Unorm:
        return {2, 1, 0, 3};
    case protocol::GpuFormat::x8b8g8r8Unorm:
    case protocol::GpuFormat::a8b8g8r8Unorm:
        return {1, 2, 3, 0};
    }
    throw std::invalid_argument("unsupported Virtio-GPU pixel format");
}

void copyRectangleWithVImage(
    std::uint8_t* destination,
    const std::uint8_t* source,
    std::uint32_t width,
    std::uint32_t height,
    std::size_t sourceRowBytes,
    std::size_t destinationRowBytes,
    protocol::GpuFormat format)
{
    const vImage_Buffer sourceBuffer {
        .data = const_cast<std::uint8_t*>(source),
        .height = height,
        .width = width,
        .rowBytes = sourceRowBytes,
    };
    const vImage_Buffer destinationBuffer {
        .data = destination,
        .height = height,
        .width = width,
        .rowBytes = destinationRowBytes,
    };

    vImage_Error error = kvImageNoError;
    if (format == protocol::GpuFormat::b8g8r8a8Unorm ||
        format == protocol::GpuFormat::b8g8r8x8Unorm) {
        error = vImageCopyBuffer(
            &sourceBuffer,
            &destinationBuffer,
            protocol::gpuBytesPerPixel,
            kvImageNoFlags);
    } else {
        const auto permutation = channelPermutation(format);
        error = vImagePermuteChannels_ARGB8888(
            &sourceBuffer,
            &destinationBuffer,
            permutation.data(),
            kvImageNoFlags);
    }
    if (error != kvImageNoError)
        throw std::runtime_error("vImage pixel conversion failed");
}

} // namespace

VncDisplay::VncDisplay(
    const std::string& bindAddress,
    std::uint16_t port,
    std::uint32_t width,
    std::uint32_t height,
    VncInputSink inputSink)
    : width_(width)
    , height_(height)
    , inputSink_(std::move(inputSink))
{
    if (width == 0 || height == 0 ||
        width > std::numeric_limits<std::size_t>::max() / height / 4U) {
        throw std::invalid_argument("invalid VNC framebuffer dimensions");
    }
    framebuffer_.resize(static_cast<std::size_t>(width) * height * 4U);

    int argumentCount = 0;
    screen_ = rfbGetScreen(
        &argumentCount,
        nullptr,
        static_cast<int>(width),
        static_cast<int>(height),
        8,
        3,
        4);
    if (screen_ == nullptr) {
        throw std::runtime_error("rfbGetScreen failed");
    }
    screen_->desktopName = "Darwin UML Virtio-GPU";
    screen_->frameBuffer = reinterpret_cast<char*>(framebuffer_.data());
    screen_->screenData = this;
    screen_->kbdAddEvent = [](
                               rfbBool down,
                               rfbKeySym keySymbol,
                               rfbClientPtr client) {
        static_cast<VncDisplay*>(client->screen->screenData)
            ->handleKey(down != FALSE, keySymbol);
    };
    screen_->ptrAddEvent = [](
                               int buttonMask,
                               int x,
                               int y,
                               rfbClientPtr client) {
        rfbDefaultPtrAddEvent(buttonMask, x, y, client);
        static_cast<VncDisplay*>(client->screen->screenData)
            ->handlePointer(buttonMask, x, y);
    };
    screen_->alwaysShared = TRUE;
    screen_->port = static_cast<int>(port);
    screen_->ipv6port = -1;
    in_addr interfaceAddress {};
    if (::inet_pton(AF_INET, bindAddress.c_str(), &interfaceAddress) != 1) {
        rfbScreenCleanup(screen_);
        screen_ = nullptr;
        throw std::invalid_argument(
            "invalid VNC IPv4 bind address: " + bindAddress);
    }
    screen_->listenInterface = interfaceAddress.s_addr;
    screen_->serverFormat.bitsPerPixel = 32;
    screen_->serverFormat.depth = 24;
    screen_->serverFormat.bigEndian = FALSE;
    screen_->serverFormat.trueColour = TRUE;
    screen_->serverFormat.redMax = 255;
    screen_->serverFormat.greenMax = 255;
    screen_->serverFormat.blueMax = 255;
    screen_->serverFormat.redShift = 16;
    screen_->serverFormat.greenShift = 8;
    screen_->serverFormat.blueShift = 0;
    rfbInitServer(screen_);
    if (!rfbIsActive(screen_)) {
        rfbScreenCleanup(screen_);
        screen_ = nullptr;
        throw std::runtime_error("VNC server failed to listen");
    }
    eventThread_ = std::thread(&VncDisplay::eventLoop, this);
}

VncDisplay::~VncDisplay()
{
    running_.store(false, std::memory_order_release);
    if (eventThread_.joinable())
        eventThread_.join();
    if (screen_ != nullptr) {
        rfbShutdownServer(screen_, TRUE);
        rfbScreenCleanup(screen_);
    }
}

std::uint32_t VncDisplay::width() const noexcept
{
    return width_;
}

std::uint32_t VncDisplay::height() const noexcept
{
    return height_;
}

void VncDisplay::update(
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

void VncDisplay::update(
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
    const auto minimumSourceRowBytes =
        static_cast<std::uint64_t>(sourceWidth) *
        protocol::gpuBytesPerPixel;
    if (sourceRowBytes < minimumSourceRowBytes)
        throw std::invalid_argument("short Virtio-GPU source stride");
    if (sourceHeight != 0) {
        const auto requiredSourceBytes =
            static_cast<std::uint64_t>(sourceHeight - 1U) *
                sourceRowBytes +
            minimumSourceRowBytes;
        if (requiredSourceBytes > source.size())
            throw std::invalid_argument("short Virtio-GPU resource");
    }
    if (sourceX >= sourceWidth || sourceY >= sourceHeight ||
        destinationX >= width_ || destinationY >= height_) {
        return;
    }
    const auto rectangleWidth = std::min(
        {width, sourceWidth - sourceX, width_ - destinationX});
    const auto rectangleHeight = std::min(
        {height, sourceHeight - sourceY, height_ - destinationY});
    if (rectangleWidth == 0 || rectangleHeight == 0)
        return;
    {
        std::lock_guard lock(framebufferMutex_);
        const auto sourceOffset =
            static_cast<std::size_t>(sourceY) * sourceRowBytes +
            static_cast<std::size_t>(sourceX) *
                protocol::gpuBytesPerPixel;
        const auto destinationOffset =
            (static_cast<std::size_t>(destinationY) * width_ +
                destinationX) *
            protocol::gpuBytesPerPixel;
        copyRectangleWithVImage(
            framebuffer_.data() + destinationOffset,
            source.data() + sourceOffset,
            rectangleWidth,
            rectangleHeight,
            sourceRowBytes,
            static_cast<std::size_t>(width_) *
                protocol::gpuBytesPerPixel,
            sourceFormat);
    }
    rfbMarkRectAsModified(
        screen_,
        static_cast<int>(destinationX),
        static_cast<int>(destinationY),
        static_cast<int>(destinationX + rectangleWidth),
        static_cast<int>(destinationY + rectangleHeight));
}

void VncDisplay::fillSelfTestPattern()
{
    alignas(16) static constexpr std::array<std::uint8_t, 16> pixelLanes {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    };
    constexpr std::uint8_t testRed = 0x5aU;

    std::lock_guard lock(framebufferMutex_);
    for (std::uint32_t y = 0; y < height_; ++y) {
        std::uint32_t x = 0;
        const uint8x16_t lane = vld1q_u8(pixelLanes.data());
        for (; width_ - x >= 16U; x += 16U) {
            const auto offset =
                (static_cast<std::size_t>(y) * width_ + x) * 4U;
            uint8x16x4_t pixels {};
            pixels.val[0] =
                vaddq_u8(lane, vdupq_n_u8(static_cast<std::uint8_t>(x)));
            pixels.val[1] =
                vdupq_n_u8(static_cast<std::uint8_t>(y));
            pixels.val[2] = vdupq_n_u8(testRed);
            pixels.val[3] = vdupq_n_u8(opaqueAlpha);
            vst4q_u8(framebuffer_.data() + offset, pixels);
        }
        for (; x < width_; ++x) {
            const auto offset =
                (static_cast<std::size_t>(y) * width_ + x) * 4U;
            framebuffer_[offset] = static_cast<std::uint8_t>(x & 0xffU);
            framebuffer_[offset + 1U] = static_cast<std::uint8_t>(y & 0xffU);
            framebuffer_[offset + 2U] = testRed;
            framebuffer_[offset + 3U] = opaqueAlpha;
        }
    }
    rfbMarkRectAsModified(
        screen_, 0, 0, static_cast<int>(width_), static_cast<int>(height_));
}

void VncDisplay::handleKey(bool down, std::uint32_t keySymbol)
{
    if (!inputSink_)
        return;
    const auto code = input::keyCode(keySymbol);
    if (!code)
        return;
    const std::array events {
        VncInputEvent {
            .type = input::eventKey,
            .code = *code,
            .value = down ? 1 : 0,
        },
        VncInputEvent {
            .type = input::eventSync,
            .code = input::syncReport,
            .value = 0,
        },
    };
    inputSink_(events);
}

void VncDisplay::handlePointer(int buttonMask, int x, int y)
{
    if (!inputSink_)
        return;
    const auto scale = [](int coordinate, std::uint32_t extent) {
        if (extent <= 1)
            return 0;
        const auto clamped = std::clamp(
            coordinate, 0, static_cast<int>(extent - 1U));
        return static_cast<std::int32_t>(
            static_cast<std::int64_t>(clamped) * input::absoluteMaximum /
            static_cast<std::int64_t>(extent - 1U));
    };

    std::array<VncInputEvent, 10> events {};
    std::size_t count = 0;
    const auto append = [&](std::uint16_t type,
                            std::uint16_t code,
                            std::int32_t value) {
        events[count++] = {.type = type, .code = code, .value = value};
    };
    append(input::eventAbsolute, input::absoluteX, scale(x, width_));
    append(input::eventAbsolute, input::absoluteY, scale(y, height_));
    append(input::eventKey, input::buttonLeft, buttonMask & 1);
    append(input::eventKey, input::buttonMiddle, (buttonMask >> 1) & 1);
    append(input::eventKey, input::buttonRight, (buttonMask >> 2) & 1);
    if ((buttonMask & 8) != 0)
        append(input::eventRelative, input::relativeWheel, 1);
    if ((buttonMask & 16) != 0)
        append(input::eventRelative, input::relativeWheel, -1);
    if ((buttonMask & 32) != 0)
        append(input::eventRelative, input::relativeHorizontalWheel, -1);
    if ((buttonMask & 64) != 0)
        append(input::eventRelative, input::relativeHorizontalWheel, 1);
    append(input::eventSync, input::syncReport, 0);
    inputSink_(std::span(events).first(count));
}

void VncDisplay::eventLoop()
{
    (void)::pthread_setname_np("uml-vnc");
    while (running_.load(std::memory_order_acquire) && rfbIsActive(screen_))
        rfbProcessEvents(screen_, 10'000);
}

} // namespace uml::gpu
