// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_environment.hpp"

#import <Foundation/Foundation.h>
#include <mach/mach.h>

namespace uml::platform {

bool running_ios_app_on_macos() noexcept
{
	/* The answer is fixed for the life of the process, and callers reach it
	 * on the boot path, so resolve it once. */
	static const bool answer = [] {
		@autoreleasepool {
			return NSProcessInfo.processInfo.isiOSAppOnMac ? true
								       : false;
		}
	}();
	return answer;
}

std::uint64_t process_memory_limit_bytes() noexcept
{
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO,
		    reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS ||
	    count < TASK_VM_INFO_COUNT || info.limit_bytes_remaining == 0)
		return 0;
	/* What the footprint may still grow by, on top of what it is. */
	return info.phys_footprint + info.limit_bytes_remaining;
}

} // namespace uml::platform
