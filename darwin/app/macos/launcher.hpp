// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string_view>

namespace uml::app {

enum class DisplayInput { mouse, touchscreen };

struct LaunchConfiguration {
	const char *rootfs_path {};
	std::string_view default_guest_memory;
	std::string_view default_guest_cpus;
	bool top_byte_ignore{};
	DisplayInput input_mode{DisplayInput::mouse};
	/* A file or directory the launcher created for this run only, removed
	 * once the guest stops. */
	std::filesystem::path run_scratch;
};

int runEmbeddedKernel(int argc, char *argv[], char *envp[],
		      const LaunchConfiguration &configuration);

/* Whether one of argv[1..argc) is exactly `argument`. */
[[nodiscard]] bool has_argument(
	int argc, char *const argv[], std::string_view argument);
/* Whether the run only prints information (--help, --version or
 * --showconfig) and boots no guest. */
[[nodiscard]] bool informational(int argc, char *const argv[]);

} // namespace uml::app
