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
 * outstanding. It is reported once; any new activity makes it eligible again.
 *
 * Only contexts that were added are tracked. Safe to call from any thread. */
class ContextIdleTracker {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr auto defaultIdleInterval = std::chrono::seconds(1);

    explicit ContextIdleTracker(
        Clock::duration idleInterval = defaultIdleInterval) noexcept;

    void add(std::uint32_t contextId, Clock::time_point now);
    void remove(std::uint32_t contextId);
    void reset();

    /* The context ran something on the host GL stack. */
    void activity(std::uint32_t contextId, Clock::time_point now);
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

    /* The contexts to trim at `now`, each reported until it is active again. */
    [[nodiscard]] std::vector<std::uint32_t> takeIdle(Clock::time_point now);

    /* How long until a context becomes idle, zero if one already is, and
     * nothing if no context is waiting for it: all of them have been
     * reported, or are waiting on a fence. */
    [[nodiscard]] std::optional<Clock::duration> nextDelay(
        Clock::time_point now) const;

private:
    struct Ring {
        std::uint64_t created {};
        std::uint64_t retired {};
    };
    struct Context {
        Clock::time_point lastActivity {};
        bool reported {};
        std::map<std::uint32_t, Ring> rings;

        [[nodiscard]] bool hasPendingFence() const noexcept;
    };

    Clock::duration idleInterval_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, Context> contexts_;
};

} // namespace uml::gpu
