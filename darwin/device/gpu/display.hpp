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

struct MetalScanout {
    void* texture {};
    std::uint32_t sourceWidth {};
    std::uint32_t sourceHeight {};
    std::uint32_t sourceX {};
    std::uint32_t sourceY {};
    std::uint32_t width {};
    std::uint32_t height {};
};

/* A direct presentation either cannot be expressed by this display at all, or
 * it was accepted. Acceptance is deliberately two outcomes: the guest must not
 * distinguish them, because a dropped frame is a normal result of rendering
 * faster than the host presents, and treating it as a failure would send the
 * scanout flush down the CPU readback path that this decoupling exists to
 * avoid. */
enum class PresentOutcome {
    unsupported,
    presented,
    dropped,
};

[[nodiscard]] constexpr bool presentAccepted(PresentOutcome outcome) noexcept
{
    return outcome != PresentOutcome::unsupported;
}

/* Guest and host rendering are independent timelines once a display accepts
 * direct presentation, so their frame counts are reported separately rather
 * than as one rate. */
struct DisplayStatistics {
    /* Scanout flushes the guest offered to this display. */
    std::uint64_t guestFrames {};
    /* Frames the host actually finished presenting. */
    std::uint64_t presentedFrames {};
    /* Guest frames retired without ever being presented, because a newer one
     * replaced them or no free surface was available. */
    std::uint64_t droppedFrames {};
    /* Guest frames that could not use direct presentation and took the CPU
     * readback path instead. */
    std::uint64_t cpuFallbackFrames {};
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
    /* ANGLE's Metal backend exposes a virgl scanout as the native MTLTexture
     * it already renders into. The opaque pointer keeps AppKit, UIKit, and
     * Objective-C types out of the platform-neutral device contract. */
    [[nodiscard]] virtual PresentOutcome presentMetal(
        const MetalScanout& scanout) noexcept
    {
        (void)scanout;
        return PresentOutcome::unsupported;
    }
    /* A native display can switch from direct GPU presentation to CPU
     * fallback after prior frames never populated its CPU framebuffer. */
    [[nodiscard]] virtual bool needsFullCpuFallback() const noexcept
    {
        return false;
    }
    /* Counters for the guest and host timelines. A display that never presents
     * directly still counts the guest frames it was offered. */
    [[nodiscard]] virtual DisplayStatistics statistics() const noexcept
    {
        return {};
    }
    /* The device records its own CPU readback frames here, because only the
     * device knows a direct presentation was refused. */
    virtual void recordCpuFallbackFrame() noexcept {}

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
    void* nativeView = nullptr,
    uml::input::vnc::PointerMode pointerMode =
        uml::input::vnc::PointerMode::mouse);

} // namespace uml::gpu
