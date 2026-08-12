// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"
#include "../input/vnc/vnc_input.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uml::gpu {

enum class DisplayMode {
    vnc,
    native,
};

struct OpenGlScanout {
    std::uint32_t textureId {};
    std::uint32_t resourceTarget {};
    std::uint32_t sourceWidth {};
    std::uint32_t sourceHeight {};
    std::uint32_t sourceX {};
    std::uint32_t sourceY {};
    std::uint32_t width {};
    std::uint32_t height {};
};

struct MetalScanout {
    void* texture {};
    std::uint32_t sourceWidth {};
    std::uint32_t sourceHeight {};
    std::uint32_t sourceX {};
    std::uint32_t sourceY {};
    std::uint32_t width {};
    std::uint32_t height {};
};

class Display {
public:
    virtual ~Display() = default;

    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::uint16_t> vncPort() const noexcept
    {
        return std::nullopt;
    }
    virtual void clear() noexcept = 0;
    virtual void update(
        const std::vector<std::uint8_t>& source,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        protocol::GpuFormat sourceFormat,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height) = 0;
    virtual void update(
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
        std::uint32_t height) = 0;
    virtual void fillSelfTestPattern() = 0;
    /* Native displays may keep a Virgl scanout entirely on the GPU. VNC and
     * unsupported OpenGL resource shapes return false so the device uses its
     * existing CPU readback path. */
    [[nodiscard]] virtual bool presentOpenGl(
        const OpenGlScanout& scanout) noexcept
    {
        (void)scanout;
        return false;
    }
    /* ANGLE's Metal backend can expose a virgl scanout as the native
     * MTLTexture it already renders into. The opaque pointer keeps UIKit and
     * Objective-C types out of the platform-neutral device contract. */
    [[nodiscard]] virtual bool presentMetal(
        const MetalScanout& scanout) noexcept
    {
        (void)scanout;
        return false;
    }
    /* A native display can switch from direct GPU presentation to CPU
     * fallback after prior frames never populated its CPU framebuffer. */
    [[nodiscard]] virtual bool needsFullCpuFallback() const noexcept
    {
        return false;
    }

protected:
    Display() = default;
};

[[nodiscard]] std::unique_ptr<Display> makeDisplay(
    DisplayMode mode,
    const std::string& vncBindAddress,
    std::uint16_t vncPort,
    std::string vncPassword,
    std::uint32_t width,
    std::uint32_t height,
    uml::input::vnc::VncInputSink inputSink = {},
    void* nativeView = nullptr);

} // namespace uml::gpu
