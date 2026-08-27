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
constexpr std::uint64_t diskSize = 8'589'934'592ULL;
constexpr std::uint64_t bootImagePageSize = 4096;
constexpr std::size_t bootImageHeaderSize = 1584;
constexpr std::size_t cowHeaderSize = 4128;
constexpr std::uint32_t cowVersion = 3;
constexpr std::uint32_t cowSectorSize = 512;
constexpr std::uint32_t cowAlignment = 16384;
constexpr std::string_view recoveryCommand = "boot-recovery";

struct ExpectedPartition {
	std::string_view name;
	std::uint64_t offset;
	std::uint64_t size;
};

constexpr std::array expectedPartitions {
	ExpectedPartition{"system", 1'048'576ULL, 1'073'741'824ULL},
	ExpectedPartition{"system_ext", 1'074'790'400ULL, 268'435'456ULL},
	ExpectedPartition{"product", 1'343'225'856ULL, 268'435'456ULL},
	ExpectedPartition{"vendor", 1'611'661'312ULL, 268'435'456ULL},
	ExpectedPartition{"odm", 1'880'096'768ULL, 16'777'216ULL},
	ExpectedPartition{"metadata", 1'896'873'984ULL, 67'108'864ULL},
	ExpectedPartition{"init_boot", 1'963'982'848ULL, 8'388'608ULL},
	ExpectedPartition{"misc", 1'972'371'456ULL, 4'194'304ULL},
	ExpectedPartition{"recovery", 1'976'565'760ULL, 268'435'456ULL},
	ExpectedPartition{"userdata", 2'245'001'216ULL, 6'343'884'800ULL},
};

constexpr std::size_t initBootPartition = 6;
constexpr std::size_t miscPartition = 7;
constexpr std::size_t recoveryPartition = 8;

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

std::uint32_t big32(const std::byte *input)
{
	return (std::to_integer<std::uint32_t>(input[0]) << 24U) |
		(std::to_integer<std::uint32_t>(input[1]) << 16U) |
		(std::to_integer<std::uint32_t>(input[2]) << 8U) |
		std::to_integer<std::uint32_t>(input[3]);
}

std::uint64_t big64(const std::byte *input)
{
	return (static_cast<std::uint64_t>(big32(input)) << 32U) |
		static_cast<std::uint64_t>(big32(input + 4));
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

void validateGpt(const File &disk)
{
	if (disk.size() != diskSize)
		throw std::invalid_argument("Android base disk has the wrong size");
	std::array<std::byte, sectorSize> header {};
	disk.read(sectorSize, header);
	if (std::memcmp(header.data(), "EFI PART", 8) != 0 ||
	    little32(header.data() + 8) != 0x00010000U ||
	    little32(header.data() + 12) != 92U ||
	    little64(header.data() + 24) != 1U ||
	    little64(header.data() + 32) != diskSize / sectorSize - 1U ||
	    little64(header.data() + 72) != 2U ||
	    little32(header.data() + 80) != 128U ||
	    little32(header.data() + 84) != 128U)
		throw std::invalid_argument("Android base disk has an invalid GPT header");
	const auto expectedHeaderCrc = little32(header.data() + 16);
	std::fill(header.begin() + 16, header.begin() + 20, std::byte{});
	if (crc32(std::span{header}.first(92)) != expectedHeaderCrc)
		throw std::invalid_argument("Android base disk GPT header CRC differs");

	std::vector<std::byte> entries(128U * 128U);
	disk.read(2U * sectorSize, entries);
	if (crc32(entries) != little32(header.data() + 88))
		throw std::invalid_argument("Android base disk GPT entry CRC differs");
	for (std::size_t index = 0; index < expectedPartitions.size(); ++index)
		validatePartitionEntry(entries.data() + index * 128U,
				       expectedPartitions[index]);
	for (std::size_t index = expectedPartitions.size(); index < 128U; ++index)
		if (!std::all_of(entries.begin() + index * 128U,
				 entries.begin() + (index + 1U) * 128U,
				 [](std::byte value) { return value == std::byte{}; }))
			throw std::invalid_argument("Android base disk has extra partitions");
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

std::uint64_t roundUp(std::uint64_t value, std::uint64_t alignment)
{
	if (alignment == 0 || value >
		std::numeric_limits<std::uint64_t>::max() - (alignment - 1U))
		throw std::invalid_argument("COW alignment overflows");
	return (value + alignment - 1U) / alignment * alignment;
}

class CowReader final {
public:
	CowReader(const std::filesystem::path &path, const File &base)
		: cow_(path), base_(base)
	{
		std::array<std::byte, cowHeaderSize> header {};
		cow_.read(0, header);
		if (big32(header.data()) != 0x4f4f4f4dU ||
		    big32(header.data() + 4) != cowVersion ||
		    big64(header.data() + 12) != base.size() ||
		    big32(header.data() + 20) != cowSectorSize ||
		    big32(header.data() + 24) != cowAlignment ||
		    little32(header.data() + 28) != 0U ||
		    header[32] == std::byte{})
			throw std::invalid_argument("Android instance has an unsupported COW header");
		const auto pathEnd = std::find(header.begin() + 32, header.end(), std::byte{});
		if (pathEnd == header.end())
			throw std::invalid_argument("Android instance COW path is not terminated");
		bitmapOffset_ = roundUp(cowHeaderSize, cowAlignment);
		const auto sectors = (base.size() + cowSectorSize - 1U) / cowSectorSize;
		bitmapSize_ = (sectors + 7U) / 8U;
		dataOffset_ = roundUp(bitmapOffset_ + bitmapSize_, cowAlignment);
		if (cow_.size() != dataOffset_ + base.size())
			throw std::invalid_argument("Android instance COW extent differs");
	}

	void read(std::uint64_t offset, std::span<std::byte> output) const
	{
		if (offset % cowSectorSize != 0 || output.size() % cowSectorSize != 0)
			throw std::invalid_argument("COW read is not sector-aligned");
		for (std::size_t completed = 0; completed < output.size();
		     completed += cowSectorSize) {
			const auto logicalOffset = offset + completed;
			const auto sector = logicalOffset / cowSectorSize;
			std::array<std::byte, 1> bitmap {};
			cow_.read(bitmapOffset_ + sector / 8U, bitmap);
			const bool present =
				(std::to_integer<unsigned int>(bitmap[0]) &
				 (1U << (sector % 8U))) != 0;
			auto destination = output.subspan(completed, cowSectorSize);
			if (present)
				cow_.read(dataOffset_ + logicalOffset, destination);
			else
				base_.read(logicalOffset, destination);
		}
	}

private:
	File cow_;
	const File &base_;
	std::uint64_t bitmapOffset_ = 0;
	std::uint64_t bitmapSize_ = 0;
	std::uint64_t dataOffset_ = 0;
};

bool recoveryRequested(const std::filesystem::path &instance, const File &base)
{
	struct stat details {};
	if (lstat(instance.c_str(), &details) != 0) {
		if (errno == ENOENT)
			return false;
		throw std::system_error(errno, std::generic_category(),
					"cannot inspect Android instance");
	}
	if (!S_ISREG(details.st_mode))
		throw std::invalid_argument("Android instance is not a regular file");
	const CowReader cow(instance, base);
	std::array<std::byte, 2048> bootloaderMessage {};
	cow.read(expectedPartitions[miscPartition].offset, bootloaderMessage);
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

BootSelection selectBoot(const std::filesystem::path &baseDisk,
			 const std::filesystem::path &instance,
			 BootMode requestedMode)
{
	if (requestedMode != BootMode::automatic &&
	    requestedMode != BootMode::normal && requestedMode != BootMode::recovery)
		throw std::invalid_argument("unknown Android boot mode");
	const File disk(baseDisk);
	validateGpt(disk);
	const auto normal = bootRamdisk(disk, expectedPartitions[initBootPartition]);
	const auto recovery = bootRamdisk(disk, expectedPartitions[recoveryPartition]);
	const auto selected = requestedMode == BootMode::automatic
		? (recoveryRequested(instance, disk) ? BootMode::recovery : BootMode::normal)
		: requestedMode;
	return BootSelection{selected,
			     selected == BootMode::recovery ? recovery : normal};
}

} // namespace uml::android
