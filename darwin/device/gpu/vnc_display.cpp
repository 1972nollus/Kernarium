// SPDX-License-Identifier: GPL-2.0-or-later

#include "vnc_display.hpp"

#if UML_GPU_VNC_STARTUP_TESTING
#include "vnc_display_testing.hpp"
#endif

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cstdio>
#include <limits>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <utility>

extern "C" {
#include <rfb/rfb.h>
}

namespace uml::gpu {
namespace {

namespace vnc_input = uml::input::vnc;

constexpr std::uint64_t clientCountMask =
    std::numeric_limits<std::uint32_t>::max();
constexpr unsigned connectionEpochShift = 32;

[[nodiscard]] constexpr std::uint32_t stateClientCount(
    const std::uint64_t state) noexcept
{
    return static_cast<std::uint32_t>(state & clientCountMask);
}

[[nodiscard]] constexpr std::uint32_t stateConnectionEpoch(
    const std::uint64_t state) noexcept
{
    return static_cast<std::uint32_t>(state >> connectionEpochShift);
}

[[nodiscard]] constexpr std::uint64_t makeClientState(
    const std::uint32_t epoch,
    const std::uint32_t count) noexcept
{
    return static_cast<std::uint64_t>(epoch) << connectionEpochShift | count;
}

[[nodiscard]] constexpr std::uint64_t makeExtent(
    const std::uint32_t width,
    const std::uint32_t height) noexcept
{
    return static_cast<std::uint64_t>(width) << 32U | height;
}

[[nodiscard]] constexpr std::uint32_t extentWidth(
    const std::uint64_t extent) noexcept
{
    return static_cast<std::uint32_t>(extent >> 32U);
}

[[nodiscard]] constexpr std::uint32_t extentHeight(
    const std::uint64_t extent) noexcept
{
    return static_cast<std::uint32_t>(extent);
}

class ScreenInitializationGuard final {
public:
    explicit ScreenInitializationGuard(rfbScreenInfoPtr& screen) noexcept
        : screen_(screen)
    {
    }

    ~ScreenInitializationGuard()
    {
        if (!armed_ || screen_ == nullptr)
            return;
        if (initialized_)
            rfbShutdownServer(screen_, TRUE);
        rfbScreenCleanup(screen_);
        screen_ = nullptr;
    }

    void initialized() noexcept { initialized_ = true; }
    void release() noexcept { armed_ = false; }

private:
    rfbScreenInfoPtr& screen_;
    bool initialized_ {};
    bool armed_ {true};
};

} // namespace

VncDisplay::VncDisplay(
    const std::string& bindAddress,
    std::uint16_t port,
    std::string password,
    std::uint32_t width,
    std::uint32_t height,
    vnc_input::VncInputSink inputSink,
    vnc_input::PointerMode pointerMode)
    : inputSink_(std::move(inputSink))
    , framebuffer_(width, height)
    , password_(std::move(password))
    , pointerMode_(pointerMode)
{
    cpuFallbackExtent_.store(
        makeExtent(framebuffer_.width(), framebuffer_.height()),
        std::memory_order_relaxed);
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
    ScreenInitializationGuard screenGuard{screen_};
    screen_->desktopName = "Darwin UML Virtio-GPU";
    screen_->frameBuffer = reinterpret_cast<char*>(framebuffer_.data());
    screen_->screenData = this;
    /* The GPU transport publishes pixels on a different thread from
     * LibVNCServer's event loop. LibVNCServer deliberately leaves ownership
     * of frameBuffer synchronization to its caller, so bracket every encoder
     * read with the same mutex used by clear(), update(), and the self-test
     * producer. The event loop is single-threaded in this integration. */
    screen_->displayHook = [](rfbClientPtr client) noexcept {
        static_cast<VncDisplay*>(client->screen->screenData)
            ->framebuffer_.lock();
    };
    screen_->displayFinishedHook = [](rfbClientPtr client, int) noexcept {
        static_cast<VncDisplay*>(client->screen->screenData)
            ->framebuffer_.unlock();
    };
    screen_->newClientHook = [](rfbClientPtr client) noexcept {
        auto* display =
            static_cast<VncDisplay*>(client->screen->screenData);
        if (!display->clientConnected())
            return RFB_CLIENT_REFUSE;
        client->clientGoneHook = [](rfbClientPtr disconnected) noexcept {
            static_cast<VncDisplay*>(disconnected->screen->screenData)
                ->clientDisconnected();
        };
        return RFB_CLIENT_ACCEPT;
    };
    screen_->kbdAddEvent = [](
                               rfbBool down,
                               rfbKeySym keySymbol,
                               rfbClientPtr client) noexcept {
        static_cast<VncDisplay*>(client->screen->screenData)
            ->handleKey(down != FALSE, keySymbol);
    };
    screen_->ptrAddEvent = [](
                               int buttonMask,
                               int x,
                               int y,
                               rfbClientPtr client) noexcept {
        rfbDefaultPtrAddEvent(buttonMask, x, y, client);
        static_cast<VncDisplay*>(client->screen->screenData)
            ->handlePointer(buttonMask, x, y);
    };
    screen_->alwaysShared = TRUE;
    const bool chooseVncPort = port == 0;
    // LibVNCServer's autoPort mode scans from the current positive port; it
    // does not treat port zero as an OS-assigned ephemeral port.
    screen_->port = chooseVncPort ? 5900 : static_cast<int>(port);
    screen_->autoPort = chooseVncPort ? TRUE : FALSE;
    screen_->ipv6port = -1;
    in_addr interfaceAddress {};
    if (::inet_pton(AF_INET, bindAddress.c_str(), &interfaceAddress) != 1) {
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
    if (!password_.empty()) {
        if (password_.size() > 8) {
            throw std::invalid_argument(
                "VNC password must contain at most 8 bytes");
        }
        passwordList_[0] = password_.data();
        screen_->authPasswdData = passwordList_.data();
        screen_->passwordCheck = rfbCheckPasswordByList;
    }
    rfbInitServer(screen_);
    screenGuard.initialized();
    if (!rfbIsActive(screen_) || screen_->port <= 0 ||
        screen_->listenSock == RFB_INVALID_SOCKET) {
        throw std::runtime_error("VNC server failed to listen");
    }
#if UML_GPU_VNC_STARTUP_TESTING
    testing::eventThreadStartCheckpoint(
        static_cast<std::uint16_t>(screen_->port));
#endif
    eventThread_ = std::thread(&VncDisplay::eventLoop, this);
    screenGuard.release();
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
    return framebuffer_.width();
}

std::uint32_t VncDisplay::height() const noexcept
{
    return framebuffer_.height();
}

std::uint16_t VncDisplay::port() const noexcept
{
    return screen_ == nullptr ? 0 : static_cast<std::uint16_t>(screen_->port);
}

std::optional<std::uint16_t> VncDisplay::vncPort() const noexcept
{
    return port();
}

void VncDisplay::clear() noexcept
{
    std::lock_guard lock(framebuffer_);
    framebuffer_.clearLocked();
    // Scaled client framebuffers are derived synchronously by this call, so
    // keep them in the same publication critical section as the source image.
    rfbMarkRectAsModified(
        screen_, 0, 0, static_cast<int>(width()), static_cast<int>(height()));
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
    const auto updateEpoch = connectionEpoch();
    const auto expectedExtent =
        cpuFallbackExtent_.load(std::memory_order_acquire);
    const auto rectangle = framebuffer_.update(
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
    if (rectangle.width == 0 || rectangle.height == 0)
        return;
    std::lock_guard lock(framebuffer_);
    rfbMarkRectAsModified(
        screen_,
        static_cast<int>(rectangle.x),
        static_cast<int>(rectangle.y),
        static_cast<int>(rectangle.x + rectangle.width),
        static_cast<int>(rectangle.y + rectangle.height));
    if (rectangle.x == 0 && rectangle.y == 0 &&
        rectangle.width == extentWidth(expectedExtent) &&
        rectangle.height == extentHeight(expectedExtent)) {
        recordFullFrame(updateEpoch);
    }
}

void VncDisplay::fillSelfTestPattern()
{
    std::lock_guard lock(framebuffer_);
    framebuffer_.fillSelfTestPatternLocked();
    rfbMarkRectAsModified(
        screen_, 0, 0, static_cast<int>(width()), static_cast<int>(height()));
}

PresentOutcome VncDisplay::presentMetal(
    const MetalScanout& scanout) noexcept
{
    /* With no remote consumer, accepting and dropping the scanout avoids the
     * synchronous Metal-to-CPU readback. Once a client exists, unsupported
     * deliberately selects the ordinary CPU framebuffer path. */
    if (stateClientCount(
            clientState_.load(std::memory_order_acquire)) == 0) {
        return PresentOutcome::dropped;
    }
    cpuFallbackExtent_.store(
        makeExtent(
            std::min(scanout.width, framebuffer_.width()),
            std::min(scanout.height, framebuffer_.height())),
        std::memory_order_release);
    return PresentOutcome::unsupported;
}

bool VncDisplay::needsFullCpuFallback() const noexcept
{
    const auto state = clientState_.load(std::memory_order_acquire);
    return stateClientCount(state) != 0 &&
        fullFrameEpoch_.load(std::memory_order_acquire) !=
            stateConnectionEpoch(state);
}

bool VncDisplay::clientConnected() noexcept
{
    auto state = clientState_.load(std::memory_order_relaxed);
    while (true) {
        const auto count = stateClientCount(state);
        const auto epoch = stateConnectionEpoch(state);
        if (count == std::numeric_limits<std::uint32_t>::max())
            return false;
        const auto desired = makeClientState(
            count == 0 ? epoch + 1U : epoch,
            count + 1U);
        if (clientState_.compare_exchange_weak(
                state,
                desired,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            return true;
        }
    }
}

void VncDisplay::clientDisconnected() noexcept
{
    auto state = clientState_.load(std::memory_order_relaxed);
    while (stateClientCount(state) != 0) {
        const auto desired = makeClientState(
            stateConnectionEpoch(state), stateClientCount(state) - 1U);
        if (clientState_.compare_exchange_weak(
                state,
                desired,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            return;
        }
    }
}

std::uint32_t VncDisplay::connectionEpoch() const noexcept
{
    return stateConnectionEpoch(
        clientState_.load(std::memory_order_acquire));
}

void VncDisplay::recordFullFrame(const std::uint32_t epoch) noexcept
{
    auto completed = fullFrameEpoch_.load(std::memory_order_relaxed);
    while (completed < epoch &&
           !fullFrameEpoch_.compare_exchange_weak(
               completed,
               epoch,
               std::memory_order_release,
               std::memory_order_relaxed)) {
    }
}

void VncDisplay::handleKey(bool down, std::uint32_t keySymbol) noexcept
{
    if (!inputSink_)
        return;
    const auto code = vnc_input::keyCode(keySymbol);
    if (!code)
        return;
    const std::array events {
        vnc_input::VncInputEvent {
            .type = vnc_input::eventKey,
            .code = *code,
            .value = down ? 1 : 0,
        },
        vnc_input::VncInputEvent {
            .type = vnc_input::eventSync,
            .code = vnc_input::syncReport,
            .value = 0,
        },
    };
    if (!vnc_input::tryDeliver(inputSink_, events))
        reportInputFailure();
}

void VncDisplay::handlePointer(int buttonMask, int x, int y) noexcept
{
    if (!inputSink_)
        return;
    const bool pressed = (buttonMask & 1) != 0;
    if (pointerMode_ == vnc_input::PointerMode::touchscreen &&
        !touchContact_.update(pressed))
        return;
    const auto scale = [](int coordinate, std::uint32_t extent) {
        if (extent <= 1)
            return 0;
        const auto clamped = std::clamp(
            coordinate, 0, static_cast<int>(extent - 1U));
        return static_cast<std::int32_t>(
            static_cast<std::int64_t>(clamped) *
                vnc_input::absoluteMaximum /
            static_cast<std::int64_t>(extent - 1U));
    };

    std::array<vnc_input::VncInputEvent, 10> events {};
    std::size_t count = 0;
    const auto append = [&](std::uint16_t type,
                            std::uint16_t code,
                            std::int32_t value) {
        events[count++] = {.type = type, .code = code, .value = value};
    };
    append(vnc_input::eventAbsolute, vnc_input::absoluteX, scale(x, width()));
    append(vnc_input::eventAbsolute, vnc_input::absoluteY, scale(y, height()));
    if (pointerMode_ == vnc_input::PointerMode::touchscreen) {
        append(vnc_input::eventKey, vnc_input::buttonTouch, pressed ? 1 : 0);
    } else {
        append(vnc_input::eventKey, vnc_input::buttonLeft, buttonMask & 1);
        append(
            vnc_input::eventKey,
            vnc_input::buttonMiddle,
            (buttonMask >> 1) & 1);
        append(
            vnc_input::eventKey,
            vnc_input::buttonRight,
            (buttonMask >> 2) & 1);
        if ((buttonMask & 8) != 0)
            append(vnc_input::eventRelative, vnc_input::relativeWheel, 1);
        if ((buttonMask & 16) != 0)
            append(vnc_input::eventRelative, vnc_input::relativeWheel, -1);
        if ((buttonMask & 32) != 0)
            append(
                vnc_input::eventRelative,
                vnc_input::relativeHorizontalWheel,
                -1);
        if ((buttonMask & 64) != 0)
            append(
                vnc_input::eventRelative,
                vnc_input::relativeHorizontalWheel,
                1);
    }
    append(vnc_input::eventSync, vnc_input::syncReport, 0);
    if (!vnc_input::tryDeliver(inputSink_, std::span(events).first(count)))
        reportInputFailure();
}

void VncDisplay::reportInputFailure() noexcept
{
    if (!inputFailureReported_.exchange(true, std::memory_order_relaxed)) {
        std::fputs(
            "Unable to deliver VNC input to the emulator\n", stderr);
    }
}

void VncDisplay::eventLoop()
{
    (void)::pthread_setname_np("uml-vnc");
    while (running_.load(std::memory_order_acquire) && rfbIsActive(screen_))
        rfbProcessEvents(screen_, 10'000);
}

} // namespace uml::gpu
