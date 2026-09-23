// SPDX-License-Identifier: GPL-2.0-or-later

#include "launcher.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::string_view macos_default_guest_memory{"mem=8G"};
constexpr std::string_view macos_default_guest_cpus{"ncpus=10"};

bool has_argument(int argc, char *const argv[], std::string_view argument)
{
	for (int index = 1; index < argc; ++index)
		if (argv[index] != nullptr && argument == argv[index])
			return true;
	return false;
}

bool has_prefix(int argc, char *const argv[], std::string_view prefix)
{
	for (int index = 1; index < argc; ++index)
		if (argv[index] != nullptr &&
		    std::string_view{argv[index]}.starts_with(prefix))
			return true;
	return false;
}

bool informational(int argc, char *const argv[])
{
	return has_argument(argc, argv, "--help") ||
		has_argument(argc, argv, "--version") ||
		has_argument(argc, argv, "--showconfig");
}

bool usable_rootfs(const std::filesystem::path &path)
{
	struct stat details {};
	return stat(path.c_str(), &details) == 0 && S_ISREG(details.st_mode) &&
		access(path.c_str(), R_OK) == 0;
}

std::optional<std::filesystem::path> canonical_rootfs(
	const std::filesystem::path &candidate)
{
	std::error_code error;
	auto path = std::filesystem::canonical(candidate, error);
	if (!error && usable_rootfs(path))
		return path;
	return std::nullopt;
}

std::optional<std::filesystem::path> locate_rootfs()
{
	if (const char *configured = std::getenv("UML_ROOTFS");
	    configured != nullptr && configured[0] != '\0') {
		auto path = canonical_rootfs(configured);
		if (!path)
			std::fprintf(stderr,
				"UML_ROOTFS is not a readable EROFS image: %s\n",
				configured);
		return path;
	}

	std::array<char, 4096> executable {};
	std::uint32_t length = executable.size();
	if (_NSGetExecutablePath(executable.data(), &length) == 0) {
		std::error_code error;
		auto parent = std::filesystem::canonical(executable.data(), error)
			.parent_path();
		if (!error)
			if (auto path = canonical_rootfs(parent / "rootfs.erofs"))
				return path;
	}
	if (auto path = canonical_rootfs("rootfs.erofs"))
		return path;
	std::fputs("rootfs.erofs was not found beside the executable or in the "
		"current directory\n", stderr);
	return std::nullopt;
}

} // namespace

int main(int argc, char *argv[], char *envp[])
{
	std::string rootfs;
	if (!informational(argc, argv) && !has_prefix(argc, argv, "ubd0")) {
		auto located = locate_rootfs();
		if (!located)
			return 1;
		rootfs = located->string();
	}
	return uml::app::runEmbeddedKernel(
		argc, argv, envp,
		{
			.rootfs_path = rootfs.empty() ? nullptr : rootfs.c_str(),
			.default_guest_memory = macos_default_guest_memory,
			.default_guest_cpus = macos_default_guest_cpus,
		});
}
