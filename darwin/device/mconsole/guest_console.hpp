// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace uml::device {

/*
 * UML's own management console is the only channel that reaches the guest
 * kernel while it runs, and the only way to make it write back pages it has
 * not yet handed to the block layer. Everything the host can flush by itself
 * stops at the block device.
 *
 * The guest binds a socket named after the run's umid and never removes it, so
 * the stock name gives the first guest on a host a console and every later one
 * a silent failure. The host therefore names each run, which also lets guests
 * run side by side, and owns the path that results.
 */
[[nodiscard]] std::string guest_management_console_umid();

/*
 * Asks the guest to write its filesystems back and waits for it to accept the
 * request, then allows a bounded settling period for the writeback itself.
 *
 * True means the guest confirmed the request. It is not a completion proof:
 * the guest schedules the writeback and reports that it started, so the
 * settling period is a bound rather than a guarantee. False means no console
 * answered, which is the normal result where the guest could not bind one.
 */
[[nodiscard]] bool request_guest_writeback(unsigned int settle_ms) noexcept;

/*
 * Freezes the guest where it stands, so that nothing it holds can be dirtied
 * again, and thaws it. A frozen guest issues no new writes, which is what
 * makes a written-back disk stay written back while an application waits to be
 * either resumed or reclaimed.
 *
 * Suspending returns false when no console answered. Resuming is best effort
 * and reports nothing: a guest that was never frozen has nothing to thaw.
 */
[[nodiscard]] bool suspend_guest(unsigned int settle_ms) noexcept;
void resume_guest() noexcept;

/* Removes this run's console path. Adopting it separately covers the
 * terminating-signal paths that never unwind. */
void release_guest_management_console() noexcept;

} // namespace uml::device
