// SPDX-License-Identifier: GPL-2.0-or-later

#include "launcher.hpp"
#include "block/android_boot.hpp"

#include <cerrno>
#include <charconv>
#include <cstdint>
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
constexpr std::string_view instance_option{"--android-instance="};
constexpr std::string_view base_disk_option{"--android-base-disk="};
constexpr std::string_view disk_size_option{"--android-disk-size-gib="};
constexpr std::string_view boot_mode_option{"--android-boot-mode="};

struct AndroidOptions {
	std::filesystem::path release;
	std::filesystem::path instance;
	std::filesystem::path base_disk;
	std::uint32_t disk_size_gib = uml::android::defaultDiskSizeGiB;
	uml::android::BootMode boot_mode = uml::android::BootMode::automatic;
	bool boot_mode_set = false;
	bool disk_size_set = false;
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

std::uint32_t parse_disk_size(std::string_view value)
{
	std::uint32_t result = 0;
	const auto [end, error] =
		std::from_chars(value.data(), value.data() + value.size(), result);
	if (error != std::errc{} || end != value.data() + value.size() ||
	    result < uml::android::minimumDiskSizeGiB ||
	    result > uml::android::maximumDiskSizeGiB)
		throw std::invalid_argument(
			"Android disk capacity must be 4 through 64 GiB");
	return result;
}

bool has_argument(int argc, char *const argv[], std::string_view argument)
{
	for (int index = 1; index < argc; ++index)
		if (argv[index] != nullptr && argument == argv[index])
			return true;
	return false;
}

bool informational(int argc, char *const argv[])
{
	return has_argument(argc, argv, "--help") ||
		has_argument(argc, argv, "--version") ||
		has_argument(argc, argv, "--showconfig");
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
		if (argument.starts_with(instance_option)) {
			if (!options.instance.empty())
				throw std::invalid_argument(
					"--android-instance may be specified only once");
			options.instance = argument.substr(instance_option.size());
			if (options.instance.empty())
				throw std::invalid_argument(
					"--android-instance requires a path");
			continue;
		}
		if (argument.starts_with(base_disk_option)) {
			if (!options.base_disk.empty())
				throw std::invalid_argument(
					"--android-base-disk may be specified only once");
			options.base_disk = argument.substr(base_disk_option.size());
			if (options.base_disk.empty())
				throw std::invalid_argument(
					"--android-base-disk requires a path");
			continue;
		}
		if (argument.starts_with(disk_size_option)) {
			if (options.disk_size_set)
				throw std::invalid_argument(
					"--android-disk-size-gib may be specified only once");
			options.disk_size_gib = parse_disk_size(
				argument.substr(disk_size_option.size()));
			options.disk_size_set = true;
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
	if (options.instance.empty())
		options.instance = environment_value("UML_ANDROID17_INSTANCE");
	if (options.base_disk.empty())
		options.base_disk = environment_value("UML_ANDROID17_BASE_DISK");
	if (!options.disk_size_set) {
		const auto environment_size =
			environment_value("UML_ANDROID17_DISK_SIZE_GIB");
		if (!environment_size.empty())
			options.disk_size_gib = parse_disk_size(environment_size);
	}
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

std::filesystem::path validated_base_disk(
	const std::filesystem::path &candidate)
{
	if (!candidate.is_absolute())
		throw std::invalid_argument("--android-base-disk must be absolute");
	struct stat details {};
	if (lstat(candidate.c_str(), &details) != 0 ||
	    !S_ISREG(details.st_mode))
		throw std::invalid_argument("Android base disk is unsafe");
	return candidate;
}

std::size_t checked_regular_file_size(const std::filesystem::path &path)
{
	struct stat details {};
	if (lstat(path.c_str(), &details) != 0 || !S_ISREG(details.st_mode) ||
	    details.st_size < 0)
		throw std::runtime_error("release payload is missing or unsafe: " +
					 std::string{path});
	return static_cast<std::size_t>(details.st_size);
}

std::vector<unsigned char> read_regular_file(const std::filesystem::path &path)
{
	const auto size = checked_regular_file_size(path);
	FILE *file = std::fopen(path.c_str(), "rb");
	if (file == nullptr)
		throw std::system_error(errno, std::generic_category(),
					"cannot open " + std::string{path});
	std::vector<unsigned char> payload(size);
	const auto read = std::fread(payload.data(), 1, payload.size(), file);
	const int close_result = std::fclose(file);
	if (read != payload.size() || close_result != 0)
		throw std::runtime_error("cannot read release payload: " +
					 std::string{path});
	return payload;
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

void validate_ubd_path(const std::filesystem::path &path, const char *label)
{
	const auto value = path.string();
	if (value.find_first_of(",:") != std::string::npos)
		throw std::invalid_argument(std::string{label} +
			" path contains a UBD delimiter");
}

std::filesystem::path validated_instance(
	const std::filesystem::path &candidate,
	const std::filesystem::path &base_disk)
{
	if (!candidate.is_absolute())
		throw std::invalid_argument("--android-instance must be absolute");
	std::error_code error;
	auto parent = std::filesystem::canonical(candidate.parent_path(), error);
	if (error || access(parent.c_str(), W_OK) != 0)
		throw std::invalid_argument(
			"Android instance parent is missing or not writable");
	auto instance = parent / candidate.filename();
	validate_ubd_path(instance, "Android instance");
	validate_ubd_path(base_disk, "Android base disk");
	struct stat base_details {};
	if (stat(base_disk.c_str(), &base_details) != 0 ||
	    !S_ISREG(base_details.st_mode))
		throw std::invalid_argument("Android base disk is unsafe");
	struct stat instance_details {};
	if (lstat(instance.c_str(), &instance_details) == 0) {
		if (!S_ISREG(instance_details.st_mode) ||
		    (instance_details.st_dev == base_details.st_dev &&
		     instance_details.st_ino == base_details.st_ino))
			throw std::invalid_argument("Android instance disk is unsafe");
	} else if (errno != ENOENT) {
		throw std::system_error(errno, std::generic_category(),
					"cannot inspect Android instance disk");
	}
	return instance;
}

void append_kernel_command_line(
	std::vector<std::string> &arguments, const std::filesystem::path &path)
{
	const auto payload = read_regular_file(path);
	const std::string text(payload.begin(), payload.end());
	std::size_t cursor = 0;
	std::size_t appended = 0;
	while (cursor < text.size()) {
		while (cursor < text.size() &&
		       (text[cursor] == ' ' || text[cursor] == '\n' ||
			text[cursor] == '\t'))
			++cursor;
		const auto begin = cursor;
		while (cursor < text.size() && text[cursor] != ' ' &&
		       text[cursor] != '\n' && text[cursor] != '\t')
			++cursor;
		if (cursor != begin) {
			arguments.emplace_back(text.substr(begin, cursor - begin));
			++appended;
		}
	}
	if (appended == 0)
		throw std::invalid_argument("Android kernel command line is empty");
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
			"  --android-instance=/absolute/writable-instance.cow\n"
			"  --android-base-disk=/absolute/writable-base.img\n"
			"  --android-disk-size-gib=4..64\n"
			"  --android-boot-mode=auto|normal|recovery\n"
			"  --virtio-console-socket=/absolute/console.sock\n"
			"The same values may be supplied through UML_ANDROID17_RELEASE, "
			"UML_ANDROID17_INSTANCE, UML_ANDROID17_BASE_DISK, "
			"UML_ANDROID17_DISK_SIZE_GIB, and UML_ANDROID17_BOOT_MODE.\n",
		stderr);
}

} // namespace

int main(int argc, char *argv[], char *envp[])
{
	try {
		const bool information_only = informational(argc, argv);
		if (has_argument(argc, argv, "--help"))
			print_usage();
		auto options = parse_options(argc, argv);
		if (!information_only) {
			if (options.release.empty() || options.instance.empty())
				throw std::invalid_argument(
					"Android launch requires release and instance paths");
			const auto release = validated_release_root(options.release);
			const auto base_disk = validated_base_disk(
				options.base_disk.empty()
					? release / "guest/android-17-uml-arm64.img"
					: options.base_disk);
			const auto instance =
				validated_instance(options.instance, base_disk);
			uml::android::prepareDisk(
				base_disk, instance, options.disk_size_gib);
			const auto boot = uml::android::selectBoot(
				base_disk, instance, options.boot_mode);
			options.forwarded.emplace_back("ubd0=" + instance.string() +
						       "," + base_disk.string());
			options.forwarded.emplace_back("initrd=" + base_disk.string());
			options.forwarded.emplace_back(
				"initrd_offset=" + std::to_string(boot.ramdisk.offset));
			options.forwarded.emplace_back(
				"initrd_size=" + std::to_string(boot.ramdisk.size));
			options.forwarded.emplace_back("root=/dev/ram0");
			options.forwarded.emplace_back("rootfstype=ramfs");
			append_kernel_command_line(
				options.forwarded,
				release / "boot/kernel-command-line.txt");
			std::fprintf(
				stderr,
					"Android rootfs %s, %s boot, writable instance %s\n",
					release.c_str(),
					boot.mode == uml::android::BootMode::recovery
						? "recovery" : "normal",
					instance.c_str());
		}
		auto pointers = argument_pointers(options.forwarded);
		return uml::app::runEmbeddedKernel(
			static_cast<int>(pointers.size()), pointers.data(), envp,
			{
				.rootfs_path = nullptr,
				.default_guest_memory = android_default_guest_memory,
				.default_guest_cpus = android_default_guest_cpus,
				.top_byte_ignore = true,
				.touchscreen_input = true,
			});
	} catch (const std::exception &error) {
		std::fprintf(stderr, "Unable to configure Android guest: %s\n",
			     error.what());
		return 1;
	}
}
