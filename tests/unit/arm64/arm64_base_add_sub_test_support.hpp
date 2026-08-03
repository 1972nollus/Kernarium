// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace uml::emulator::tests::base_add_sub {

namespace arm64 = uml::cpu::arm64;

enum class Form {
	immediate,
	shifted_register,
	extended_register,
};

[[nodiscard]] constexpr std::uint64_t width_mask(bool wide) noexcept
{
	return wide ? ~std::uint64_t{0} : std::uint64_t{0xffff'ffffU};
}

[[nodiscard]] constexpr std::uint32_t encode_immediate(
	bool wide, bool subtract, bool set_flags, bool shift_12,
	unsigned immediate, unsigned source, unsigned destination) noexcept
{
	return 0x1100'0000U | (wide ? 0x8000'0000U : 0) |
		(subtract ? 0x4000'0000U : 0) |
		(set_flags ? 0x2000'0000U : 0) |
		(shift_12 ? 0x0040'0000U : 0) |
		((immediate & 0xfffU) << 10) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_shifted(
	bool wide, bool subtract, bool set_flags, unsigned shift_type,
	unsigned amount, unsigned left, unsigned right,
	unsigned destination) noexcept
{
	return 0x0b00'0000U | (wide ? 0x8000'0000U : 0) |
		(subtract ? 0x4000'0000U : 0) |
		(set_flags ? 0x2000'0000U : 0) |
		((shift_type & 3U) << 22) | ((right & 31U) << 16) |
		((amount & 63U) << 10) | ((left & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_extended(
	bool wide, bool subtract, bool set_flags, unsigned extension,
	unsigned amount, unsigned left, unsigned right,
	unsigned destination) noexcept
{
	return 0x0b20'0000U | (wide ? 0x8000'0000U : 0) |
		(subtract ? 0x4000'0000U : 0) |
		(set_flags ? 0x2000'0000U : 0) |
		((right & 31U) << 16) | ((extension & 7U) << 13) |
		((amount & 7U) << 10) | ((left & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index) {
		const auto ordinal = static_cast<std::uint64_t>(index + 1U);
		cpu.registers[index] =
			(seed ^ (ordinal * 0x9e37'79b9'7f4a'7c15ULL)) +
			(ordinal * 0x0102'0304'0506'0708ULL);
	}
	cpu.sp = seed ^ 0xfedc'ba98'7654'3210ULL;
	cpu.nzcv = static_cast<std::uint32_t>((seed >> 7) & 0xfU) << 28;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 29U + byte * 17U);
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.tls = seed ^ 0x0123'4567'89ab'cdefULL;
	cpu.set_mte_configuration(true, 0x5a5aU);
	cpu.set_mte_generator(true, 0x1357U, 9U,
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

[[nodiscard]] constexpr bool is_valid(
	Form form, std::uint32_t instruction) noexcept
{
	if (form == Form::immediate)
		return true;
	if (form == Form::extended_register)
		return ((instruction >> 10) & 7U) <= 4U;
	const bool wide = (instruction & 0x8000'0000U) != 0;
	const unsigned shift_type = (instruction >> 22) & 3U;
	const unsigned amount = (instruction >> 10) & 63U;
	return shift_type != 3U && (wide || amount < 32U);
}

[[nodiscard]] constexpr std::uint64_t register_value(
	const arm64::CpuState &cpu, unsigned index, bool stack_pointer) noexcept
{
	if (index != 31U)
		return cpu.registers[index];
	return stack_pointer ? cpu.sp : 0;
}

[[nodiscard]] constexpr std::uint64_t logical_shift_left(
	std::uint64_t value, unsigned amount, bool wide) noexcept
{
	return (value << amount) & width_mask(wide);
}

[[nodiscard]] constexpr std::uint64_t logical_shift_right(
	std::uint64_t value, unsigned amount, bool wide) noexcept
{
	return (value & width_mask(wide)) >> amount;
}

[[nodiscard]] constexpr std::uint64_t arithmetic_shift_right(
	std::uint64_t value, unsigned amount, bool wide) noexcept
{
	const unsigned bits = wide ? 64U : 32U;
	const auto mask = width_mask(wide);
	value &= mask;
	if (amount == 0)
		return value;
	const auto shifted = value >> amount;
	const auto sign = std::uint64_t{1} << (bits - 1U);
	if ((value & sign) == 0)
		return shifted;
	const auto low_bits = (std::uint64_t{1} << (bits - amount)) - 1U;
	return shifted | (mask & ~low_bits);
}

[[nodiscard]] constexpr std::uint64_t shifted_operand(
	std::uint64_t value, bool wide, unsigned shift_type,
	unsigned amount) noexcept
{
	switch (shift_type) {
	case 0:
		return logical_shift_left(value, amount, wide);
	case 1:
		return logical_shift_right(value, amount, wide);
	default:
		return arithmetic_shift_right(value, amount, wide);
	}
}

[[nodiscard]] constexpr std::uint64_t extended_operand(
	std::uint64_t value, bool wide, unsigned extension,
	unsigned amount) noexcept
{
	const unsigned source_bits = 8U << (extension & 3U);
	const auto source_mask = source_bits == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << source_bits) - 1U;
	value &= source_mask;
	const auto source_sign = std::uint64_t{1} << (source_bits - 1U);
	if ((extension & 4U) != 0 && (value & source_sign) != 0)
		value |= ~source_mask;
	return (value << amount) & width_mask(wide);
}

struct ArithmeticResult final {
	std::uint64_t value;
	std::uint32_t nzcv;
};

[[nodiscard]] constexpr ArithmeticResult arithmetic(
	std::uint64_t left, std::uint64_t right, bool wide,
	bool subtract) noexcept
{
	const unsigned bits = wide ? 64U : 32U;
	const auto mask = width_mask(wide);
	const auto sign = std::uint64_t{1} << (bits - 1U);
	left &= mask;
	right &= mask;

	const auto second = subtract ? (~right & mask) : right;
	const unsigned carry_in = subtract ? 1U : 0U;
	const auto sum = static_cast<unsigned __int128>(left) + second +
		carry_in;
	const auto result = static_cast<std::uint64_t>(sum) & mask;
	const bool carry = ((sum >> bits) & 1U) != 0;
	const bool overflow = subtract ?
		(((left ^ right) & (left ^ result) & sign) != 0) :
		(((~(left ^ right)) & (left ^ result) & sign) != 0);
	const auto flags =
		(result & sign ? 0x8000'0000U : 0U) |
		(result == 0 ? 0x4000'0000U : 0U) |
		(carry ? 0x2000'0000U : 0U) |
		(overflow ? 0x1000'0000U : 0U);
	return {result, flags};
}

static_assert(arithmetic(0x7fff'ffffU, 1, false, false).value ==
	0x8000'0000U);
static_assert(arithmetic(0x7fff'ffffU, 1, false, false).nzcv ==
	0x9000'0000U);
static_assert(arithmetic(0xffff'ffffU, 1, false, false).value == 0);
static_assert(arithmetic(0xffff'ffffU, 1, false, false).nzcv ==
	0x6000'0000U);
static_assert(arithmetic(0, 1, false, true).value == 0xffff'ffffU);
static_assert(arithmetic(0, 1, false, true).nzcv == 0x8000'0000U);
static_assert(arithmetic(0x8000'0000U, 1, false, true).value ==
	0x7fff'ffffU);
static_assert(arithmetic(0x8000'0000U, 1, false, true).nzcv ==
	0x3000'0000U);
static_assert(arithmetic(0xffff'ffff'ffff'ffffULL, 1, true, false).nzcv ==
	0x6000'0000U);

[[nodiscard]] inline arm64::CpuState expected_state(
	Form form, std::uint32_t instruction, const arm64::CpuState &initial)
{
	const bool wide = (instruction & 0x8000'0000U) != 0;
	const bool subtract = (instruction & 0x4000'0000U) != 0;
	const bool set_flags = (instruction & 0x2000'0000U) != 0;
	const unsigned destination = instruction & 31U;
	const unsigned left_index = (instruction >> 5) & 31U;
	const bool stack_form = form != Form::shifted_register;
	const auto left = register_value(initial, left_index, stack_form) &
		width_mask(wide);

	std::uint64_t right{};
	if (form == Form::immediate) {
		right = static_cast<std::uint64_t>(
			(instruction >> 10) & 0xfffU);
		if ((instruction & 0x0040'0000U) != 0)
			right <<= 12;
	} else {
		const unsigned right_index = (instruction >> 16) & 31U;
		const auto source = register_value(initial, right_index, false);
		if (form == Form::shifted_register)
			right = shifted_operand(source, wide,
				(instruction >> 22) & 3U,
				(instruction >> 10) & 63U);
		else
			right = extended_operand(source, wide,
				(instruction >> 13) & 7U,
				(instruction >> 10) & 7U);
	}

	const auto result = arithmetic(left, right, wide, subtract);
	auto expected = initial;
	if (set_flags)
		expected.nzcv = result.nzcv;
	if (destination != 31U)
		expected.registers[destination] = result.value;
	else if (stack_form && !set_flags)
		expected.sp = result.value;
	return expected;
}

inline void check(EngineHarness &harness, std::string_view name, Form form,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	if (!is_valid(form, instruction)) {
		std::fprintf(stderr,
			"valid add/subtract case has reserved instruction %#010x\n",
			instruction);
		std::abort();
	}
	harness.check_one(name, instruction, initial,
		expected_state(form, instruction, initial),
		NativeRequirements::baseline());
}

inline void check_illegal(EngineHarness &harness, std::string_view name,
	Form form, std::uint32_t instruction, const arm64::CpuState &initial)
{
	if (is_valid(form, instruction)) {
		std::fprintf(stderr,
			"reserved add/subtract case has valid instruction %#010x\n",
			instruction);
		std::abort();
	}
	harness.check_illegal(name, instruction, initial,
		NativeRequirements::baseline());
}

void run_immediate_tests(EngineHarness &harness);
void run_shifted_tests(EngineHarness &harness);
void run_extended_tests(EngineHarness &harness);

} // namespace uml::emulator::tests::base_add_sub
