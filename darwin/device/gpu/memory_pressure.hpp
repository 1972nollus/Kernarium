// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace uml::gpu {

/* When the host's graphics memory counts as under pressure, for a guest that
 * subscribed to hear of it (the Virtio-GPU memory report, kernel/common/gpu).
 *
 * The budget B is refused by the pool's usage, which also holds memory no
 * guest context accounts for: the host drivers' caches, and the surfaces the
 * system counts. That overhead only grows when a driver caches more, and a
 * guest cannot free it, so the condition counts the smallest overhead seen
 * (the pool's usage less what the contexts hold, never below zero) and is
 *
 *   context bytes + overhead + margin >= B
 *
 * Pressure begins once the condition has held for `sustain` and ends once it
 * has been false for `calm`, so a spike that passes is not reported. A refused
 * allocation is an event of its own, raised at most once per `refusalInterval`.
 * An event is raised on a change of the pressure and on a refusal, and never
 * otherwise.
 *
 * The class keeps no clock and no timer: the caller reads the figures, passes
 * the time, and arms a timer for deadline() when it returns one. It does
 * nothing while nobody is subscribed. Not thread-safe. */
class MemoryPressure {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::uint64_t margin = 96ULL * 1024ULL * 1024ULL;
    static constexpr Clock::duration sustain = std::chrono::seconds(1);
    static constexpr Clock::duration calm = std::chrono::seconds(5);
    static constexpr Clock::duration refusalInterval = std::chrono::seconds(1);

    /* The figures one evaluation reads, at one instant. */
    struct Reading {
        /* B; 0 when the process has no budget. */
        std::uint64_t budgetBytes {};
        /* The pool's usage. */
        std::uint64_t usageBytes {};
        /* What the guest's contexts hold of it. */
        std::uint64_t contextBytes {};
        /* Allocations refused so far. */
        std::uint64_t refusals {};
    };

    /* What a reply reports. */
    struct State {
        bool pressure {};
        /* An allocation was refused since the previous reply. */
        bool refused {};
    };

    /* The guest subscribed. Nothing changes when it already has: every query
     * of an armed reclaimer carries the subscription. */
    void subscribe(const Reading& reading) noexcept;
    void unsubscribe() noexcept;
    [[nodiscard]] bool subscribed() const noexcept { return subscribed_; }

    /* The figures or the time changed. Returns whether the event is to be
     * raised now. */
    [[nodiscard]] bool evaluate(
        Clock::time_point now, const Reading& reading) noexcept;

    /* The reply to a query, evaluated at `now`: the state the guest is told,
     * so a change it carries needs no event. The refusals it reports are
     * consumed. */
    [[nodiscard]] State query(
        Clock::time_point now, const Reading& reading) noexcept;

    /* When to evaluate again if nothing changes before, or nothing. */
    [[nodiscard]] std::optional<Clock::time_point> deadline() const noexcept;

    [[nodiscard]] bool pressure() const noexcept { return pressure_; }
    /* The smallest overhead seen since the subscription. */
    [[nodiscard]] std::uint64_t overheadBytes() const noexcept
    {
        return overhead_;
    }

private:
    /* Updates the overhead, the pressure and the refusals, and returns
     * whether the pressure changed. */
    [[nodiscard]] bool step(
        Clock::time_point now, const Reading& reading) noexcept;

    bool subscribed_ {};
    bool high_ {};
    bool pressure_ {};
    bool hasOverhead_ {};
    bool queried_ {};
    bool refusalUnraised_ {};
    std::uint64_t overhead_ {};
    std::uint64_t seenRefusals_ {};
    std::uint64_t queriedRefusals_ {};
    std::optional<Clock::time_point> highSince_;
    std::optional<Clock::time_point> calmSince_;
    std::optional<Clock::time_point> lastRefusalEvent_;
};

} // namespace uml::gpu
