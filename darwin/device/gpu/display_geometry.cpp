// SPDX-License-Identifier: GPL-2.0-or-later

#include "display_geometry.hpp"

#include <algorithm>
#include <cmath>

namespace uml::gpu {

AspectFitRectangle aspectFitRectangle(
    double containerWidth,
    double containerHeight,
    double contentWidth,
    double contentHeight) noexcept
{
    if (!std::isfinite(containerWidth) ||
        !std::isfinite(containerHeight) ||
        !std::isfinite(contentWidth) ||
        !std::isfinite(contentHeight) ||
        containerWidth <= 0.0 || containerHeight <= 0.0 ||
        contentWidth <= 0.0 || contentHeight <= 0.0) {
        return {};
    }
    const double scale = std::min(
        containerWidth / contentWidth,
        containerHeight / contentHeight);
    const double width = contentWidth * scale;
    const double height = contentHeight * scale;
    return {
        .x = (containerWidth - width) / 2.0,
        .y = (containerHeight - height) / 2.0,
        .width = width,
        .height = height,
    };
}

DisplayRotation rotateDisplay(
    DisplayRotation rotation,
    int quarterTurns) noexcept
{
    constexpr int rotationCount = 4;
    int value = static_cast<int>(rotation) + quarterTurns % rotationCount;
    value %= rotationCount;
    if (value < 0)
        value += rotationCount;
    return static_cast<DisplayRotation>(value);
}

DisplaySize presentedDisplaySize(
    DisplaySize guestSize,
    DisplayRotation rotation) noexcept
{
    if ((static_cast<std::uint8_t>(rotation) & 1U) == 0)
        return guestSize;
    return {.width = guestSize.height, .height = guestSize.width};
}

DisplayPoint guestPointFromPresentedPoint(
    DisplayPoint point,
    DisplayRotation rotation) noexcept
{
    switch (rotation) {
    case DisplayRotation::upright:
        return point;
    case DisplayRotation::clockwise90:
        return {.x = point.y, .y = 1.0 - point.x};
    case DisplayRotation::upsideDown:
        return {.x = 1.0 - point.x, .y = 1.0 - point.y};
    case DisplayRotation::counterclockwise90:
        return {.x = 1.0 - point.y, .y = point.x};
    }
    return point;
}

std::optional<ScanoutSampling> scanoutSampling(
    const ScanoutRegion& region) noexcept
{
    const auto fits = [](std::uint32_t offset,
                          std::uint32_t length,
                          std::uint32_t limit) {
        return length != 0 &&
            static_cast<std::uint64_t>(offset) + length <= limit;
    };
    if (region.source.width == 0 || region.source.height == 0 ||
        region.texture.width < region.source.width ||
        region.texture.height < region.source.height ||
        !fits(region.x, region.width, region.source.width) ||
        !fits(region.y, region.height, region.source.height)) {
        return std::nullopt;
    }
    const double textureWidth = region.texture.width;
    const double textureHeight = region.texture.height;
    const double left = region.x;
    const double right = left + region.width;
    /* Texture rows holding the rectangle's top and bottom edges. A flipped
     * source keeps its bottom row in texture row 0, so its top edge is the
     * higher row. */
    const double top = region.flipped ?
        static_cast<double>(region.source.height) - region.y :
        static_cast<double>(region.y);
    const double bottom = region.flipped ?
        top - region.height : top + region.height;
    const double lowRow = std::min(top, bottom);
    const double highRow = std::max(top, bottom);
    return ScanoutSampling {
        .content = {.width = region.width, .height = region.height},
        .origin = {.x = left / textureWidth, .y = top / textureHeight},
        .extent = {
            .x = (right - left) / textureWidth,
            .y = (bottom - top) / textureHeight,
        },
        .minimum = {
            .x = (left + 0.5) / textureWidth,
            .y = (lowRow + 0.5) / textureHeight,
        },
        .maximum = {
            .x = (right - 0.5) / textureWidth,
            .y = (highRow - 0.5) / textureHeight,
        },
    };
}

std::optional<ClippedDisplaySegment> clipDisplaySegment(
    AspectFitRectangle rectangle,
    DisplayPoint start,
    DisplayPoint end) noexcept
{
    const double right = rectangle.x + rectangle.width;
    const double top = rectangle.y + rectangle.height;
    if (!std::isfinite(rectangle.x) || !std::isfinite(rectangle.y) ||
        !std::isfinite(rectangle.width) ||
        !std::isfinite(rectangle.height) || !std::isfinite(right) ||
        !std::isfinite(top) || rectangle.width <= 0.0 ||
        rectangle.height <= 0.0 || !std::isfinite(start.x) ||
        !std::isfinite(start.y) || !std::isfinite(end.x) ||
        !std::isfinite(end.y)) {
        return std::nullopt;
    }

    const double deltaX = end.x - start.x;
    const double deltaY = end.y - start.y;
    double entry = 0.0;
    double exit = 1.0;
    const auto clip = [&](double direction, double distance) {
        if (direction == 0.0)
            return distance >= 0.0;
        const double fraction = distance / direction;
        if (direction < 0.0) {
            if (fraction > exit)
                return false;
            entry = std::max(entry, fraction);
        } else {
            if (fraction < entry)
                return false;
            exit = std::min(exit, fraction);
        }
        return true;
    };
    if (!clip(-deltaX, start.x - rectangle.x) ||
        !clip(deltaX, right - start.x) ||
        !clip(-deltaY, start.y - rectangle.y) ||
        !clip(deltaY, top - start.y) || entry > exit) {
        return std::nullopt;
    }

    const auto inside = [&](DisplayPoint point) {
        return point.x >= rectangle.x && point.x <= right &&
            point.y >= rectangle.y && point.y <= top;
    };
    if (entry == exit && !inside(start) && !inside(end)) {
        /* Merely touching one corner has no path inside the display and must
         * not become a zero-duration guest contact. */
        return std::nullopt;
    }

    const auto pointAt = [&](double fraction) {
        return DisplayPoint {
            .x = std::clamp(
                start.x + fraction * deltaX, rectangle.x, right),
            .y = std::clamp(
                start.y + fraction * deltaY, rectangle.y, top),
        };
    };
    return ClippedDisplaySegment {
        .entry = pointAt(entry),
        .exit = pointAt(exit),
    };
}

DisplayGeometryPolicy::DisplayGeometryPolicy(
    std::uint32_t width,
    std::uint32_t height,
    bool autoResizeGuest) noexcept
    : autoResizeGuest_(autoResizeGuest)
    , advertisedSize_(encode({width, height}))
    , activeGuestSize_(encode({width, height}))
    , settledHostSize_(encode({width, height}))
{
}

DisplaySize DisplayGeometryPolicy::advertisedSize() const noexcept
{
    return decode(advertisedSize_.load(std::memory_order_acquire));
}

DisplaySize DisplayGeometryPolicy::activeGuestSize() const noexcept
{
    return decode(activeGuestSize_.load(std::memory_order_acquire));
}

DisplaySize DisplayGeometryPolicy::settledHostSize() const noexcept
{
    return decode(settledHostSize_.load(std::memory_order_acquire));
}

bool DisplayGeometryPolicy::autoResizeGuestEnabled() const noexcept
{
    return autoResizeGuest_;
}

bool DisplayGeometryPolicy::hostSizeSettled(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    const auto encoded = encode({width, height});
    settledHostSize_.store(encoded, std::memory_order_release);
    if (!autoResizeGuestEnabled())
        return false;
    const auto previous = advertisedSize_.exchange(
        encoded, std::memory_order_acq_rel);
    if (activeGuestSize_.load(std::memory_order_acquire) == encoded) {
        /* AppKit follows every applied guest mode. Its resulting drawable
         * callback settles at the already-active guest size and must not feed
         * that size back to the guest as another resize request. */
        return false;
    }
    return previous != encoded;
}

bool DisplayGeometryPolicy::guestScanoutChanged(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    if (width == 0 || height == 0)
        return false;
    const auto encoded = encode({width, height});
    return activeGuestSize_.exchange(encoded, std::memory_order_acq_rel) !=
        encoded;
}

void DisplayGeometryPolicy::resetGuestSession(DisplaySize activeSize) noexcept
{
    const auto settled = settledHostSize_.load(std::memory_order_acquire);
    advertisedSize_.store(settled, std::memory_order_release);
    const auto active = activeSize.width == 0 || activeSize.height == 0 ?
        settled : encode(activeSize);
    activeGuestSize_.store(active, std::memory_order_release);
}

std::uint64_t DisplayGeometryPolicy::encode(DisplaySize size) noexcept
{
    return static_cast<std::uint64_t>(size.width) << 32U | size.height;
}

DisplaySize DisplayGeometryPolicy::decode(std::uint64_t size) noexcept
{
    return {
        .width = static_cast<std::uint32_t>(size >> 32U),
        .height = static_cast<std::uint32_t>(size),
    };
}

} // namespace uml::gpu
