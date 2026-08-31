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

DisplayGeometryPolicy::DisplayGeometryPolicy(
    std::uint32_t width,
    std::uint32_t height,
    bool autoResizeGuest) noexcept
    : configuredAutoResizeGuest_(autoResizeGuest)
    , autoResizeGuestEnabled_(autoResizeGuest)
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

bool DisplayGeometryPolicy::configuredAutoResizeGuest() const noexcept
{
    return configuredAutoResizeGuest_;
}

bool DisplayGeometryPolicy::autoResizeGuestEnabled() const noexcept
{
    return autoResizeGuestEnabled_.load(std::memory_order_acquire);
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
        /* No SET_SCANOUT is required when the guest already presents this
         * size. Retire older targets so a later independent mode cannot be
         * mistaken for a delayed response to one of them. */
        hostTargetCount_ = 0;
    } else if (previous != encoded) {
        appendHostTarget(encoded);
    }
    if (previous == encoded) {
        return false;
    }
    return true;
}

GuestDisplayChange DisplayGeometryPolicy::guestScanoutChanged(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    if (width == 0 || height == 0)
        return {};
    const auto encoded = encode({width, height});
    const auto previous = activeGuestSize_.load(std::memory_order_acquire);
    if (hasActiveGuestScanout_ && encoded == previous) {
        (void)consumeHostTarget(encoded);
        return {};
    }

    const bool firstScanout = !hasActiveGuestScanout_;
    hasActiveGuestScanout_ = true;
    activeGuestSize_.store(encoded, std::memory_order_release);
    const bool expectedHostResponse = autoResizeGuestEnabled() &&
        (consumeHostTarget(encoded) ||
            advertisedSize_.load(std::memory_order_acquire) == encoded);
    if (firstScanout) {
        /* The first nonzero scanout establishes the guest's boot mode rather
         * than changing an already active resolution. */
        return {.changed = encoded != previous, .independent = false};
    }
    const bool independent = !expectedHostResponse;
    if (autoResizeGuestEnabled() && independent) {
        autoResizeGuestEnabled_.store(false, std::memory_order_release);
        hostTargetCount_ = 0;
    }
    return {.changed = true, .independent = independent};
}

void DisplayGeometryPolicy::resetGuestSession(DisplaySize activeSize) noexcept
{
    hostTargetCount_ = 0;
    hasActiveGuestScanout_ = false;
    autoResizeGuestEnabled_.store(
        configuredAutoResizeGuest_, std::memory_order_release);
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

void DisplayGeometryPolicy::appendHostTarget(std::uint64_t target) noexcept
{
    if (hostTargetCount_ == targetCapacity) {
        std::move(
            hostTargets_.begin() + 1,
            hostTargets_.end(),
            hostTargets_.begin());
        --hostTargetCount_;
    }
    hostTargets_[hostTargetCount_++] = target;
}

bool DisplayGeometryPolicy::consumeHostTarget(std::uint64_t target) noexcept
{
    std::size_t match = hostTargetCount_;
    for (std::size_t index = 0; index < hostTargetCount_; ++index) {
        if (hostTargets_[index] == target) {
            match = index;
            break;
        }
    }
    if (match == hostTargetCount_)
        return false;
    const auto consumed = match + 1U;
    std::move(
        hostTargets_.begin() + static_cast<std::ptrdiff_t>(consumed),
        hostTargets_.begin() + static_cast<std::ptrdiff_t>(hostTargetCount_),
        hostTargets_.begin());
    hostTargetCount_ -= consumed;
    return true;
}

} // namespace uml::gpu
