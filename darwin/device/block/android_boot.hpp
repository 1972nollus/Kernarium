// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UML_DEVICE_BLOCK_ANDROID_BOOT_HPP
#define UML_DEVICE_BLOCK_ANDROID_BOOT_HPP

#include <cstdint>
#include <filesystem>

namespace uml::android {

inline constexpr std::uint32_t minimumDiskSizeGiB = 4;
inline constexpr std::uint32_t maximumDiskSizeGiB = 64;
inline constexpr std::uint32_t defaultDiskSizeGiB = 8;

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

void prepareDisk(const std::filesystem::path &baseDisk,
		 const std::filesystem::path &instance,
		 std::uint32_t requestedSizeGiB);

BootSelection selectBoot(const std::filesystem::path &baseDisk,
			 const std::filesystem::path &instance,
			 BootMode requestedMode);

} // namespace uml::android

#endif
