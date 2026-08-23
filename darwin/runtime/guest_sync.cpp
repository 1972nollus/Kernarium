// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_sync.hpp"

#include "guest_files.hpp"

namespace uml::runtime {

GuestSyncResult sync_guest_files() noexcept
{
	const auto result = ::uml::platform::sync_guest_files();
	return {.flushed = result.flushed, .failed = result.failed};
}

} // namespace uml::runtime
