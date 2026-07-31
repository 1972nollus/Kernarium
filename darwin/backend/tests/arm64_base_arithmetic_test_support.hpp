// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::backend::tests::base_arithmetic {

namespace arm64 = uml::backend::arm64;

enum class ShiftOperation : unsigned {
	left = 0,
	right = 1,
	arithmetic_right = 2,
	rotate_right = 3,
};

[[nodiscard]] constexpr std::uint64_t width_mask(bool wide) noexcept
{
	return wide ? ~std::uint64_t{0} : std::uint64_t{0xffff'ffffU};
}

[[nodiscard]] constexpr std::uint32_t encode_carry(
	bool wide, bool subtract, bool set_flags, unsigned destination,
	unsigned left, unsigned right) noexcept
{
	return 0x1a00'0000U | (wide ? 0x8000'0000U : 0U) |
		(subtract ? 0x4000'0000U : 0U) |
		(set_flags ? 0x2000'0000U : 0U) |
		((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_divide(
	bool wide, bool signed_divide, unsigned destination, unsigned dividend,
	unsigned divisor) noexcept
{
	return 0x1ac0'0800U | (wide ? 0x8000'0000U : 0U) |
		(signed_divide ? 0x400U : 0U) |
		((divisor & 31U) << 16) | ((dividend & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_variable_shift(
	bool wide, ShiftOperation operation, unsigned destination,
	unsigned value, unsigned shift) noexcept
{
	return 0x1ac0'2000U | (wide ? 0x8000'0000U : 0U) |
		(static_cast<unsigned>(operation) << 10) |
		((shift & 31U) << 16) | ((value & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_two_source(
	bool wide, bool s, unsigned opcode, unsigned destination,
	unsigned left, unsigned right) noexcept
{
	return 0x1ac0'0000U | (wide ? 0x8000'0000U : 0U) |
		(s ? 0x2000'0000U : 0U) | ((right & 31U) << 16) |
		((opcode & 63U) << 10) | ((left & 31U) << 5) |
		(destination & 31U);
}

/*
 * Allocation from the 2026-06 Data-processing (2 source) decode table.
 * Feature-gated instructions remain allocated even when unavailable on the
 * running host.
 */
[[nodiscard]] constexpr bool two_source_allocated(
	bool wide, bool s, unsigned opcode) noexcept
{
	if (opcode >= 32U)
		return false;
	if (s)
		return wide && opcode == 0U;
	if (!wide) {
		switch (opcode) {
		case 2:
		case 3:
		case 8:
		case 9:
		case 10:
		case 11:
		case 16:
		case 17:
		case 18:
		case 20:
		case 21:
		case 22:
		case 24:
		case 25:
		case 26:
		case 27:
			return true;
		default:
			return false;
		}
	}
	switch (opcode) {
	case 0:
	case 2:
	case 3:
	case 4:
	case 5:
	case 8:
	case 9:
	case 10:
	case 11:
	case 12:
	case 19:
	case 23:
	case 24:
	case 25:
	case 26:
	case 27:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index) {
		const auto ordinal = static_cast<std::uint64_t>(index + 1U);
		cpu.registers[index] =
			(seed ^ (ordinal * 0x9e37'79b9'7f4a'7c15ULL)) +
			ordinal * 0x0102'0304'0506'0708ULL;
	}
	cpu.sp = seed ^ 0xfedc'ba98'7654'3210ULL;
	cpu.nzcv = static_cast<std::uint32_t>((seed >> 8) & 15U) << 28;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size();
		     ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 31U + byte * 19U);
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.tls = seed ^ 0x0123'4567'89ab'cdefULL;
	cpu.set_mte_configuration(true, 0x5a5aU);
	cpu.set_mte_generator(true, 0x1357U, 11U,
		seed ^ 0x6a09'e667'f3bc'c909ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = static_cast<std::uint32_t>(seed);
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_sm = true;
	cpu.pstate_za = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xbU;
	cpu.exclusive_address = seed ^ 0x8877'6655'4433'2210ULL;
	cpu.exclusive_key = static_cast<std::uintptr_t>(
		seed ^ 0x1020'3040'5060'7080ULL);
	cpu.exclusive_value = seed ^ 0x1111'2222'3333'4444ULL;
	cpu.exclusive_value2 = seed ^ 0x5555'6666'7777'8888ULL;
	cpu.exclusive_version = seed ^ 0x9999'aaaa'bbbb'ccccULL;
	cpu.exclusive_size = 8;
	cpu.fpmr = seed ^ 0x0f1e'2d3c'4b5a'6978ULL;
	return cpu;
}

[[nodiscard]] constexpr std::uint64_t register_value(
	const arm64::CpuState &cpu, unsigned index) noexcept
{
	return index == 31U ? 0U : cpu.registers[index];
}

[[nodiscard]] constexpr __int128 signed_value(
	std::uint64_t value, bool wide) noexcept
{
	const unsigned bits = wide ? 64U : 32U;
	const auto mask = width_mask(wide);
	value &= mask;
	const auto sign = std::uint64_t{1} << (bits - 1U);
	return (value & sign) == 0U ? static_cast<__int128>(value) :
		static_cast<__int128>(value) -
			(static_cast<__int128>(1) << bits);
}

struct CarryResult final {
	std::uint64_t value;
	std::uint32_t nzcv;
};

[[nodiscard]] constexpr CarryResult add_with_carry(
	std::uint64_t left, std::uint64_t right, bool carry, bool subtract,
	bool wide) noexcept
{
	const unsigned bits = wide ? 64U : 32U;
	const auto mask = width_mask(wide);
	left &= mask;
	right &= mask;
	const auto unsigned_sum = static_cast<unsigned __int128>(left) +
		(subtract ? mask ^ right : right) +
		static_cast<unsigned>(carry);
	const auto result = static_cast<std::uint64_t>(unsigned_sum) & mask;
	const auto signed_sum = subtract ?
		signed_value(left, wide) - signed_value(right, wide) -
			static_cast<unsigned>(!carry) :
		signed_value(left, wide) + signed_value(right, wide) +
			static_cast<unsigned>(carry);
	const auto minimum = -(static_cast<__int128>(1) << (bits - 1U));
	const auto maximum =
		(static_cast<__int128>(1) << (bits - 1U)) - 1;
	const auto sign = std::uint64_t{1} << (bits - 1U);
	const auto flags =
		((result & sign) != 0U ? 0x8000'0000U : 0U) |
		(result == 0U ? 0x4000'0000U : 0U) |
		((unsigned_sum >> bits) != 0U ? 0x2000'0000U : 0U) |
		(signed_sum < minimum || signed_sum > maximum ?
			0x1000'0000U : 0U);
	return {result, flags};
}

static_assert(add_with_carry(
	0x7fff'ffffU, 1U, false, false, false).value == 0x8000'0000U);
static_assert(add_with_carry(
	0x7fff'ffffU, 1U, false, false, false).nzcv == 0x9000'0000U);
static_assert(add_with_carry(
	0xffff'ffffU, 1U, false, false, false).nzcv == 0x6000'0000U);
static_assert(add_with_carry(
	0U, 1U, true, true, false).nzcv == 0x8000'0000U);
static_assert(add_with_carry(
	0x8000'0000U, 1U, true, true, false).nzcv == 0x3000'0000U);
static_assert(add_with_carry(
	0U, 0U, true, true, false).nzcv == 0x6000'0000U);

[[nodiscard]] inline arm64::CpuState expected_carry(
	const arm64::CpuState &initial, std::uint32_t instruction)
{
	const bool wide = (instruction & 0x8000'0000U) != 0U;
	const bool subtract = (instruction & 0x4000'0000U) != 0U;
	const bool set_flags = (instruction & 0x2000'0000U) != 0U;
	const unsigned destination = instruction & 31U;
	const auto result = add_with_carry(
		register_value(initial, (instruction >> 5) & 31U),
		register_value(initial, (instruction >> 16) & 31U),
		(initial.nzcv & 0x2000'0000U) != 0U, subtract, wide);
	auto expected = initial;
	if (destination != 31U)
		expected.registers[destination] = result.value;
	if (set_flags)
		expected.nzcv = result.nzcv;
	return expected;
}

[[nodiscard]] constexpr std::uint64_t divide_result(
	std::uint64_t dividend, std::uint64_t divisor, bool signed_divide,
	bool wide) noexcept
{
	const auto mask = width_mask(wide);
	dividend &= mask;
	divisor &= mask;
	if (divisor == 0U)
		return 0U;
	if (!signed_divide)
		return dividend / divisor;
	const auto quotient =
		signed_value(dividend, wide) / signed_value(divisor, wide);
	return static_cast<std::uint64_t>(
		static_cast<unsigned __int128>(quotient)) & mask;
}

static_assert(divide_result(17U, 0U, false, false) == 0U);
static_assert(divide_result(17U, 3U, false, false) == 5U);
static_assert(divide_result(
	0x8000'0000U, 0xffff'ffffU, true, false) == 0x8000'0000U);
static_assert(divide_result(
	0xffff'fff7U, 3U, true, false) == 0xffff'fffdU);

[[nodiscard]] inline arm64::CpuState expected_divide(
	const arm64::CpuState &initial, std::uint32_t instruction)
{
	const bool wide = (instruction & 0x8000'0000U) != 0U;
	const bool signed_divide = (instruction & 0x400U) != 0U;
	const unsigned destination = instruction & 31U;
	auto expected = initial;
	if (destination != 31U)
		expected.registers[destination] = divide_result(
			register_value(initial, (instruction >> 5) & 31U),
			register_value(initial, (instruction >> 16) & 31U),
			signed_divide, wide);
	return expected;
}

[[nodiscard]] constexpr std::uint64_t arithmetic_shift_right(
	std::uint64_t value, unsigned shift, bool wide) noexcept
{
	const unsigned bits = wide ? 64U : 32U;
	const auto mask = width_mask(wide);
	value &= mask;
	if (shift == 0U)
		return value;
	const auto result = value >> shift;
	const auto sign = std::uint64_t{1} << (bits - 1U);
	if ((value & sign) == 0U)
		return result;
	const auto retained = (std::uint64_t{1} << (bits - shift)) - 1U;
	return result | (mask & ~retained);
}

[[nodiscard]] constexpr std::uint64_t variable_shift_result(
	std::uint64_t value, std::uint64_t encoded_shift,
	ShiftOperation operation, bool wide) noexcept
{
	const unsigned bits = wide ? 64U : 32U;
	const auto mask = width_mask(wide);
	value &= mask;
	const auto shift =
		static_cast<unsigned>(encoded_shift) & (bits - 1U);
	switch (operation) {
	case ShiftOperation::left:
		return (value << shift) & mask;
	case ShiftOperation::right:
		return value >> shift;
	case ShiftOperation::arithmetic_right:
		return arithmetic_shift_right(value, shift, wide);
	case ShiftOperation::rotate_right:
		return shift == 0U ? value :
			((value >> shift) | (value << (bits - shift))) & mask;
	}
	return 0U;
}

static_assert(variable_shift_result(
	1U, 32U, ShiftOperation::left, false) == 1U);
static_assert(variable_shift_result(
	0x8000'0000U, 31U, ShiftOperation::arithmetic_right,
	false) == 0xffff'ffffU);
static_assert(variable_shift_result(
	0x8000'0001U, 1U, ShiftOperation::rotate_right,
	false) == 0xc000'0000U);

[[nodiscard]] inline arm64::CpuState expected_variable_shift(
	const arm64::CpuState &initial, std::uint32_t instruction)
{
	const bool wide = (instruction & 0x8000'0000U) != 0U;
	const auto operation = static_cast<ShiftOperation>(
		(instruction >> 10) & 3U);
	const unsigned destination = instruction & 31U;
	auto expected = initial;
	if (destination != 31U)
		expected.registers[destination] = variable_shift_result(
			register_value(initial, (instruction >> 5) & 31U),
			register_value(initial, (instruction >> 16) & 31U),
			operation, wide);
	return expected;
}

inline void check_carry(
	EngineHarness &harness, std::string_view name, std::uint32_t instruction,
	const arm64::CpuState &initial)
{
	harness.check_one(name, instruction, initial,
		expected_carry(initial, instruction),
		NativeRequirements::baseline());
}

inline void check_divide(
	EngineHarness &harness, std::string_view name, std::uint32_t instruction,
	const arm64::CpuState &initial)
{
	harness.check_one(name, instruction, initial,
		expected_divide(initial, instruction),
		NativeRequirements::baseline());
}

inline void check_variable_shift(
	EngineHarness &harness, std::string_view name, std::uint32_t instruction,
	const arm64::CpuState &initial)
{
	harness.check_one(name, instruction, initial,
		expected_variable_shift(initial, instruction),
		NativeRequirements::baseline());
}

} // namespace uml::backend::tests::base_arithmetic
