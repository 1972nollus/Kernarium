// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::platform {

/*
 * Every host file the guest kernel opens passes through the host bridge, so
 * this registry is the only place that knows the complete set of descriptors a
 * running guest can still write to. A durability request flushes exactly that
 * set: the block backing files, and nothing the host itself owns.
 *
 * Read-only descriptors are not recorded. They carry nothing to flush, and
 * keeping them out bounds the registry to the handful of writable images a
 * guest actually has open.
 *
 * A record also remembers which file it described. That makes the registry
 * self-validating, so closing a descriptor needs no hook here: a stale record
 * is recognised at flush time and dropped, whether the descriptor is now
 * closed or has been reused for something else entirely.
 */
void record_guest_file(int descriptor, int open_flags) noexcept;

struct GuestFileSyncResult final {
	unsigned int flushed;
	unsigned int failed;
	unsigned int retired;
};

/*
 * Flushes every recorded descriptor that still names the file it was recorded
 * for. Records that no longer do are retired rather than flushed.
 */
[[nodiscard]] GuestFileSyncResult sync_guest_files() noexcept;

} // namespace uml::platform
