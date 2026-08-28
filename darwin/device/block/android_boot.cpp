// SPDX-License-Identifier: GPL-2.0-or-later

#include "android_boot.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace uml::android {
namespace {

constexpr std::uint64_t sectorSize = 512;
constexpr std::uint64_t mebibyte = 1'048'576ULL;
constexpr std::uint64_t gibibyte = 1'073'741'824ULL;
constexpr std::uint64_t seedDiskSize = 1'983ULL * mebibyte;
constexpr std::uint64_t backupGptReservation = mebibyte;
constexpr std::uint64_t minimumDiskSizeGiB = 4;
constexpr std::uint64_t maximumDiskSizeGiB = 64;
constexpr std::uint64_t bootImagePageSize = 4096;
constexpr std::size_t bootImageHeaderSize = 1584;
constexpr std::string_view recoveryCommand = "boot-recovery";

struct ExpectedPartition {
	std::string_view name;
	std::uint64_t offset;
	std::uint64_t size;
};

constexpr std::array seedPartitions {
	ExpectedPartition{"system", 1'048'576ULL, 1'073'741'824ULL},
	ExpectedPartition{"system_ext", 1'074'790'400ULL, 268'435'456ULL},
	ExpectedPartition{"product", 1'343'225'856ULL, 268'435'456ULL},
	ExpectedPartition{"vendor", 1'611'661'312ULL, 268'435'456ULL},
	ExpectedPartition{"odm", 1'880'096'768ULL, 4'194'304ULL},
	ExpectedPartition{"metadata", 1'884'291'072ULL, 16'777'216ULL},
	ExpectedPartition{"init_boot", 1'901'068'288ULL, 8'388'608ULL},
	ExpectedPartition{"misc", 1'909'456'896ULL, 1'048'576ULL},
	ExpectedPartition{"recovery", 1'910'505'472ULL, 33'554'432ULL},
	ExpectedPartition{"userdata", 1'944'059'904ULL, 134'217'728ULL},
};

constexpr std::size_t initBootPartition = 6;
constexpr std::size_t miscPartition = 7;
constexpr std::size_t recoveryPartition = 8;
constexpr std::size_t userdataPartition = 9;

class File final {
public:
	explicit File(const std::filesystem::path &path)
	{
		struct stat details {};
		if (lstat(path.c_str(), &details) != 0)
			throw std::system_error(errno, std::generic_category(),
						"cannot inspect " + path.string());
		if (!S_ISREG(details.st_mode) || details.st_size < 0)
			throw std::invalid_argument("unsafe regular file: " + path.string());
		size_ = static_cast<std::uint64_t>(details.st_size);
		descriptor_ = open(path.c_str(), O_RDONLY);
		if (descriptor_ < 0)
			throw std::system_error(errno, std::generic_category(),
						"cannot open " + path.string());
	}

	~File()
	{
		if (descriptor_ >= 0)
			(void)close(descriptor_);
	}

	File(const File &) = delete;
	File &operator=(const File &) = delete;

	std::uint64_t size() const noexcept { return size_; }

	void read(std::uint64_t offset, std::span<std::byte> output) const
	{
		if (offset > size_ || output.size() > size_ - offset ||
		    offset > static_cast<std::uint64_t>(
				     std::numeric_limits<off_t>::max()))
			throw std::invalid_argument("file range is outside its payload");
		std::size_t completed = 0;
		while (completed < output.size()) {
			const auto absolute = offset + completed;
			const auto count = pread(descriptor_, output.data() + completed,
						 output.size() - completed,
						 static_cast<off_t>(absolute));
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0)
				throw std::system_error(
					count < 0 ? errno : EIO, std::generic_category(),
					"cannot read file range");
			completed += static_cast<std::size_t>(count);
		}
	}

private:
	int descriptor_ = -1;
	std::uint64_t size_ = 0;
};

std::uint32_t little32(const std::byte *input)
{
	return std::to_integer<std::uint32_t>(input[0]) |
		(std::to_integer<std::uint32_t>(input[1]) << 8U) |
		(std::to_integer<std::uint32_t>(input[2]) << 16U) |
		(std::to_integer<std::uint32_t>(input[3]) << 24U);
}

std::uint64_t little64(const std::byte *input)
{
	return static_cast<std::uint64_t>(little32(input)) |
		(static_cast<std::uint64_t>(little32(input + 4)) << 32U);
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

std::string partitionName(const std::byte *input)
{
	std::string result;
	for (std::size_t index = 0; index < 36; ++index) {
		const auto low = std::to_integer<unsigned int>(input[index * 2]);
		const auto high = std::to_integer<unsigned int>(input[index * 2 + 1]);
		if (low == 0 && high == 0)
			return result;
		if (high != 0 || low < 0x20U || low > 0x7eU)
			throw std::invalid_argument("GPT partition name is not ASCII");
		result.push_back(static_cast<char>(low));
	}
	throw std::invalid_argument("GPT partition name is not terminated");
}

void validatePartitionEntry(const std::byte *entry,
			    const ExpectedPartition &expected)
{
	const bool unused = std::all_of(entry, entry + 16,
					[](std::byte value) { return value == std::byte{}; });
	if (unused)
		throw std::invalid_argument("fixed GPT partition entry is unused");
	const auto firstLba = little64(entry + 32);
	const auto lastLba = little64(entry + 40);
	if (firstLba > lastLba || firstLba * sectorSize != expected.offset ||
	    (lastLba - firstLba + 1U) * sectorSize != expected.size ||
	    partitionName(entry + 56) != expected.name)
		throw std::invalid_argument("fixed GPT partition layout differs");
}

bool supportedDiskSize(std::uint64_t size)
{
	if (size == seedDiskSize)
		return true;
	return size % gibibyte == 0 &&
		size / gibibyte >= minimumDiskSizeGiB &&
		size / gibibyte <= maximumDiskSizeGiB;
}

ExpectedPartition expectedPartition(std::size_t index, std::uint64_t diskSize)
{
	auto expected = seedPartitions[index];
	if (index == userdataPartition)
		expected.size = diskSize - expected.offset - backupGptReservation;
	return expected;
}

void validateGptHeader(std::array<std::byte, sectorSize> header,
		       std::uint64_t currentLba, std::uint64_t backupLba,
		       std::uint64_t entriesLba, std::uint64_t lastUsableLba)
{
	if (std::memcmp(header.data(), "EFI PART", 8) != 0 ||
	    little32(header.data() + 8) != 0x00010000U ||
	    little32(header.data() + 12) != 92U ||
	    little64(header.data() + 24) != currentLba ||
	    little64(header.data() + 32) != backupLba ||
	    little64(header.data() + 40) != 34U ||
	    little64(header.data() + 48) != lastUsableLba ||
	    little64(header.data() + 72) != entriesLba ||
	    little32(header.data() + 80) != 128U ||
	    little32(header.data() + 84) != 128U)
		throw std::invalid_argument("Android disk has an invalid GPT header");
	const auto expectedHeaderCrc = little32(header.data() + 16);
	std::fill(header.begin() + 16, header.begin() + 20, std::byte{});
	if (crc32(std::span{header}.first(92)) != expectedHeaderCrc)
		throw std::invalid_argument("Android disk GPT header CRC differs");
}

void validateGpt(const File &disk)
{
	if (!supportedDiskSize(disk.size()))
		throw std::invalid_argument("Android disk has the wrong size");
	const auto physicalLastLba = disk.size() / sectorSize - 1U;
	std::array<std::byte, sectorSize> header {};
	disk.read(sectorSize, header);
	const auto declaredLastLba = little64(header.data() + 32);
	if (declaredLastLba != physicalLastLba)
		throw std::invalid_argument(
			"Android disk GPT capacity differs from its file");
	const auto lastUsableLba = declaredLastLba - 33U;
	validateGptHeader(header, 1U, declaredLastLba, 2U, lastUsableLba);

	std::vector<std::byte> entries(128U * 128U);
	disk.read(2U * sectorSize, entries);
	if (crc32(entries) != little32(header.data() + 88))
		throw std::invalid_argument("Android disk GPT entry CRC differs");
	for (std::size_t index = 0; index < seedPartitions.size(); ++index)
		validatePartitionEntry(entries.data() + index * 128U,
				       expectedPartition(index, disk.size()));
	for (std::size_t index = seedPartitions.size(); index < 128U; ++index)
		if (!std::all_of(entries.begin() + index * 128U,
				 entries.begin() + (index + 1U) * 128U,
				 [](std::byte value) { return value == std::byte{}; }))
			throw std::invalid_argument("Android disk has extra partitions");

	std::array<std::byte, sectorSize> backupHeader {};
	disk.read(declaredLastLba * sectorSize, backupHeader);
	validateGptHeader(backupHeader, declaredLastLba, 1U,
			  declaredLastLba - 32U,
			  lastUsableLba);
	if (!std::equal(header.begin() + 56, header.begin() + 72,
			backupHeader.begin() + 56) ||
	    little32(backupHeader.data() + 88) != little32(header.data() + 88))
		throw std::invalid_argument("Android disk GPT copies differ");
	std::vector<std::byte> backupEntries(entries.size());
	disk.read((declaredLastLba - 32U) * sectorSize, backupEntries);
	if (backupEntries != entries ||
	    crc32(backupEntries) != little32(backupHeader.data() + 88))
		throw std::invalid_argument("Android disk backup GPT entries differ");
}

FileRange bootRamdisk(const File &disk, const ExpectedPartition &partition)
{
	std::array<std::byte, bootImageHeaderSize> header {};
	disk.read(partition.offset, header);
	if (std::memcmp(header.data(), "ANDROID!", 8) != 0 ||
	    little32(header.data() + 8) != 0U ||
	    little32(header.data() + 20) != bootImageHeaderSize ||
	    little32(header.data() + 40) != 4U ||
	    little32(header.data() + 1580) != 0U ||
	    !std::all_of(header.begin() + 24, header.begin() + 40,
			 [](std::byte value) { return value == std::byte{}; }))
		throw std::invalid_argument("Android boot partition is not kernelless v4");
	const auto ramdiskSize = little32(header.data() + 12);
	if (ramdiskSize == 0 ||
	    ramdiskSize > partition.size - bootImagePageSize)
		throw std::invalid_argument("Android boot partition ramdisk is invalid");
	return FileRange{partition.offset + bootImagePageSize, ramdiskSize};
}

bool recoveryRequested(const File &disk)
{
	std::array<std::byte, 2048> bootloaderMessage {};
	disk.read(seedPartitions[miscPartition].offset, bootloaderMessage);
	const auto commandEnd = std::find(
		bootloaderMessage.begin(), bootloaderMessage.begin() + 32, std::byte{});
	if (commandEnd == bootloaderMessage.begin() + 32)
		throw std::invalid_argument("Android bootloader message command is malformed");
	const std::string_view command(
		reinterpret_cast<const char *>(bootloaderMessage.data()),
		static_cast<std::size_t>(commandEnd - bootloaderMessage.begin()));
	return command == recoveryCommand;
}

} // namespace

BootSelection selectBoot(const std::filesystem::path &diskPath,
			 BootMode requestedMode)
{
	if (requestedMode != BootMode::automatic &&
	    requestedMode != BootMode::normal && requestedMode != BootMode::recovery)
		throw std::invalid_argument("unknown Android boot mode");
	const File disk(diskPath);
	validateGpt(disk);
	const auto normal = bootRamdisk(disk, seedPartitions[initBootPartition]);
	const auto recovery = bootRamdisk(disk, seedPartitions[recoveryPartition]);
	const auto selected = requestedMode == BootMode::automatic
		? (recoveryRequested(disk) ? BootMode::recovery : BootMode::normal)
		: requestedMode;
	return BootSelection{selected,
			     selected == BootMode::recovery ? recovery : normal};
}

} // namespace uml::android
