// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace uml::emulator::tests::base_logical {

namespace arm64 = uml::cpu::arm64;

enum class Form {
	immediate,
	shifted_register,
};

[[nodiscard]] constexpr std::uint64_t width_mask(bool wide) noexcept
{
	return wide ? ~std::uint64_t{0} : std::uint64_t{0xffff'ffffU};
}

[[nodiscard]] constexpr std::uint32_t encode_immediate(
	bool wide, unsigned operation, unsigned n, unsigned rotate,
	unsigned set_bits, unsigned source, unsigned destination) noexcept
{
	return 0x1200'0000U | (wide ? 0x8000'0000U : 0U) |
		((operation & 3U) << 29) | ((n & 1U) << 22) |
		((rotate & 63U) << 16) | ((set_bits & 63U) << 10) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_shifted(
	bool wide, unsigned operation, bool invert, unsigned shift_type,
	unsigned amount, unsigned left, unsigned right,
	unsigned destination) noexcept
{
	return 0x0a00'0000U | (wide ? 0x8000'0000U : 0U) |
		((operation & 3U) << 29) | ((shift_type & 3U) << 22) |
		(invert ? 0x0020'0000U : 0U) | ((right & 31U) << 16) |
		((amount & 63U) << 10) | ((left & 31U) << 5) |
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

[[nodiscard]] constexpr std::uint64_t rotate_right(
	std::uint64_t value, unsigned amount, unsigned width) noexcept
{
	const auto mask = width == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << width) - 1U;
	value &= mask;
	amount %= width;
	if (amount == 0U)
		return value;
	return ((value >> amount) | (value << (width - amount))) & mask;
}

/*
 * Independent transcription of DecodeBitMasks for logical immediates.  This
 * intentionally does not call the implementation's shared decoder.
 */
[[nodiscard]] constexpr std::optional<std::uint64_t>
decode_immediate_oracle(bool wide, unsigned n, unsigned rotate,
	unsigned set_bits) noexcept
{
	if (!wide && n != 0U)
		return std::nullopt;
	const unsigned marker = (n << 6) | ((~set_bits) & 63U);
	if (marker == 0U)
		return std::nullopt;
	const unsigned length = std::bit_width(marker) - 1U;
	if (length < 1U || (!wide && length > 5U))
		return std::nullopt;
	const unsigned levels = (1U << length) - 1U;
	const unsigned run = set_bits & levels;
	if (run == levels)
		return std::nullopt;
	const unsigned element_width = 1U << length;
	const unsigned one_count = run + 1U;
	const auto ones = one_count == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << one_count) - 1U;
	const auto element = rotate_right(
		ones, rotate & levels, element_width);
	std::uint64_t result = 0;
	for (unsigned position = 0; position < (wide ? 64U : 32U);
	     position += element_width)
		result |= element << position;
	return result & width_mask(wide);
}

[[nodiscard]] constexpr bool move_wide_preferred(
	bool wide, unsigned n, unsigned set_bits, unsigned rotate) noexcept
{
	if (wide) {
		if (n != 1U)
			return false;
	} else if (n != 0U || (set_bits & 32U) != 0U) {
		return false;
	}
	const unsigned width = wide ? 64U : 32U;
	if (set_bits < 16U)
		return ((16U - (rotate & 15U)) & 15U) <=
			(15U - set_bits);
	if (set_bits >= width - 15U)
		return (rotate & 15U) <= (set_bits - (width - 15U));
	return false;
}

[[nodiscard]] constexpr bool is_valid(
	Form form, std::uint32_t instruction) noexcept
{
	const bool wide = (instruction & 0x8000'0000U) != 0U;
	if (form == Form::shifted_register)
		return wide || ((instruction >> 10) & 63U) < 32U;
	return decode_immediate_oracle(
		wide, (instruction >> 22) & 1U,
		(instruction >> 16) & 63U,
		(instruction >> 10) & 63U).has_value();
}

[[nodiscard]] constexpr std::uint64_t register_value(
	const arm64::CpuState &cpu, unsigned index) noexcept
{
	return index == 31U ? 0U : cpu.registers[index];
}

[[nodiscard]] constexpr std::uint64_t arithmetic_shift_right(
	std::uint64_t value, unsigned amount, bool wide) noexcept
{
	const unsigned width = wide ? 64U : 32U;
	const auto mask = width_mask(wide);
	value &= mask;
	if (amount == 0U)
		return value;
	auto result = value >> amount;
	const auto sign = std::uint64_t{1} << (width - 1U);
	if ((value & sign) != 0U)
		result |= mask & (~std::uint64_t{0} << (width - amount));
	return result & mask;
}

[[nodiscard]] constexpr std::uint64_t shifted_operand(
	std::uint64_t value, bool wide, unsigned shift_type,
	unsigned amount) noexcept
{
	const auto mask = width_mask(wide);
	value &= mask;
	switch (shift_type) {
	case 0:
		return (value << amount) & mask;
	case 1:
		return value >> amount;
	case 2:
		return arithmetic_shift_right(value, amount, wide);
	default:
		return rotate_right(value, amount, wide ? 64U : 32U);
	}
}

[[nodiscard]] constexpr std::uint64_t apply_operation(
	unsigned operation, std::uint64_t left, std::uint64_t right) noexcept
{
	switch (operation) {
	case 0:
	case 3:
		return left & right;
	case 1:
		return left | right;
	default:
		return left ^ right;
	}
}

[[nodiscard]] inline arm64::CpuState expected_state(
	Form form, std::uint32_t instruction, const arm64::CpuState &initial)
{
	const bool wide = (instruction & 0x8000'0000U) != 0U;
	const auto mask = width_mask(wide);
	const unsigned operation = (instruction >> 29) & 3U;
	const unsigned destination = instruction & 31U;
	const unsigned left_index = (instruction >> 5) & 31U;
	const auto left = register_value(initial, left_index) & mask;
	std::uint64_t right;
	if (form == Form::immediate) {
		const auto decoded = decode_immediate_oracle(
			wide, (instruction >> 22) & 1U,
			(instruction >> 16) & 63U,
			(instruction >> 10) & 63U);
		if (!decoded) {
			std::fprintf(stderr,
				"logical oracle asked to execute reserved instruction "
				"%#010x\n", instruction);
			std::abort();
		}
		right = *decoded;
	} else {
		const unsigned right_index = (instruction >> 16) & 31U;
		right = shifted_operand(register_value(initial, right_index),
			wide, (instruction >> 22) & 3U,
			(instruction >> 10) & 63U);
		if ((instruction & 0x0020'0000U) != 0U)
			right = ~right & mask;
	}
	const auto result = apply_operation(operation, left, right) & mask;
	auto expected = initial;
	if (operation == 3U) {
		const auto sign = std::uint64_t{1} << (wide ? 63U : 31U);
		expected.nzcv = (result & sign ? 0x8000'0000U : 0U) |
			(result == 0U ? 0x4000'0000U : 0U);
	}
	if (destination != 31U)
		expected.registers[destination] = result;
	else if (form == Form::immediate && operation != 3U)
		expected.sp = result;
	return expected;
}

inline void check(EngineHarness &harness, std::string_view name, Form form,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	if (!is_valid(form, instruction)) {
		std::fprintf(stderr,
			"valid logical case has reserved instruction %#010x\n",
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
			"reserved logical case has valid instruction %#010x\n",
			instruction);
		std::abort();
	}
	harness.check_illegal(name, instruction, initial,
		NativeRequirements::baseline());
}

void run_immediate_tests(EngineHarness &harness);
void run_shifted_tests(EngineHarness &harness);
void run_behavior_tests(EngineHarness &harness);

} // namespace uml::emulator::tests::base_logical
