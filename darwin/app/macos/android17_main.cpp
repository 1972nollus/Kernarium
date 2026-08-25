// SPDX-License-Identifier: GPL-2.0-or-later

#include "launcher.hpp"
#include "block/android_boot.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef UML_ANDROID17_ACCEPTED_DIRECTORY_NAME
#error "Android release directory identity is required"
#endif
#ifndef UML_ANDROID17_ACCEPTED_RELEASE_ID
#error "Android release identity is required"
#endif
#ifndef UML_ANDROID17_ACCEPTED_PRODUCT_FAMILY
#error "Android product-family identity is required"
#endif
#ifndef UML_ANDROID17_BASE_DISK_SIZE
#error "Android base-disk size is required"
#endif
extern "C" {
extern const unsigned char uml_android_manifest_start[]
	__asm("section$start$__DATA$__uml_android");
extern const unsigned char uml_android_manifest_end[]
	__asm("section$end$__DATA$__uml_android");
}

namespace {

constexpr std::string_view android_default_guest_memory{"mem=4G"};
constexpr std::string_view android_default_guest_cpus{"ncpus=10"};
constexpr std::string_view release_option{"--android-release="};
constexpr std::string_view instance_option{"--android-instance="};
constexpr std::string_view boot_mode_option{"--android-boot-mode="};

struct AndroidOptions {
	std::filesystem::path release;
	std::filesystem::path instance;
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

bool starts_with_any(std::string_view value,
		     std::initializer_list<std::string_view> prefixes)
{
	for (const auto prefix : prefixes)
		if (value.starts_with(prefix))
			return true;
	return false;
}

void reject_protected_kernel_argument(std::string_view argument)
{
	if (starts_with_any(argument,
				    {"ubd0", "initrd=", "initrd_offset=", "initrd_size=",
				     "init=", "root=", "rootfstype=",
			     "androidboot.", "console=", "8250.nr_uarts=",
			     "firmware_class.path="}))
		throw std::invalid_argument(
			"release-owned kernel argument cannot be overridden: " +
			std::string{argument});
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
		if (!argument.starts_with("--"))
			reject_protected_kernel_argument(argument);
		options.forwarded.emplace_back(argument);
	}
	if (options.release.empty())
		options.release = environment_value("UML_ANDROID17_RELEASE");
	if (options.instance.empty())
		options.instance = environment_value("UML_ANDROID17_INSTANCE");
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
	return options;
}

std::size_t checked_regular_file_size(
	const std::filesystem::path &path, std::uintmax_t expected_size = 0)
{
	struct stat details {};
	if (lstat(path.c_str(), &details) != 0 || !S_ISREG(details.st_mode) ||
	    details.st_size < 0 ||
	    (expected_size != 0 &&
	     static_cast<std::uintmax_t>(details.st_size) != expected_size))
		throw std::runtime_error("release payload is missing or unsafe: " +
					 std::string{path});
	return static_cast<std::size_t>(details.st_size);
}

std::vector<unsigned char> read_regular_file(
	const std::filesystem::path &path, std::uintmax_t expected_size = 0)
{
	const auto size = checked_regular_file_size(path, expected_size);
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
	if (error || release.filename() != UML_ANDROID17_ACCEPTED_DIRECTORY_NAME)
		throw std::invalid_argument(
			"Android release directory does not match the accepted identity");
	const auto manifest = read_regular_file(release / "release-manifest.json");
	const std::size_t embedded_size = static_cast<std::size_t>(
		uml_android_manifest_end - uml_android_manifest_start);
	if (manifest.size() != embedded_size ||
	    !std::equal(manifest.begin(), manifest.end(),
		       uml_android_manifest_start))
		throw std::invalid_argument(
			"Android release manifest differs from the embedded identity");
	(void)checked_regular_file_size(
		release / "guest/android-17-uml-arm64.img",
		UML_ANDROID17_BASE_DISK_SIZE);
	return release;
}

void validate_ubd_path(const std::filesystem::path &path, const char *label)
{
	const auto value = path.string();
	if (value.find_first_of(",:") != std::string::npos)
		throw std::invalid_argument(std::string{label} +
			" path contains a UBD delimiter");
}

void write_receipt(const std::filesystem::path &path)
{
	const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (descriptor < 0) {
		if (errno == EEXIST)
			return;
		throw std::system_error(errno, std::generic_category(),
					"cannot create Android instance receipt");
	}
	const unsigned char *cursor = uml_android_manifest_start;
	std::size_t remaining = static_cast<std::size_t>(
		uml_android_manifest_end - uml_android_manifest_start);
	while (remaining != 0) {
		const ssize_t count = write(descriptor, cursor, remaining);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			const int error = count < 0 ? errno : EIO;
			close(descriptor);
			throw std::system_error(
				error, std::generic_category(),
				"cannot write Android instance receipt");
		}
		cursor += count;
		remaining -= static_cast<std::size_t>(count);
	}
	const int sync_error = fsync(descriptor) == 0 ? 0 : errno;
	const int close_error = close(descriptor) == 0 ? 0 : errno;
	if (sync_error != 0 || close_error != 0)
		throw std::system_error(sync_error != 0 ? sync_error : close_error,
					std::generic_category(),
					"cannot finish Android instance receipt");
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
	const auto receipt = std::filesystem::path{instance.string() +
							 ".release-manifest.json"};
	write_receipt(receipt);
	const auto stored = read_regular_file(receipt);
	const std::size_t embedded_size = static_cast<std::size_t>(
		uml_android_manifest_end - uml_android_manifest_start);
	if (stored.size() != embedded_size ||
	    !std::equal(stored.begin(), stored.end(), uml_android_manifest_start))
		throw std::invalid_argument(
			"Android instance belongs to another accepted release");
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
			"  --android-release=/absolute/accepted/release\n"
			"  --android-instance=/absolute/writable-instance.cow\n"
			"  --android-boot-mode=auto|normal|recovery\n"
			"  --virtio-console-socket=/absolute/console.sock\n"
			"The same values may be supplied through UML_ANDROID17_RELEASE, "
			"UML_ANDROID17_INSTANCE, and UML_ANDROID17_BOOT_MODE.\n",
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
			const auto base_disk =
				release / "guest/android-17-uml-arm64.img";
			const auto instance =
				validated_instance(options.instance, base_disk);
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
					"Android release %s (%s), %s boot, writable instance %s\n",
					UML_ANDROID17_ACCEPTED_RELEASE_ID,
					UML_ANDROID17_ACCEPTED_PRODUCT_FAMILY,
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
			});
	} catch (const std::exception &error) {
		std::fprintf(stderr, "Unable to configure Android guest: %s\n",
			     error.what());
		return 1;
	}
}
