// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string_view>

namespace uml::app {

struct LaunchConfiguration {
	const char *rootfs_path {};
	std::string_view default_guest_memory;
	std::string_view default_guest_cpus;
	bool top_byte_ignore{};
	bool touchscreen_input{};
};

int runEmbeddedKernel(int argc, char *argv[], char *envp[],
		      const LaunchConfiguration &configuration);

} // namespace uml::app
