// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace uml::gpu {

struct DisplaySize final {
    std::uint32_t width {};
    std::uint32_t height {};

    [[nodiscard]] bool operator==(const DisplaySize&) const noexcept = default;
};

struct AspectFitRectangle final {
    double x {};
    double y {};
    double width {};
    double height {};
};

struct DisplayPoint final {
    double x {};
    double y {};
};

struct ClippedDisplaySegment final {
    DisplayPoint entry;
    DisplayPoint exit;
};

[[nodiscard]] AspectFitRectangle aspectFitRectangle(
    double containerWidth,
    double containerHeight,
    double contentWidth,
    double contentHeight) noexcept;

/* Returns the portion of a pointer path inside a display rectangle. Entry and
 * exit lie exactly on the rectangle boundary when the corresponding endpoint
 * is outside. This lets native touchscreen adapters synthesize contact at the
 * same boundary a physical finger would cross. */
[[nodiscard]] std::optional<ClippedDisplaySegment> clipDisplaySegment(
    AspectFitRectangle rectangle,
    DisplayPoint start,
    DisplayPoint end) noexcept;

struct GuestDisplayChange final {
    bool changed {};
    bool independent {};
};

/* Separates the host geometry advertised through Virtio-GPU from the guest's
 * active scanout. Host targets are retained in order so a delayed response to
 * an earlier settled resize is not mistaken for an independent guest mode
 * change. All mutation is owned by the GPU transport thread; packed atomic
 * snapshots are also safe for AppKit/UIKit input and layout callbacks. */
class DisplayGeometryPolicy final {
public:
    DisplayGeometryPolicy(
        std::uint32_t width,
        std::uint32_t height,
        bool autoResizeGuest) noexcept;

    [[nodiscard]] DisplaySize advertisedSize() const noexcept;
    [[nodiscard]] DisplaySize activeGuestSize() const noexcept;
    [[nodiscard]] DisplaySize settledHostSize() const noexcept;
    [[nodiscard]] bool configuredAutoResizeGuest() const noexcept;
    [[nodiscard]] bool autoResizeGuestEnabled() const noexcept;

    /* Records every settled host geometry. Returns true only when it should be
     * advertised to the guest as a new preferred mode. */
    [[nodiscard]] bool hostSizeSettled(
        std::uint32_t width,
        std::uint32_t height) noexcept;
    [[nodiscard]] GuestDisplayChange guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept;
    /* Restore the configured per-boot policy while keeping the published
     * input geometry aligned with presentation storage. Normally activeSize
     * is the settled host size; callers may retain the previous presentation
     * size if allocating the reset surface failed. */
    void resetGuestSession(DisplaySize activeSize) noexcept;

private:
    static constexpr std::size_t targetCapacity = 16;
    static constexpr std::uint32_t maximumPreferredModeRounding = 16;

    [[nodiscard]] static std::uint64_t encode(DisplaySize size) noexcept;
    [[nodiscard]] static DisplaySize decode(std::uint64_t size) noexcept;
    [[nodiscard]] static bool matchesHostTarget(
        std::uint64_t guest,
        std::uint64_t target) noexcept;
    void appendHostTarget(std::uint64_t target) noexcept;
    [[nodiscard]] bool consumeHostTarget(std::uint64_t guest) noexcept;

    const bool configuredAutoResizeGuest_;
    std::atomic_bool autoResizeGuestEnabled_;
    std::atomic_uint64_t advertisedSize_;
    std::atomic_uint64_t activeGuestSize_;
    std::atomic_uint64_t settledHostSize_;
    std::array<std::uint64_t, targetCapacity> hostTargets_ {};
    std::size_t hostTargetCount_ {};
    bool hasActiveGuestScanout_ {};
};

} // namespace uml::gpu
