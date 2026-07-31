// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_system_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uml::backend::tests::simd_lut {

namespace arm64 = uml::backend::arm64;

inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

struct Form final {
	unsigned op2{};
	unsigned len{};
	unsigned op{};
	unsigned index_bits{};
	unsigned element_bytes{};
	unsigned part{};
	unsigned table_count{};
};

struct Decoded final {
	bool recognized{};
	bool valid{};
	Form form{};
};

inline constexpr std::array forms{
	Form{1U, 1U, 0U, 4U, 1U, 0U, 1U},
	Form{1U, 3U, 0U, 4U, 1U, 1U, 1U},
	Form{1U, 0U, 1U, 4U, 2U, 0U, 2U},
	Form{1U, 1U, 1U, 4U, 2U, 1U, 2U},
	Form{1U, 2U, 1U, 4U, 2U, 2U, 2U},
	Form{1U, 3U, 1U, 4U, 2U, 3U, 2U},
	Form{2U, 0U, 1U, 2U, 1U, 0U, 1U},
	Form{2U, 1U, 1U, 2U, 1U, 1U, 1U},
	Form{2U, 2U, 1U, 2U, 1U, 2U, 1U},
	Form{2U, 3U, 1U, 2U, 1U, 3U, 1U},
	Form{3U, 0U, 0U, 2U, 2U, 0U, 1U},
	Form{3U, 0U, 1U, 2U, 2U, 1U, 1U},
	Form{3U, 1U, 0U, 2U, 2U, 2U, 1U},
	Form{3U, 1U, 1U, 2U, 2U, 3U, 1U},
	Form{3U, 2U, 0U, 2U, 2U, 4U, 1U},
	Form{3U, 2U, 1U, 2U, 2U, 5U, 1U},
	Form{3U, 3U, 0U, 2U, 2U, 6U, 1U},
	Form{3U, 3U, 1U, 2U, 2U, 7U, 1U},
};

[[nodiscard]] constexpr std::uint32_t encode(
	unsigned op2, unsigned len, unsigned op, unsigned indices,
	unsigned table, unsigned destination) noexcept
{
	return 0x4e00'0000U | ((op2 & 3U) << 22U) |
		((indices & 31U) << 16U) | ((len & 3U) << 13U) |
		((op & 1U) << 12U) | ((table & 31U) << 5U) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode(
	const Form &form, unsigned indices, unsigned table,
	unsigned destination) noexcept
{
	return encode(form.op2, form.len, form.op, indices, table,
		destination);
}

[[nodiscard]] constexpr Decoded decode(std::uint32_t instruction) noexcept
{
	if ((instruction & 0xff20'8c00U) != 0x4e00'0000U ||
	    (instruction & 0x00c0'0000U) == 0U)
		return {};
	const unsigned op2 = (instruction >> 22U) & 3U;
	const unsigned len = (instruction >> 13U) & 3U;
	const unsigned op = (instruction >> 12U) & 1U;
	Form form{
		.op2 = op2,
		.len = len,
		.op = op,
	};
	if (op2 == 1U) {
		if (op == 0U && (len & 1U) == 0U)
			return {.recognized = true};
		form.index_bits = 4U;
		form.element_bytes = op == 0U ? 1U : 2U;
		form.part = op == 0U ? len >> 1U : len;
		form.table_count = op == 0U ? 1U : 2U;
	} else if (op2 == 2U) {
		if (op == 0U)
			return {.recognized = true};
		form.index_bits = 2U;
		form.element_bytes = 1U;
		form.part = len;
		form.table_count = 1U;
	} else {
		form.index_bits = 2U;
		form.element_bytes = 2U;
		form.part = (len << 1U) | op;
		form.table_count = 1U;
	}
	return {
		.recognized = true,
		.valid = true,
		.form = form,
	};
}

[[nodiscard]] inline NativeRequirements native_requirements() noexcept
{
	return NativeRequirements::features(
		{ArmFeature::advanced_simd, ArmFeature::lut});
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	auto cpu = arm64_system_test::state();
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size();
		     ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 0x35U + byte * 0x79U +
				(reg ^ byte) * 0x13U);
	cpu.registers[0] ^= seed * 0x9e37'79b9U;
	cpu.registers[30] += seed;
	return cpu;
}

inline void write_index(arm64::VectorRegister &indices, const Form &form,
	unsigned element, unsigned value) noexcept
{
	const unsigned elements = 16U / form.element_bytes;
	const unsigned bit =
		(elements * form.part + element) * form.index_bits;
	const unsigned mask = (1U << form.index_bits) - 1U;
	auto byte = std::to_integer<unsigned>(indices.bytes[bit / 8U]);
	byte &= ~(mask << (bit & 7U));
	byte |= (value & mask) << (bit & 7U);
	indices.bytes[bit / 8U] = static_cast<std::byte>(byte);
}

[[nodiscard]] inline arm64::CpuState expected(
	const arm64::CpuState &initial, std::uint32_t instruction)
{
	const auto decoded = decode(instruction);
	arm64_system_test::require(decoded.valid);
	const auto &form = decoded.form;
	const unsigned destination = instruction & 31U;
	const unsigned table = (instruction >> 5U) & 31U;
	const unsigned indices_register = (instruction >> 16U) & 31U;
	const auto indices = initial.vectors[indices_register].bytes;
	const auto table1 = initial.vectors[table].bytes;
	const auto table2 = initial.vectors[(table + 1U) & 31U].bytes;
	std::array<std::byte, 16> result{};
	const unsigned elements = 16U / form.element_bytes;
	const unsigned first_index = elements * form.part;
	const unsigned index_mask = (1U << form.index_bits) - 1U;
	for (unsigned element = 0; element < elements; ++element) {
		const unsigned bit =
			(first_index + element) * form.index_bits;
		const unsigned index =
			(std::to_integer<unsigned>(indices[bit / 8U]) >>
				(bit & 7U)) &
			index_mask;
		const bool second = index >= elements;
		arm64_system_test::require(
			!second || form.table_count == 2U);
		const auto &source = second ? table2 : table1;
		const unsigned source_element =
			second ? index - elements : index;
		std::copy_n(
			source.begin() + source_element * form.element_bytes,
			form.element_bytes,
			result.begin() + element * form.element_bytes);
	}
	auto result_state = initial;
	result_state.vectors[destination].bytes = result;
	return result_state;
}

inline void check(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	arm64_system_test::require(decode(instruction).valid);
	harness.check_one(name, instruction, initial,
		expected(initial, instruction), native_requirements());
}

inline void check_reserved(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	const auto decoded = decode(instruction);
	arm64_system_test::require(decoded.recognized && !decoded.valid);
	harness.check_illegal(name, instruction, initial);
}

} // namespace uml::backend::tests::simd_lut
