// SPDX-License-Identifier: GPL-2.0-or-later

#include "android_boot.hpp"
#include "scratch_directory.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::uint64_t sectorSize = 512;
constexpr std::uint64_t backupGptReservation = 1'048'576ULL;
constexpr std::uint64_t seedDiskSize = 2'079'326'208ULL;
constexpr std::uint64_t expandedDiskSize = 8'589'934'592ULL;
constexpr std::uint64_t initBootOffset = 1'901'068'288ULL;
constexpr std::uint64_t initBootSize = 8'388'608ULL;
constexpr std::uint64_t miscOffset = 1'909'456'896ULL;
constexpr std::uint64_t recoveryOffset = 1'910'505'472ULL;
constexpr std::uint64_t recoverySize = 33'554'432ULL;
constexpr std::uint64_t normalRamdiskSize = 8192;
constexpr std::uint64_t recoveryRamdiskSize = 12288;

struct Partition {
	const char *name;
	std::uint64_t offset;
	std::uint64_t size;
};

constexpr std::array partitions {
	Partition{"system", 1'048'576ULL, 1'073'741'824ULL},
	Partition{"system_ext", 1'074'790'400ULL, 268'435'456ULL},
	Partition{"product", 1'343'225'856ULL, 268'435'456ULL},
	Partition{"vendor", 1'611'661'312ULL, 268'435'456ULL},
	Partition{"odm", 1'880'096'768ULL, 4'194'304ULL},
	Partition{"metadata", 1'884'291'072ULL, 16'777'216ULL},
	Partition{"init_boot", initBootOffset, initBootSize},
	Partition{"misc", miscOffset, 1'048'576ULL},
	Partition{"recovery", recoveryOffset, recoverySize},
	Partition{"userdata", 1'944'059'904ULL, 134'217'728ULL},
};

void require(bool condition)
{
	if (!condition)
		std::abort();
}

template<typename Operation>
void requireThrows(Operation &&operation)
{
	try {
		operation();
	} catch (const std::exception &) {
		return;
	}
	std::abort();
}

void little32(std::span<std::byte> output, std::size_t offset,
	      std::uint32_t value)
{
	for (unsigned int index = 0; index < 4; ++index)
		output[offset + index] = static_cast<std::byte>(value >> (index * 8U));
}

void little64(std::span<std::byte> output, std::size_t offset,
	      std::uint64_t value)
{
	for (unsigned int index = 0; index < 8; ++index)
		output[offset + index] = static_cast<std::byte>(value >> (index * 8U));
}

std::uint32_t crc32(std::span<const std::byte> input)
{
	std::uint32_t value = ~std::uint32_t{0};
	for (const auto byte : input) {
		value ^= std::to_integer<std::uint8_t>(byte);
		for (unsigned int bit = 0; bit < 8; ++bit)
			value = (value >> 1U) ^
				((value & 1U) != 0 ? 0xedb88320U : 0U);
	}
	return ~value;
}

void writeAll(int descriptor, std::uint64_t offset,
	      std::span<const std::byte> payload)
{
	std::size_t completed = 0;
	while (completed < payload.size()) {
		const auto count = pwrite(
			descriptor, payload.data() + completed, payload.size() - completed,
			static_cast<off_t>(offset + completed));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			throw std::system_error(
				count < 0 ? errno : EIO, std::generic_category(), "pwrite");
		completed += static_cast<std::size_t>(count);
	}
}

void writeBootImage(int descriptor, std::uint64_t offset,
		    std::uint32_t ramdiskSize)
{
	std::array<std::byte, 4096> header {};
	const char magic[] = "ANDROID!";
	for (std::size_t index = 0; index < 8; ++index)
		header[index] = static_cast<std::byte>(magic[index]);
	little32(header, 12, ramdiskSize);
	little32(header, 20, 1584);
	little32(header, 40, 4);
	writeAll(descriptor, offset, header);
	std::array<std::byte, 1> marker {std::byte{0x5a}};
	writeAll(descriptor, offset + 4096, marker);
}

std::filesystem::path scratchDirectory()
{
	const auto path = uml::testing::scratch_directory() /
		("uml-android-boot-tests-" + std::to_string(getpid()));
	std::filesystem::remove_all(path);
	std::filesystem::create_directories(path);
	return path;
}

std::filesystem::path makeDisk(const std::filesystem::path &directory,
			       std::uint64_t diskSize)
{
	const auto path = directory / "android.img";
	const int descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
	if (descriptor < 0)
		throw std::system_error(errno, std::generic_category(), "open disk");
	try {
		if (ftruncate(descriptor, static_cast<off_t>(diskSize)) != 0)
			throw std::system_error(errno, std::generic_category(), "ftruncate");
		std::vector<std::byte> entries(128U * 128U);
		for (std::size_t index = 0; index < partitions.size(); ++index) {
			auto entry = std::span{entries}.subspan(index * 128U, 128U);
			entry[0] = std::byte{1};
			entry[16] = static_cast<std::byte>(index + 1U);
			little64(entry, 32, partitions[index].offset / sectorSize);
			const auto size = index + 1U == partitions.size()
				? diskSize - partitions[index].offset - backupGptReservation
				: partitions[index].size;
			little64(entry, 40,
				 (partitions[index].offset + size) / sectorSize - 1U);
			const std::string name {partitions[index].name};
			for (std::size_t character = 0; character < name.size(); ++character)
				entry[56 + character * 2U] =
					static_cast<std::byte>(name[character]);
		}
		std::array<std::byte, 512> header {};
		const char signature[] = "EFI PART";
		for (std::size_t index = 0; index < 8; ++index)
			header[index] = static_cast<std::byte>(signature[index]);
		little32(header, 8, 0x00010000U);
		little32(header, 12, 92U);
		little64(header, 24, 1U);
		little64(header, 32, diskSize / sectorSize - 1U);
		little64(header, 40, 34U);
		little64(header, 48, diskSize / sectorSize - 34U);
		header[56] = std::byte{1};
		little64(header, 72, 2U);
		little32(header, 80, 128U);
		little32(header, 84, 128U);
		little32(header, 88, crc32(entries));
		little32(header, 16, crc32(std::span{header}.first(92)));
		writeAll(descriptor, sectorSize, header);
		writeAll(descriptor, 2U * sectorSize, entries);
		auto backupHeader = header;
		little64(backupHeader, 24, diskSize / sectorSize - 1U);
		little64(backupHeader, 32, 1U);
		little64(backupHeader, 72, diskSize / sectorSize - 33U);
		little32(backupHeader, 16, 0U);
		little32(backupHeader, 16,
			 crc32(std::span{backupHeader}.first(92)));
		writeAll(descriptor, diskSize - 33U * sectorSize, entries);
		writeAll(descriptor, diskSize - sectorSize, backupHeader);
		writeBootImage(descriptor, initBootOffset, normalRamdiskSize);
		writeBootImage(descriptor, recoveryOffset, recoveryRamdiskSize);
	} catch (...) {
		(void)close(descriptor);
		throw;
	}
	require(close(descriptor) == 0);
	return path;
}

void requestRecovery(const std::filesystem::path &disk)
{
	const int descriptor = open(disk.c_str(), O_RDWR);
	if (descriptor < 0)
		throw std::system_error(errno, std::generic_category(), "open disk");
	try {
		const std::string command = "boot-recovery";
		writeAll(descriptor, miscOffset,
			 std::as_bytes(std::span{command.data(), command.size() + 1U}));
	} catch (...) {
		(void)close(descriptor);
		throw;
	}
	require(close(descriptor) == 0);
}

} // namespace

int main()
{
	const auto directory = scratchDirectory();
	try {
		const auto stale = makeDisk(directory, seedDiskSize);
		std::filesystem::resize_file(stale, expandedDiskSize);
		requireThrows([&] {
			(void)uml::android::selectBoot(
				stale, uml::android::BootMode::normal);
		});
		require(std::filesystem::remove(stale));

		const auto disk = makeDisk(directory, expandedDiskSize);
		require(std::filesystem::file_size(disk) == expandedDiskSize);
		const auto automatic =
			uml::android::selectBoot(disk, uml::android::BootMode::automatic);
		require(automatic.mode == uml::android::BootMode::normal);
		require(automatic.ramdisk.offset == initBootOffset + 4096U);
		require(automatic.ramdisk.size == normalRamdiskSize);

		const auto explicitRecovery =
			uml::android::selectBoot(disk, uml::android::BootMode::recovery);
		require(explicitRecovery.mode == uml::android::BootMode::recovery);
		require(explicitRecovery.ramdisk.offset == recoveryOffset + 4096U);
		require(explicitRecovery.ramdisk.size == recoveryRamdiskSize);

		requestRecovery(disk);
		const auto requestedRecovery =
			uml::android::selectBoot(disk, uml::android::BootMode::automatic);
		require(requestedRecovery.mode == uml::android::BootMode::recovery);

		const int descriptor = open(disk.c_str(), O_RDWR);
		require(descriptor >= 0);
		std::array<std::byte, 1> corrupt {std::byte{0xff}};
		writeAll(descriptor, sectorSize + 20U, corrupt);
		require(close(descriptor) == 0);
		requireThrows([&] {
			(void)uml::android::selectBoot(
				disk, uml::android::BootMode::normal);
		});
	} catch (...) {
		std::filesystem::remove_all(directory);
		throw;
	}
	std::filesystem::remove_all(directory);
	return 0;
}
