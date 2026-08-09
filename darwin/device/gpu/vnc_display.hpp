// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"
#include "../input/vnc/vnc_input.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <os/lock.h>

#ifndef UML_GPU_VNC_STARTUP_TESTING
#define UML_GPU_VNC_STARTUP_TESTING 0
#endif

struct _rfbScreenInfo;
using rfbScreenInfoPtr = _rfbScreenInfo*;

namespace uml::gpu {

class FramebufferMutex final {
public:
    void lock() noexcept { os_unfair_lock_lock(&lock_); }
    void unlock() noexcept { os_unfair_lock_unlock(&lock_); }

private:
    os_unfair_lock lock_ = OS_UNFAIR_LOCK_INIT;
};

class VncDisplay {
public:
    VncDisplay(
        const std::string& bindAddress,
        std::uint16_t port,
        std::string password,
        std::uint32_t width,
        std::uint32_t height,
        uml::input::vnc::VncInputSink inputSink = {});
    ~VncDisplay();

    VncDisplay(const VncDisplay&) = delete;
    VncDisplay& operator=(const VncDisplay&) = delete;

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    // A requested port of zero asks LibVNCServer to bind an available port.
    // Callers use the post-bind value instead of racing a probe socket.
    [[nodiscard]] std::uint16_t port() const noexcept;
    // Return the published scanout to a blank device-reset state. This is
    // allocation-free so transport teardown can call it from a noexcept reset.
    void clear() noexcept;
    void update(
        const std::vector<std::uint8_t>& source,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        protocol::GpuFormat sourceFormat,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height);
    void update(
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
        std::uint32_t height);
    void fillSelfTestPattern();

private:
    void eventLoop();
    void handleKey(bool down, std::uint32_t keySymbol) noexcept;
    void handlePointer(int buttonMask, int x, int y) noexcept;
    void reportInputFailure() noexcept;

    std::uint32_t width_;
    std::uint32_t height_;
    uml::input::vnc::VncInputSink inputSink_;
    std::vector<std::uint8_t> framebuffer_;
    std::string password_;
    std::array<char*, 2> passwordList_ {};
    rfbScreenInfoPtr screen_ {};
    FramebufferMutex framebufferMutex_;
    std::atomic_bool running_ {true};
    std::atomic_bool inputFailureReported_ {};
    std::thread eventThread_;
};

} // namespace uml::gpu
