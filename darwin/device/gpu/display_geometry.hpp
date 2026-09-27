// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
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

enum class DisplayRotation : std::uint8_t {
    upright,
    clockwise90,
    upsideDown,
    counterclockwise90,
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

[[nodiscard]] DisplayRotation rotateDisplay(
    DisplayRotation rotation,
    int quarterTurns) noexcept;
[[nodiscard]] DisplaySize presentedDisplaySize(
    DisplaySize guestSize,
    DisplayRotation rotation) noexcept;
/* Maps a normalized, top-left-origin point from the rotated presentation back
 * into the guest's unchanged scanout coordinate system. */
[[nodiscard]] DisplayPoint guestPointFromPresentedPoint(
    DisplayPoint point,
    DisplayRotation rotation) noexcept;

/* A guest scanout as a presenter samples it: the rectangle of the guest's
 * source image that the scanout shows, and the texture that stores the
 * source. A flipped texture stores the source bottom row first. */
struct ScanoutRegion final {
    DisplaySize texture;
    DisplaySize source;
    std::uint32_t x {};
    std::uint32_t y {};
    std::uint32_t width {};
    std::uint32_t height {};
    bool flipped {};
};

struct TextureCoordinate final {
    double x {};
    double y {};
};

/* Normalized texture coordinates that stretch a scanout rectangle over a
 * presenter's top-left-origin unit square: origin + point * extent, clamped
 * to [minimum, maximum], samples the rectangle, with a flip folded into a
 * negative vertical extent. The clamp keeps a filtered sample on the centres
 * of the rectangle's edge texels, so no pixel outside it bleeds in. content is
 * the rectangle's size, the one a presenter aspect-fits. */
struct ScanoutSampling final {
    DisplaySize content;
    TextureCoordinate origin;
    TextureCoordinate extent;
    TextureCoordinate minimum;
    TextureCoordinate maximum;
};

/* Returns nothing when the region cannot be sampled: an empty rectangle or
 * source, a rectangle reaching outside the source, or a texture smaller than
 * the source. */
[[nodiscard]] std::optional<ScanoutSampling> scanoutSampling(
    const ScanoutRegion& region) noexcept;

/* Returns the portion of a pointer path inside a display rectangle. Entry and
 * exit lie exactly on the rectangle boundary when the corresponding endpoint
 * is outside. This lets native touchscreen adapters synthesize contact at the
 * same boundary a physical finger would cross. */
[[nodiscard]] std::optional<ClippedDisplaySegment> clipDisplaySegment(
    AspectFitRectangle rectangle,
    DisplayPoint start,
    DisplayPoint end) noexcept;

/* Separates the host geometry advertised through Virtio-GPU from the guest's
 * active scanout. Host geometry changes only when automatic resize is
 * configured, while every guest scanout independently updates presentation.
 * All mutation is owned by the GPU transport thread; packed atomic snapshots
 * are also safe for AppKit/UIKit input and layout callbacks. */
class DisplayGeometryPolicy final {
public:
    DisplayGeometryPolicy(
        std::uint32_t width,
        std::uint32_t height,
        bool autoResizeGuest) noexcept;

    [[nodiscard]] DisplaySize advertisedSize() const noexcept;
    [[nodiscard]] DisplaySize activeGuestSize() const noexcept;
    [[nodiscard]] DisplaySize settledHostSize() const noexcept;
    [[nodiscard]] bool autoResizeGuestEnabled() const noexcept;

    /* Records every settled host geometry. Returns true only when it should be
     * advertised to the guest as a new preferred mode. */
    [[nodiscard]] bool hostSizeSettled(
        std::uint32_t width,
        std::uint32_t height) noexcept;
    [[nodiscard]] bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept;
    /* Keep published input geometry aligned with presentation storage across
     * a guest reset. Normally activeSize is the settled host size; callers may
     * retain the previous presentation size if allocating it failed. */
    void resetGuestSession(DisplaySize activeSize) noexcept;

private:
    [[nodiscard]] static std::uint64_t encode(DisplaySize size) noexcept;
    [[nodiscard]] static DisplaySize decode(std::uint64_t size) noexcept;

    const bool autoResizeGuest_;
    std::atomic_uint64_t advertisedSize_;
    std::atomic_uint64_t activeGuestSize_;
    std::atomic_uint64_t settledHostSize_;
};

} // namespace uml::gpu
