// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>

namespace uml::gpu {

/* What to do with a deferred fence the renderer has not retired yet.
 *
 * A renderer fence can stay busy because the guest command carrying the
 * signal it waits for is itself held back by fence ordering. Dropping the
 * connection resolves nothing there: it removes the GPU device from a guest
 * that is still able to finish its workload and power off, and turns one
 * wedged submission into a dead session. The device therefore escapes the
 * deadlock first and only then gives up on the renderer, so a stuck fence
 * costs the workload that owns it rather than every workload sharing the
 * session. */
enum class OverdueFenceAction {
    /* Still inside its window. */
    wait,
    /* Release queue ordering so a held-back signal can reach the renderer,
     * and grant the fence a second window. */
    releaseOrdering,
    /* The second window expired: complete the fence without its renderer
     * work and keep serving the connection. */
    retire,
};

[[nodiscard]] constexpr OverdueFenceAction classifyOverdueFence(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point deadline,
    bool orderingAlreadyReleased) noexcept
{
    if (now < deadline)
        return OverdueFenceAction::wait;
    return orderingAlreadyReleased
        ? OverdueFenceAction::retire
        : OverdueFenceAction::releaseOrdering;
}

} // namespace uml::gpu
