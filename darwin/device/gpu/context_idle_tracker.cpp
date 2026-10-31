// SPDX-License-Identifier: GPL-2.0-or-later

#include "context_idle_tracker.hpp"

#include <algorithm>

namespace uml::gpu {

bool ContextIdleTracker::Context::hasPendingFence() const noexcept
{
    return std::ranges::any_of(rings, [](const auto& ring) {
        return ring.second.created > ring.second.retired;
    });
}

ContextIdleTracker::ContextIdleTracker(Clock::duration idleInterval) noexcept
    : idleInterval_(idleInterval)
{
}

void ContextIdleTracker::add(std::uint32_t contextId, Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    contexts_[contextId] = Context {.lastActivity = now};
}

void ContextIdleTracker::remove(std::uint32_t contextId)
{
    std::lock_guard lock(mutex_);
    contexts_.erase(contextId);
}

void ContextIdleTracker::reset()
{
    std::lock_guard lock(mutex_);
    contexts_.clear();
}

void ContextIdleTracker::activity(
    std::uint32_t contextId, Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    const auto found = contexts_.find(contextId);
    if (found == contexts_.end())
        return;
    found->second.lastActivity = std::max(found->second.lastActivity, now);
    found->second.reported = false;
}

void ContextIdleTracker::fenceCreated(
    std::uint32_t contextId,
    std::uint32_t ringIndex,
    std::uint64_t cookie,
    Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    const auto found = contexts_.find(contextId);
    if (found == contexts_.end())
        return;
    auto& ring = found->second.rings[ringIndex];
    ring.created = std::max(ring.created, cookie);
    found->second.lastActivity = std::max(found->second.lastActivity, now);
    found->second.reported = false;
}

void ContextIdleTracker::fenceRetired(
    std::uint32_t contextId,
    std::uint32_t ringIndex,
    std::uint64_t cookie,
    Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    const auto found = contexts_.find(contextId);
    if (found == contexts_.end())
        return;
    auto& ring = found->second.rings[ringIndex];
    ring.retired = std::max(ring.retired, cookie);
    /* A retirement the tracker was not told of (a fence of a ring it never saw
     * created) cannot leave a fence pending. */
    ring.created = std::max(ring.created, ring.retired);
    found->second.lastActivity = std::max(found->second.lastActivity, now);
    found->second.reported = false;
}

std::vector<std::uint32_t> ContextIdleTracker::takeIdle(Clock::time_point now)
{
    std::vector<std::uint32_t> idle;
    std::lock_guard lock(mutex_);
    for (auto& [contextId, context] : contexts_) {
        if (context.reported || context.hasPendingFence() ||
            now - context.lastActivity < idleInterval_) {
            continue;
        }
        context.reported = true;
        idle.push_back(contextId);
    }
    std::ranges::sort(idle);
    return idle;
}

std::optional<ContextIdleTracker::Clock::duration>
ContextIdleTracker::nextDelay(Clock::time_point now) const
{
    std::optional<Clock::duration> delay;
    std::lock_guard lock(mutex_);
    for (const auto& entry : contexts_) {
        const auto& context = entry.second;
        if (context.reported || context.hasPendingFence())
            continue;
        const auto due = context.lastActivity + idleInterval_ - now;
        const auto wait = std::max(due, Clock::duration::zero());
        if (!delay || wait < *delay)
            delay = wait;
    }
    return delay;
}

} // namespace uml::gpu
