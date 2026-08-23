// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::runtime {

struct GuestSyncResult final {
	unsigned int flushed;
	unsigned int failed;
};

/*
 * Flushes every host file the running guest can still write to. This is the
 * durability boundary the host owns: writes the guest has already handed to
 * the block layer reach stable storage. It does not reach into the guest's own
 * page cache, which only the guest kernel can write back.
 */
[[nodiscard]] GuestSyncResult sync_guest_files() noexcept;

} // namespace uml::runtime
