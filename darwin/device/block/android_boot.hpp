// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UML_DEVICE_BLOCK_ANDROID_BOOT_HPP
#define UML_DEVICE_BLOCK_ANDROID_BOOT_HPP

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uml::android {

enum class BootMode : std::uint32_t {
	automatic = 0,
	normal = 1,
	recovery = 2,
};

struct FileRange {
	std::uint64_t offset;
	std::uint64_t size;
};

struct BootSelection {
	BootMode mode;
	FileRange ramdisk;
};

BootSelection selectBoot(const std::filesystem::path &disk,
			 BootMode requestedMode);

/* There is no bootloader to take the one-shot request that `reboot
 * bootloader` leaves in the misc partition's bootloader message (its command
 * field reads "bootonce-bootloader"). A bootloader that continues clears the
 * request and boots normally, and so does this: it clears the 32-byte command
 * field and returns true, or changes nothing and returns false when the field
 * holds anything else. Left in place, the request makes Android's init skip
 * the "boot-recovery" it writes for a later `reboot recovery`. Throws what
 * selectBoot throws for a disk that is not the expected Android layout, and
 * std::system_error when the field cannot be written. */
bool consumeBootloaderCommand(const std::filesystem::path &disk);

struct RestartBoot {
	BootSelection boot;
	/* The disk held a bootloader request, now cleared. */
	bool bootloaderConsumed;
};

/* The boot after the guest restarted itself: consumes a bootloader request,
 * then selects the ramdisk in automatic mode whatever mode the first boot was
 * given, so that `reboot recovery` boots recovery and anything else boots
 * normally. Recovery clears its own "boot-recovery" request when it is done. */
RestartBoot selectRestartBoot(const std::filesystem::path &disk);

/* Throws std::invalid_argument, naming `label`, for a path that a UBD kernel
 * argument cannot carry as itself (see uml::block::usable_ubd_path). */
void validateUbdPath(const std::filesystem::path &path, std::string_view label);

/* Checks the writable Android disk: a regular file, not a link, that this
 * process can read and write, at a path a UBD specification can carry.
 * Throws std::invalid_argument otherwise. */
void validateWritableDisk(const std::filesystem::path &disk);

/* The whitespace-separated arguments of a release's kernel command-line file.
 * Throws when the file is not a readable regular file, when it holds no
 * argument, or when an argument starts with one of `launcherPrefixes`, which
 * the launcher sets itself. */
std::vector<std::string> readKernelCommandLine(
	const std::filesystem::path &path,
	std::span<const std::string_view> launcherPrefixes);

/* The kernel argument that gives the guest its display density. Android init
 * imports it as ro.boot.lcd_density and the product's vendor init copies it to
 * ro.sf.lcd_density, so every launcher sets it and a release's command line
 * may not. */
inline constexpr std::string_view lcdDensityPrefix{"androidboot.lcd_density="};

/* The density SurfaceFlinger uses, in dots per inch, when a display has no
 * physical size to derive one from. */
inline constexpr std::uint32_t defaultLcdDensity = 213;

/* `androidboot.lcd_density=` followed by the density of the EDID the launcher
 * advertises (its --gpu-dpi), or defaultLcdDensity when it advertises none
 * (`dotsPerInch` is 0). */
std::string lcdDensityArgument(std::uint32_t dotsPerInch);

/* The dots per inch in the text of a --gpu-dpi value: a positive decimal
 * number with no other character. Throws std::invalid_argument otherwise. */
std::uint32_t parseDotsPerInch(std::string_view text);

} // namespace uml::android

#endif
