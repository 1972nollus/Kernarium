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
inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

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

} // namespace arm64_base_mte_test
