// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "uml_darwin_guest.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <span>
#include <string_view>

namespace arm64_base_mte_test {

namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace test = uml::emulator::tests;

inline constexpr std::uint64_t data_base = test::engine_test_data_base;
inline constexpr std::uint64_t address_mask = 0x00ff'ffff'ffff'ffffULL;
inline constexpr std::uint64_t granule_mask = 0x00ff'ffff'ffff'fff0ULL;
inline constexpr std::uint64_t logical_tag_mask = 0x0f00'0000'0000'0000ULL;
inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline const auto native_mte =
	test::NativeRequirements::features(::uml::cpu::ArmFeature::mte);

using Page = std::array<std::byte, page_size>;

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

class CaseName final {
public:
	explicit CaseName(const char *text)
	{
		const int length = std::snprintf(
			storage_.data(), storage_.size(), "%s", text);
		require(length >= 0 &&
			static_cast<std::size_t>(length) < storage_.size());
		length_ = static_cast<std::size_t>(length);
	}

	template <typename... Arguments>
	explicit CaseName(const char *format, Arguments... arguments)
	{
		const int length = std::snprintf(
			storage_.data(), storage_.size(), format, arguments...);
		require(length >= 0 &&
			static_cast<std::size_t>(length) < storage_.size());
		length_ = static_cast<std::size_t>(length);
	}

	[[nodiscard]] operator std::string_view() const noexcept
	{
		return {storage_.data(), length_};
	}

private:
	std::array<char, 160> storage_{};
	std::size_t length_{};
};

enum class TagMemoryOperation : unsigned {
	store_tag = 0,
	store_zero_tag = 1,
	store_two_tags = 2,
	store_zero_two_tags = 3,
};

enum class AddressMode : unsigned {
	post_index = 1,
	signed_offset = 2,
	pre_index = 3,
};

[[nodiscard]] constexpr std::uint32_t add_sub_tag(bool subtract,
	unsigned destination, unsigned source, unsigned immediate,
	unsigned tag_offset) noexcept
{
	return (subtract ? 0xd180'0000U : 0x9180'0000U) |
		((immediate & 63U) << 16) | ((tag_offset & 15U) << 10) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t gmi(
	unsigned destination, unsigned address, unsigned mask) noexcept
{
	return 0x9ac0'1400U | ((mask & 31U) << 16) |
		((address & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t irg(
	unsigned destination, unsigned address, unsigned exclude) noexcept
{
	return 0x9ac0'1000U | ((exclude & 31U) << 16) |
		((address & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t subp(bool flags,
	unsigned destination, unsigned first, unsigned second) noexcept
{
	return (flags ? 0xbac0'0000U : 0x9ac0'0000U) |
		((second & 31U) << 16) | ((first & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t tag_memory(
	TagMemoryOperation operation, AddressMode mode, unsigned imm9,
	unsigned base, unsigned transfer) noexcept
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

[[nodiscard]] constexpr std::uint32_t store_pair_tag(AddressMode mode,
	unsigned imm7, unsigned base, unsigned first, unsigned second) noexcept
{
	return 0x6800'0000U | (static_cast<unsigned>(mode) << 23) |
		((imm7 & 0x7fU) << 15) | ((second & 31U) << 10) |
		((base & 31U) << 5) | (first & 31U);
}

[[nodiscard]] constexpr std::int64_t sign_extend(
	std::uint64_t value, unsigned bits) noexcept
{
	const auto sign = std::uint64_t{1} << (bits - 1);
	const auto mask = (std::uint64_t{1} << bits) - 1;
	value &= mask;
	return static_cast<std::int64_t>((value ^ sign) - sign);
}

[[nodiscard]] constexpr std::uint64_t offset9(unsigned imm9) noexcept
{
	return static_cast<std::uint64_t>(sign_extend(imm9, 9) * 16);
}

[[nodiscard]] constexpr std::uint64_t offset7(unsigned imm7) noexcept
{
	return static_cast<std::uint64_t>(sign_extend(imm7, 7) * 16);
}

[[nodiscard]] constexpr std::uint64_t sign_extend_56(
	std::uint64_t value) noexcept
{
	return static_cast<std::uint64_t>(sign_extend(value, 56));
}

[[nodiscard]] constexpr unsigned logical_tag(std::uint64_t value) noexcept
{
	return static_cast<unsigned>(value >> 56) & 15U;
}

[[nodiscard]] constexpr std::uint64_t with_allocation_tag(
	std::uint64_t address, unsigned tag) noexcept
{
	return (address & ~logical_tag_mask) |
		(static_cast<std::uint64_t>(tag & 15U) << 56);
}

[[nodiscard]] inline arm64::CpuState initial_state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index) {
		cpu.registers[index] =
			(static_cast<std::uint64_t>((index * 7U + 3U) & 15U) << 56) |
			(0x0000'1234'5000'0000ULL +
			 static_cast<std::uint64_t>(index) * 0x10101ULL);
	}
	cpu.sp = data_base + page_size / 2;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.fpmr = 0x1020'3040'5060'7080ULL;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.set_mte_configuration(true, 0x8001U);
	cpu.set_mte_generator(
		false, 0xace1U, 7, 0xfeed'face'0123'4567ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0xa1b2'c3d4U;
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xa;
	cpu.exclusive_address = 0x7654'3000ULL;
	cpu.exclusive_key = 0x1234U;
	cpu.exclusive_value = 0xa5a5'a5a5'a5a5'a5a5ULL;
	cpu.exclusive_value2 = 0x5a5a'5a5a'5a5a'5a5aULL;
	cpu.exclusive_version = 19;
	cpu.exclusive_size = 16;
	for (unsigned vector = 0; vector < cpu.vectors.size(); ++vector)
		for (unsigned byte = 0;
		     byte < cpu.vectors[vector].bytes.size(); ++byte)
			cpu.vectors[vector].bytes[byte] = static_cast<std::byte>(
				(vector * 29U + byte * 17U + 3U) & 0xffU);
	return cpu;
}

[[nodiscard]] constexpr std::uint64_t read_register(
	const arm64::CpuState &cpu, unsigned index, bool stack_pointer) noexcept
{
	return index == 31 ? stack_pointer ? cpu.sp : 0 :
		cpu.registers[index];
}

inline void write_register(arm64::CpuState &cpu, unsigned index,
	std::uint64_t value, bool stack_pointer) noexcept
{
	if (index == 31) {
		if (stack_pointer)
			cpu.sp = value;
	} else {
		cpu.registers[index] = value;
	}
}

[[nodiscard]] constexpr std::uint8_t choose_nonexcluded_tag_or_zero(
	bool enabled, std::uint16_t exclude, unsigned start,
	unsigned tag_offset) noexcept
{
	if (!enabled)
		return 0;
	unsigned tag = start & 15U;
	unsigned offset = tag_offset & 15U;
	if (exclude == 0xffffU)
		return 0;
	if (offset == 0) {
		while ((exclude & (1U << tag)) != 0)
			tag = (tag + 1U) & 15U;
		return static_cast<std::uint8_t>(tag);
	}
	do {
		--offset;
		tag = (tag + 1U) & 15U;
		while ((exclude & (1U << tag)) != 0)
			tag = (tag + 1U) & 15U;
	} while (offset != 0);
	return static_cast<std::uint8_t>(tag);
}

[[nodiscard]] inline arm64::CpuState expected_add_sub_tag(
	const arm64::CpuState &initial, bool subtract, unsigned destination,
	unsigned source_register, unsigned immediate, unsigned tag_offset)
{
	auto expected = initial;
	const auto source = read_register(initial, source_register, true);
	const auto offset = static_cast<std::uint64_t>(immediate & 63U) << 4;
	const auto arithmetic = subtract ? source - offset : source + offset;
	const auto tag = choose_nonexcluded_tag_or_zero(
		initial.mte_enabled, initial.mte_exclude, logical_tag(source),
		tag_offset);
	write_register(expected, destination,
		with_allocation_tag(arithmetic, tag), true);
	return expected;
}

[[nodiscard]] constexpr std::uint32_t subtraction_flags(
	std::uint64_t first, std::uint64_t second) noexcept
{
	const auto result = first - second;
	const bool overflow = ((first ^ second) & (first ^ result) &
		(std::uint64_t{1} << 63)) != 0;
	return ((result >> 63) != 0 ? 0x8000'0000U : 0U) |
		(result == 0 ? 0x4000'0000U : 0U) |
		(first >= second ? 0x2000'0000U : 0U) |
		(overflow ? 0x1000'0000U : 0U);
}

[[nodiscard]] inline arm64::CpuState expected_gmi(
	const arm64::CpuState &initial, unsigned destination, unsigned address,
	unsigned mask_register)
{
	auto expected = initial;
	const auto tagged_address = read_register(initial, address, true);
	const auto mask = read_register(initial, mask_register, false);
	write_register(expected, destination,
		mask | (std::uint64_t{1} << logical_tag(tagged_address)), false);
	return expected;
}

[[nodiscard]] inline arm64::CpuState expected_subp(
	const arm64::CpuState &initial, bool flags, unsigned destination,
	unsigned first_register, unsigned second_register)
{
	auto expected = initial;
	const auto first = sign_extend_56(
		read_register(initial, first_register, true));
	const auto second = sign_extend_56(
		read_register(initial, second_register, true));
	const auto result = first - second;
	if (flags)
		expected.nzcv = subtraction_flags(first, second);
	write_register(expected, destination, result, false);
	return expected;
}

[[nodiscard]] inline std::uint8_t select_allowed_tag(
	std::uint16_t allowed, unsigned selection)
{
	for (unsigned tag = 0; tag < 16; ++tag) {
		if ((allowed & (1U << tag)) == 0)
			continue;
		if (selection-- == 0)
			return static_cast<std::uint8_t>(tag);
	}
	std::abort();
}

[[nodiscard]] inline std::uint8_t next_irg_tag(
	arm64::CpuState &state, std::uint16_t exclude)
{
	if (!state.mte_enabled || exclude == 0xffffU)
		return 0;
	const auto allowed = static_cast<std::uint16_t>(~exclude);
	const unsigned count = std::popcount(allowed);
	/* Random selection is implementation-defined and is validated through
	 * architectural invariants, never by duplicating the production
	 * generator in this oracle.
	 */
	require(!state.mte_random_tags);
	unsigned generator_tag = static_cast<unsigned>(state.mte_tag) + 1U;
	if (generator_tag >= count)
		generator_tag = 0;
	state.mte_tag = static_cast<std::uint8_t>(generator_tag);
	const unsigned selection = static_cast<unsigned>(
		(generator_tag + count *
		 static_cast<unsigned>(state.mte_seed)) >> 16) % count;
	const unsigned feedback = ((state.mte_seed >> 5) ^
		(state.mte_seed >> 3) ^ (state.mte_seed >> 2) ^
		state.mte_seed) & 1U;
	state.mte_seed = static_cast<std::uint16_t>(
		(state.mte_seed >> 1) | (feedback << 15));
	return select_allowed_tag(allowed, selection);
}

[[nodiscard]] inline arm64::CpuState expected_irg(
	const arm64::CpuState &initial, unsigned destination, unsigned address,
	unsigned exclude_register)
{
	auto expected = initial;
	const auto source = read_register(initial, address, true);
	const auto register_exclude =
		read_register(initial, exclude_register, false);
	const auto exclude = static_cast<std::uint16_t>(
		initial.mte_exclude | register_exclude);
	const auto tag = next_irg_tag(expected, exclude);
	write_register(expected, destination,
		with_allocation_tag(source, tag), true);
	return expected;
}

inline void fill_pattern(Page &page, unsigned salt = 0)
{
	for (std::size_t index = 0; index < page.size(); ++index)
		page[index] = static_cast<std::byte>(
			(index * 37U + salt * 13U + 11U) & 0xffU);
}

inline void store_u64(Page &page, std::size_t offset, std::uint64_t value)
{
	require(offset <= page.size() - sizeof(value));
	for (unsigned byte = 0; byte < sizeof(value); ++byte)
		page[offset + byte] =
			static_cast<std::byte>(value >> (byte * 8));
}

[[nodiscard]] inline test::MemoryRegion memory_region(
	const Page &initial, const Page &expected,
	std::span<const test::AllocationTag> initial_tags = {},
	std::span<const test::AllocationTag> expected_tags = {},
	bool readable = true, bool writable = true)
{
	return {
		.address = data_base,
		.initial = initial,
		.expected = expected,
		.initial_tags = initial_tags,
		.expected_tags = expected_tags,
		.readable = readable,
		.writable = writable,
	};
}

inline void check_one(test::EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial,
	const arm64::CpuState &expected)
{
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = expected,
		.native = native_mte,
	});
}

} // namespace arm64_base_mte_test
