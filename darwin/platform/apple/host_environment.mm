// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_environment.hpp"

#import <Foundation/Foundation.h>

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

} // namespace uml::platform
