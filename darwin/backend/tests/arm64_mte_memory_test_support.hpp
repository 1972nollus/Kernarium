// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_load_store_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace mte_memory_test {

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;
namespace test = arm64_load_store_test;

constexpr std::uint64_t pc = 0x7400'0000ULL;
constexpr std::uint64_t address_mask = 0x00ff'ffff'ffff'ffffULL;
constexpr std::uint64_t granule_mask = 0x00ff'ffff'ffff'fff0ULL;

enum class Mode : unsigned {
	post_index = 1,
	signed_offset = 2,
	pre_index = 3,
};

enum class Operation : unsigned {
	store_tag = 0,
	store_zero_tag = 1,
	store_two_tags = 2,
	store_zero_two_tags = 3,
};

[[nodiscard]] constexpr std::uint32_t tag_memory(Operation operation,
	Mode mode, unsigned imm9, unsigned base, unsigned transfer) noexcept
{
	return 0xd920'0000U |
		(static_cast<unsigned>(operation) << 22) |
		((imm9 & 0x1ffU) << 12) |
		(static_cast<unsigned>(mode) << 10) |
		((base & 31U) << 5) | (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t load_tag(
	unsigned imm9, unsigned base, unsigned destination) noexcept
{
	return 0xd960'0000U | ((imm9 & 0x1ffU) << 12) |
		((base & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t store_pair_tag(Mode mode,
	unsigned imm7, unsigned base, unsigned first, unsigned second) noexcept
{
	return 0x6800'0000U | (static_cast<unsigned>(mode) << 23) |
		((imm7 & 0x7fU) << 15) | ((second & 31U) << 10) |
		((base & 31U) << 5) | (first & 31U);
}

[[nodiscard]] constexpr std::uint64_t offset9(unsigned imm9) noexcept
{
	return static_cast<std::uint64_t>(test::signed_imm9(imm9) * 16);
}

[[nodiscard]] constexpr std::uint64_t offset7(unsigned imm7) noexcept
{
	return static_cast<std::uint64_t>(test::signed_imm7(imm7) * 16);
}

class Memory final : public arm64::GuestMemory {
public:
	static constexpr std::uint64_t base = 0x0010'0000ULL;
	static constexpr std::size_t length = 4 * 4096;
	static constexpr std::uint64_t no_fault =
		std::numeric_limits<std::uint64_t>::max();

	Memory()
	{
		for (std::size_t index = 0; index < bytes.size(); ++index)
			bytes[index] = static_cast<std::byte>((index * 37U + 11U) & 0xffU);
	}

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> output) override
	{
		++reads;
		last_data_address = address;
		last_data_size = output.size();
		if (!contains(address, output.size()) || overlaps_fault(
			address, output.size(), fault_read))
			return false;
		std::copy_n(at(address), output.size(), output.begin());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> input) override
	{
		++writes;
		last_data_address = address;
		last_data_size = input.size();
		if (!contains(address, input.size()) || overlaps_fault(
			address, input.size(), fault_write))
			return false;
		std::copy(input.begin(), input.end(), at(address));
		return true;
	}

	[[nodiscard]] bool read_allocation_tag(
		std::uint64_t address, std::uint8_t &tag) override
	{
		++tag_reads;
		last_tag_address = address & granule_mask;
		if (!contains(last_tag_address, 1) ||
		    last_tag_address == fault_tag_read)
			return false;
		tag = tags[tag_index(last_tag_address)];
		return true;
	}

	[[nodiscard]] bool write_allocation_tag(
		std::uint64_t address, std::uint8_t tag) override
	{
		++tag_writes;
		last_tag_address = address & granule_mask;
		if (!contains(last_tag_address, 1) ||
		    last_tag_address == fault_tag_write)
			return false;
		tags[tag_index(last_tag_address)] = tag & 15U;
		return true;
	}

	[[nodiscard]] bool contains(
		std::uint64_t address, std::size_t size = 1) const noexcept
	{
		return address >= base && address - base <= bytes.size() &&
			size <= bytes.size() - static_cast<std::size_t>(address - base);
	}

	[[nodiscard]] std::byte *at(std::uint64_t address)
	{
		test::require(contains(address));
		return bytes.data() + static_cast<std::size_t>(address - base);
	}

	[[nodiscard]] const std::byte *at(std::uint64_t address) const
	{
		test::require(contains(address));
		return bytes.data() + static_cast<std::size_t>(address - base);
	}

	void set_tag(std::uint64_t address, std::uint8_t tag)
	{
		address &= granule_mask;
		test::require(contains(address));
		tags[tag_index(address)] = tag & 15U;
	}

	[[nodiscard]] std::uint8_t tag(std::uint64_t address) const
	{
		address &= granule_mask;
		test::require(contains(address));
		return tags[tag_index(address)];
	}

	void reset_counts() noexcept
	{
		reads = 0;
		writes = 0;
		tag_reads = 0;
		tag_writes = 0;
		last_data_address = 0;
		last_data_size = 0;
		last_tag_address = 0;
	}

	std::array<std::byte, length> bytes{};
	std::array<std::uint8_t, length / 16> tags{};
	std::uint64_t fault_read{no_fault};
	std::uint64_t fault_write{no_fault};
	std::uint64_t fault_tag_read{no_fault};
	std::uint64_t fault_tag_write{no_fault};
	std::uint64_t last_data_address{};
	std::size_t last_data_size{};
	std::uint64_t last_tag_address{};
	std::size_t reads{};
	std::size_t writes{};
	std::size_t tag_reads{};
	std::size_t tag_writes{};

private:
	[[nodiscard]] static bool overlaps_fault(std::uint64_t address,
		std::size_t size, std::uint64_t fault) noexcept
	{
		return fault != no_fault && address <= fault &&
			fault - address < size;
	}

	[[nodiscard]] static constexpr std::size_t tag_index(
		std::uint64_t address) noexcept
	{
		return static_cast<std::size_t>((address - base) / 16);
	}
};

[[nodiscard]] inline arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			(static_cast<std::uint64_t>((index * 7U + 3U) & 15U) << 56) |
			(0x0000'1234'5000'0000ULL + index * 0x10101ULL);
	cpu.sp = Memory::base + 2 * 4096;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.set_mte_configuration(true, 0x8001U);
	cpu.set_mte_generator(false, 0xace1U, 7, 0xfeed'face'0123'4567ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0xa1b2'c3d4U;
	cpu.exclusive_address = 0x7654'3000ULL;
	cpu.exclusive_key = 0x1234U;
	cpu.exclusive_value = 0xa5a5'a5a5'a5a5'a5a5ULL;
	cpu.exclusive_value2 = 0x5a5a'5a5a'5a5a'5a5aULL;
	cpu.exclusive_version = 19;
	cpu.exclusive_size = 16;
	return cpu;
}

inline void set_register(arm64::CpuState &cpu, unsigned index,
	std::uint64_t value, bool stack_pointer)
{
	if (index == 31) {
		test::require(stack_pointer);
		cpu.sp = value;
	} else {
		cpu.registers[index] = value;
	}
}

[[nodiscard]] inline std::uint64_t get_register(
	const arm64::CpuState &cpu, unsigned index, bool stack_pointer)
{
	return index == 31 ? stack_pointer ? cpu.sp : 0 : cpu.registers[index];
}

inline void require_state(const arm64::CpuState &actual,
	const arm64::CpuState &expected, std::uint64_t expected_pc)
{
	test::require(actual.registers == expected.registers &&
		actual.sp == expected.sp && actual.pc == expected_pc &&
		actual.nzcv == expected.nzcv && actual.fpcr == expected.fpcr &&
		actual.fpsr == expected.fpsr && actual.tls == expected.tls &&
		actual.mte_enabled == expected.mte_enabled &&
		actual.mte_exclude == expected.mte_exclude &&
		actual.mte_tag_choices == expected.mte_tag_choices &&
		actual.mte_random_tags == expected.mte_random_tags &&
		actual.mte_seed == expected.mte_seed &&
		actual.mte_tag == expected.mte_tag &&
		actual.mte_random_state == expected.mte_random_state &&
		actual.cpa_enabled == expected.cpa_enabled &&
		actual.cpa_multiply_enabled == expected.cpa_multiply_enabled &&
		actual.accdata == expected.accdata &&
		actual.exclusive_address == expected.exclusive_address &&
		actual.exclusive_key == expected.exclusive_key &&
		actual.exclusive_value == expected.exclusive_value &&
		actual.exclusive_value2 == expected.exclusive_value2 &&
		actual.exclusive_version == expected.exclusive_version &&
		actual.exclusive_size == expected.exclusive_size &&
		actual.deferred_vectors == expected.deferred_vectors &&
		actual.deferred_fpcr == expected.deferred_fpcr &&
		actual.deferred_fpsr == expected.deferred_fpsr &&
		actual.fp_loaded == expected.fp_loaded &&
		actual.fp_dirty == expected.fp_dirty);
	for (unsigned index = 0; index < actual.vectors.size(); ++index)
		test::require(actual.vectors[index].bytes ==
			expected.vectors[index].bytes);
}

inline void require_continued(const arm64::RunResult &result,
	std::uint32_t instruction)
{
	test::require(result.reason == arm64::StopReason::continued &&
		result.address == pc && result.instruction == instruction &&
		result.instructions == 1 && !result.write);
}

inline void static_store_tag(unsigned long generation,
	std::uint64_t address, std::uint8_t tag)
{
	test::require(darwin_uml_static_vm_store_allocation_tag(
		generation, address, tag) == DARWIN_UML_STATIC_TRANSLATED);
}

[[nodiscard]] inline std::uint8_t static_load_tag(
	unsigned long generation, std::uint64_t address)
{
	std::uint8_t tag = 0xff;
	test::require(darwin_uml_static_vm_load_allocation_tag(
		generation, address, &tag) == DARWIN_UML_STATIC_TRANSLATED);
	return tag;
}

} // namespace mte_memory_test
