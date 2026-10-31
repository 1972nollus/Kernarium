// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace uml::gpu {

/* Finds the guest contexts that have gone quiet.
 *
 * The host renderer's GL contexts keep what their last batches used until the
 * context submits again, which a compositor or a launcher with nothing to draw
 * does not do for minutes. A context is idle once a whole idle interval has
 * passed since its last activity (a command buffer, a transfer, a fence it
 * asked for, a fence that retired) and no fence it asked for is still
 * outstanding. It is reported twice, an idle interval apart: the trim of the
 * first report flushes the context, and the batch that flush submits has not
 * completed when the trim looks at it, so only the second trim can release
 * what that last batch holds. Any new activity makes it eligible again.
 *
 * Only contexts that were added are tracked. Safe to call from any thread. */
class ContextIdleTracker {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr auto defaultIdleInterval = std::chrono::seconds(1);
    /* How often an idle context is reported before it is active again. */
    static constexpr unsigned reportsPerIdlePeriod = 2;

    explicit ContextIdleTracker(
        Clock::duration idleInterval = defaultIdleInterval) noexcept;

    void add(std::uint32_t contextId, Clock::time_point now);
    void remove(std::uint32_t contextId);
    void reset();

    /* The context ran something on the host GL stack. */
    void activity(std::uint32_t contextId, Clock::time_point now);    /* Every context starts a new idle period. A texture that one context
     * deleted may still have a view that another context made, and a context
     * destroys its own views only when it next flushes: the context that
     * sampled a texture the host has just deleted has something to release
     * although nothing was sent to it. */
    void activityAll(Clock::time_point now);
    /* A fence on a ring of the context was asked for and later retired.
     * Cookies on a ring only grow, and retiring one retires all before it. */
    void fenceCreated(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t cookie,
        Clock::time_point now);
    void fenceRetired(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t cookie,
        Clock::time_point now);

    /* The contexts to trim at `now`. Each is reported at the end of its idle
     * interval and once more an interval later, then not again until it is
     * active. */
    [[nodiscard]] std::vector<std::uint32_t> takeIdle(Clock::time_point now);

    /* How long until a context is next due for a report, zero if one already
     * is, and nothing if no context is waiting for it: all of them have been
     * reported twice, or are waiting on a fence. */
    [[nodiscard]] std::optional<Clock::duration> nextDelay(
        Clock::time_point now) const;

private:
    struct Ring {
        std::uint64_t created {};
        std::uint64_t retired {};
    };
    struct Context {
        Clock::time_point lastActivity {};
        /* The reports since the last activity, and when the latest was made. */
        unsigned reports {};
        Clock::time_point reportedAt {};
        std::map<std::uint32_t, Ring> rings;

        [[nodiscard]] bool hasPendingFence() const noexcept;
        /* When the next report is due, or nothing once both were made or a
         * fence is outstanding. */
        [[nodiscard]] std::optional<Clock::time_point> nextReport(
            Clock::duration idleInterval) const noexcept;
    };

    Clock::duration idleInterval_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, Context> contexts_;
};

} // namespace uml::gpu
