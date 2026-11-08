// SPDX-License-Identifier: GPL-2.0-or-later

#include "memory_pressure.hpp"

#include <algorithm>

namespace uml::gpu {

void MemoryPressure::subscribe(const Reading& reading) noexcept
{
    if (subscribed_)
        return;
    *this = {};
    subscribed_ = true;
    hasOverhead_ = true;
    overhead_ = reading.usageBytes > reading.contextBytes
        ? reading.usageBytes - reading.contextBytes
        : 0;
    seenRefusals_ = reading.refusals;
    queriedRefusals_ = reading.refusals;
    queried_ = true;
}

void MemoryPressure::unsubscribe() noexcept
{
    *this = {};
}

bool MemoryPressure::step(
    Clock::time_point now, const Reading& reading) noexcept
{
    const std::uint64_t overhead = reading.usageBytes > reading.contextBytes
        ? reading.usageBytes - reading.contextBytes
        : 0;
    overhead_ = hasOverhead_ ? std::min(overhead_, overhead) : overhead;
    hasOverhead_ = true;

    /* Compared as "held >= B - margin" so that no sum can overflow. */
    const std::uint64_t threshold = reading.budgetBytes -
        std::min(reading.budgetBytes, margin);
    const std::uint64_t held = reading.contextBytes > UINT64_MAX - overhead_
        ? UINT64_MAX
        : reading.contextBytes + overhead_;
    high_ = reading.budgetBytes != 0 && held >= threshold;

    bool changed = false;
    if (high_) {
        calmSince_.reset();
        if (!highSince_)
            highSince_ = now;
        if (!pressure_ && now - *highSince_ >= sustain) {
            pressure_ = true;
            changed = true;
        }
    } else {
        highSince_.reset();
        if (pressure_) {
            if (!calmSince_)
                calmSince_ = now;
            if (now - *calmSince_ >= calm) {
                pressure_ = false;
                calmSince_.reset();
                changed = true;
            }
        }
    }

    if (reading.refusals > seenRefusals_)
        refusalUnraised_ = true;
    seenRefusals_ = reading.refusals;
    return changed;
}

bool MemoryPressure::evaluate(
    Clock::time_point now, const Reading& reading) noexcept
{
    if (!subscribed_)
        return false;
    bool raise = step(now, reading);
    if (refusalUnraised_ &&
        (!lastRefusalEvent_ || now - *lastRefusalEvent_ >= refusalInterval)) {
        raise = true;
        refusalUnraised_ = false;
        lastRefusalEvent_ = now;
    }
    return raise;
}

MemoryPressure::State MemoryPressure::query(
    Clock::time_point now, const Reading& reading) noexcept
{
    if (subscribed_)
        (void)step(now, reading);
    State state {.pressure = pressure_};
    if (queried_ && reading.refusals > queriedRefusals_)
        state.refused = true;
    queriedRefusals_ = reading.refusals;
    queried_ = true;
    /* The reply tells the guest of the refusals, so no event follows. */
    refusalUnraised_ = false;
    return state;
}

std::optional<MemoryPressure::Clock::time_point> MemoryPressure::deadline()
    const noexcept
{
    if (!subscribed_)
        return std::nullopt;
    std::optional<Clock::time_point> earliest;
    const auto consider = [&](Clock::time_point when) {
        if (!earliest || when < *earliest)
            earliest = when;
    };
    if (high_ && !pressure_ && highSince_)
        consider(*highSince_ + sustain);
    if (!high_ && pressure_ && calmSince_)
        consider(*calmSince_ + calm);
    if (refusalUnraised_ && lastRefusalEvent_)
        consider(*lastRefusalEvent_ + refusalInterval);
    return earliest;
}

} // namespace uml::gpu
