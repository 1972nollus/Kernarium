// SPDX-License-Identifier: GPL-2.0-or-later

#include "launcher.hpp"
#include "block/android_boot.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::string_view android_default_guest_memory{"mem=4G"};
constexpr std::string_view android_default_guest_cpus{"ncpus=10"};
constexpr std::string_view release_option{"--android-release="};
constexpr std::string_view disk_option{"--android-disk="};
constexpr std::string_view boot_mode_option{"--android-boot-mode="};
constexpr std::string_view adb_authorization_prefix{
	"androidboot.kernarium.adb_auth="};

struct AndroidOptions {
	std::filesystem::path release;
	std::filesystem::path disk;
	uml::android::BootMode boot_mode = uml::android::BootMode::automatic;
	bool boot_mode_set = false;
	std::vector<std::string> forwarded;
};

uml::android::BootMode parse_boot_mode(std::string_view value)
{
	if (value == "auto")
		return uml::android::BootMode::automatic;
	if (value == "normal")
		return uml::android::BootMode::normal;
	if (value == "recovery")
		return uml::android::BootMode::recovery;
	throw std::invalid_argument("Android boot mode must be auto, normal, or recovery");
}

std::string environment_value(const char *name)
{
	const char *value = std::getenv(name);
	return value == nullptr ? std::string{} : std::string{value};
}

AndroidOptions parse_options(int argc, char *const argv[])
{
	AndroidOptions options;
	options.forwarded.reserve(static_cast<std::size_t>(argc) + 32U);
	options.forwarded.emplace_back(argv[0] == nullptr ? "uml-darwin-android17" :
							 argv[0]);
	bool has_gpu = false;
	bool has_gpu_display = false;
	bool has_audio = false;
	bool has_battery = false;
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument =
			argv[index] == nullptr ? std::string_view{} : argv[index];
		if (argument.starts_with(adb_authorization_prefix))
			throw std::invalid_argument(
				"macOS fixes Android ADB authorization at boot");
		if (argument.starts_with(release_option)) {
			if (!options.release.empty())
				throw std::invalid_argument(
					"--android-release may be specified only once");
			options.release = argument.substr(release_option.size());
			if (options.release.empty())
				throw std::invalid_argument(
					"--android-release requires a path");
			continue;
		}
		if (argument.starts_with(disk_option)) {
			if (!options.disk.empty())
				throw std::invalid_argument(
					"--android-disk may be specified only once");
			options.disk = argument.substr(disk_option.size());
			if (options.disk.empty())
				throw std::invalid_argument(
					"--android-disk requires a path");
			continue;
		}
		if (argument.starts_with(boot_mode_option)) {
			if (options.boot_mode_set)
				throw std::invalid_argument(
					"--android-boot-mode may be specified only once");
			options.boot_mode = parse_boot_mode(
				argument.substr(boot_mode_option.size()));
			options.boot_mode_set = true;
			continue;
		}
		has_gpu |= argument == "--gpu";
		has_gpu_display |= argument.starts_with("--gpu-display=");
		has_audio |= argument == "--audio";
		has_battery |= argument == "--battery";
		options.forwarded.emplace_back(argument);
	}
	if (options.release.empty())
		options.release = environment_value("UML_ANDROID17_RELEASE");
	if (options.disk.empty())
		options.disk = environment_value("UML_ANDROID17_DISK");
	if (!options.boot_mode_set) {
		const auto environment_mode = environment_value("UML_ANDROID17_BOOT_MODE");
		if (!environment_mode.empty())
			options.boot_mode = parse_boot_mode(environment_mode);
	}
	if (!has_gpu)
		options.forwarded.emplace_back("--gpu");
	if (!has_gpu_display)
		options.forwarded.emplace_back("--gpu-display=native");
	if (!has_audio)
		options.forwarded.emplace_back("--audio");
	if (!has_battery)
		options.forwarded.emplace_back("--battery");
	return options;
}

std::filesystem::path validated_disk(
	const std::filesystem::path &candidate)
{
	if (!candidate.is_absolute())
		throw std::invalid_argument("--android-disk must be absolute");
	uml::android::validateWritableDisk(candidate);
	return candidate;
}

std::filesystem::path validated_release_root(
	const std::filesystem::path &candidate)
{
	if (!candidate.is_absolute())
		throw std::invalid_argument("--android-release must be absolute");
	struct stat candidate_details {};
	if (lstat(candidate.c_str(), &candidate_details) != 0 ||
	    !S_ISDIR(candidate_details.st_mode))
		throw std::invalid_argument(
			"Android release must be a real local directory");
	std::error_code error;
	auto release = std::filesystem::canonical(candidate, error);
	if (error)
		throw std::invalid_argument("Android release cannot be resolved");
	return release;
}

std::vector<char *> argument_pointers(std::vector<std::string> &arguments)
{
	std::vector<char *> pointers;
	pointers.reserve(arguments.size());
	for (auto &argument : arguments)
		pointers.push_back(argument.data());
	return pointers;
}

void print_usage()
{
	std::fputs(
		"Android 17 options:\n"
			"  --android-release=/absolute/release\n"
			"  --android-disk=/absolute/writable-disk.img\n"
			"  --android-boot-mode=auto|normal|recovery\n"
			"  --virtio-console-socket=/absolute/console.sock\n"
			"The same values may be supplied through UML_ANDROID17_RELEASE, "
			"UML_ANDROID17_DISK, "
			"and UML_ANDROID17_BOOT_MODE.\n",
		stderr);
}

} // namespace

int main(int argc, char *argv[], char *envp[])
{
	try {
		const bool information_only = uml::app::informational(argc, argv);
		if (uml::app::has_argument(argc, argv, "--help"))
			print_usage();
		auto options = parse_options(argc, argv);
		if (!information_only) {
			if (options.release.empty() || options.disk.empty())
				throw std::invalid_argument(
					"Android launch requires release and disk paths");
			const auto release = validated_release_root(options.release);
			const auto disk = validated_disk(options.disk);
			const auto boot = uml::android::selectBoot(
				disk, options.boot_mode);
			options.forwarded.emplace_back("ubd0=" + disk.string());
			options.forwarded.emplace_back("initrd=" + disk.string());
			options.forwarded.emplace_back(
				"initrd_offset=" + std::to_string(boot.ramdisk.offset));
			options.forwarded.emplace_back(
				"initrd_size=" + std::to_string(boot.ramdisk.size));
			options.forwarded.emplace_back("root=/dev/ram0");
			options.forwarded.emplace_back("rootfstype=ramfs");
			constexpr std::array launcher_prefixes{
				adb_authorization_prefix};
			for (auto &argument : uml::android::readKernelCommandLine(
				     release / "boot/kernel-command-line.txt",
				     launcher_prefixes))
				options.forwarded.push_back(std::move(argument));
			options.forwarded.emplace_back(
				std::string{adb_authorization_prefix} + "0");
			std::fprintf(
				stderr,
					"Android rootfs %s, %s boot, writable disk %s\n",
					release.c_str(),
					boot.mode == uml::android::BootMode::recovery
						? "recovery" : "normal",
					disk.c_str());
		}
		auto pointers = argument_pointers(options.forwarded);
		return uml::app::runEmbeddedKernel(
			static_cast<int>(pointers.size()), pointers.data(), envp,
			{
				.rootfs_path = nullptr,
				.default_guest_memory = android_default_guest_memory,
				.default_guest_cpus = android_default_guest_cpus,
				.top_byte_ignore = true,
				.input_mode = uml::app::DisplayInput::touchscreen,
			});
	} catch (const std::exception &error) {
		std::fprintf(stderr, "Unable to configure Android guest: %s\n",
			     error.what());
		return 1;
	}
}
