// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace arm64_simd_lut_test {

namespace common = arm64_simd_test;
namespace arm64 = uml::backend::arm64;

struct Variant final {
	unsigned op2;
	unsigned len;
	unsigned op;
	unsigned index_bits;
	unsigned element_bytes;
	unsigned part;
	unsigned table_count;
};

inline constexpr std::array variants{
	Variant{2, 0, 1, 2, 1, 0, 1}, Variant{2, 1, 1, 2, 1, 1, 1},
	Variant{2, 2, 1, 2, 1, 2, 1}, Variant{2, 3, 1, 2, 1, 3, 1},
	Variant{3, 0, 0, 2, 2, 0, 1}, Variant{3, 0, 1, 2, 2, 1, 1},
	Variant{3, 1, 0, 2, 2, 2, 1}, Variant{3, 1, 1, 2, 2, 3, 1},
	Variant{3, 2, 0, 2, 2, 4, 1}, Variant{3, 2, 1, 2, 2, 5, 1},
	Variant{3, 3, 0, 2, 2, 6, 1}, Variant{3, 3, 1, 2, 2, 7, 1},
	Variant{1, 1, 0, 4, 1, 0, 1}, Variant{1, 3, 0, 4, 1, 1, 1},
	Variant{1, 0, 1, 4, 2, 0, 2}, Variant{1, 1, 1, 4, 2, 1, 2},
	Variant{1, 2, 1, 4, 2, 2, 2}, Variant{1, 3, 1, 4, 2, 3, 2},
};

static_assert(variants.size() == 18);

struct Registers final {
	unsigned destination;
	unsigned table;
	unsigned indices;
};

inline constexpr std::array register_cases{
	Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
	Registers{1, 1, 1}, Registers{31, 31, 0}, Registers{0, 31, 0},
	Registers{30, 31, 30}, Registers{31, 30, 31},
};

[[nodiscard]] constexpr std::uint32_t instruction(const Variant &variant,
	Registers registers = {0, 1, 2}) noexcept
{
	return 0x4e00'0000U | ((variant.op2 & 3U) << 22) |
		((registers.indices & 31U) << 16) |
		((variant.len & 3U) << 13) | ((variant.op & 1U) << 12) |
		((registers.table & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t instruction(unsigned op2,
	unsigned len, unsigned op, Registers registers = {0, 1, 2}) noexcept
{
	return 0x4e00'0000U | ((op2 & 3U) << 22) |
		((registers.indices & 31U) << 16) | ((len & 3U) << 13) |
		((op & 1U) << 12) | ((registers.table & 31U) << 5) |
		(registers.destination & 31U);
}

inline void set_selected_index(arm64::CpuState &cpu, const Variant &variant,
	unsigned indices_register, unsigned element, unsigned value)
{
	const unsigned elements = 16U / variant.element_bytes;
	const unsigned bit =
		(elements * variant.part + element) * variant.index_bits;
	const unsigned mask = (1U << variant.index_bits) - 1U;
	auto &byte = cpu.vectors[indices_register].bytes[bit / 8U];
	const unsigned shift = bit & 7U;
	const unsigned old = std::to_integer<unsigned>(byte);
	byte = static_cast<std::byte>((old & ~(mask << shift)) |
		((value & mask) << shift));
}

inline void set_selected_indices(arm64::CpuState &cpu, const Variant &variant,
	Registers registers, unsigned rotation)
{
	const unsigned elements = 16U / variant.element_bytes;
	const unsigned mask = (1U << variant.index_bits) - 1U;
	for (unsigned element = 0; element < elements; ++element)
		set_selected_index(cpu, variant, registers.indices, element,
			(element + rotation) & mask);
}

[[nodiscard]] inline arm64::VectorRegister expected_result(
	const arm64::CpuState &initial, const Variant &variant,
	Registers registers)
{
	const auto indices = initial.vectors[registers.indices].bytes;
	const auto table1 = initial.vectors[registers.table].bytes;
	const auto table2 =
		initial.vectors[(registers.table + 1U) & 31U].bytes;
	arm64::VectorRegister result{};
	const unsigned elements = 16U / variant.element_bytes;
	const unsigned mask = (1U << variant.index_bits) - 1U;
	const unsigned table_elements = 16U / variant.element_bytes;
	for (unsigned element = 0; element < elements; ++element) {
		const unsigned bit =
			(elements * variant.part + element) * variant.index_bits;
		const unsigned index =
			(std::to_integer<unsigned>(indices[bit / 8U]) >> (bit & 7U)) &
			mask;
		const bool second = index >= table_elements;
		common::require(!second || variant.table_count == 2U);
		const auto &table = second ? table2 : table1;
		const unsigned table_index = second ? index - table_elements : index;
		std::copy_n(table.begin() + table_index * variant.element_bytes,
			variant.element_bytes,
			result.bytes.begin() + element * variant.element_bytes);
	}
	return result;
}

inline void require_only_destination_changed(const arm64::CpuState &before,
	const arm64::CpuState &after, unsigned destination)
{
	common::require(after.registers == before.registers);
	common::require(after.sp == before.sp);
	common::require(after.nzcv == before.nzcv);
	common::require(after.fpcr == before.fpcr);
	common::require(after.fpsr == before.fpsr);
	common::require(after.tls == before.tls);
	for (unsigned reg = 0; reg < before.vectors.size(); ++reg)
		if (reg != destination)
			common::require(after.vectors[reg].bytes == before.vectors[reg].bytes);
}

} // namespace arm64_simd_lut_test
