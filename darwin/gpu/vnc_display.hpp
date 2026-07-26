// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"
#include "vnc_input.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

struct _rfbScreenInfo;
using rfbScreenInfoPtr = _rfbScreenInfo*;

namespace uml::gpu {

class VncDisplay {
public:
    VncDisplay(
        const std::string& bindAddress,
        std::uint16_t port,
        std::uint32_t width,
        std::uint32_t height,
        VncInputSink inputSink = {});
    ~VncDisplay();

    VncDisplay(const VncDisplay&) = delete;
    VncDisplay& operator=(const VncDisplay&) = delete;

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
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
    void handleKey(bool down, std::uint32_t keySymbol);
    void handlePointer(int buttonMask, int x, int y);

    std::uint32_t width_;
    std::uint32_t height_;
    VncInputSink inputSink_;
    std::vector<std::uint8_t> framebuffer_;
    rfbScreenInfoPtr screen_ {};
    std::mutex framebufferMutex_;
    std::atomic_bool running_ {true};
    std::thread eventThread_;
};

} // namespace uml::gpu
