// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace uml::emulator::tests::base_condition {

namespace arm64 = uml::cpu::arm64;

enum class SelectOperation : unsigned {
	select,
	increment,
	invert,
	negate,
};

struct SelectDescription final {
	SelectOperation operation;
	std::string_view id;
};

inline constexpr std::array select_descriptions{
	SelectDescription{SelectOperation::select, "BASE-0138 CSEL"},
	SelectDescription{SelectOperation::increment, "BASE-0141 CSINC"},
	SelectDescription{SelectOperation::invert, "BASE-0142 CSINV"},
	SelectDescription{SelectOperation::negate, "BASE-0143 CSNEG"},
};

[[nodiscard]] inline arm64::CpuState state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			seed ^ (0x1020'3040'5060'7080ULL +
				index * 0x0101'0101'0101'0101ULL);
	cpu.sp = seed ^ 0xa5a5'5a5a'1234'5678ULL;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.tls = seed ^ 0xfedc'ba98'7654'3210ULL;
	for (unsigned index = 0; index < cpu.vectors.size(); ++index)
		for (unsigned byte = 0;
		     byte < cpu.vectors[index].bytes.size(); ++byte)
			cpu.vectors[index].bytes[byte] =
				static_cast<std::byte>(
					seed + index * 19U + byte);
	return cpu;
}

/*
 * Independent condition-code oracle.  This deliberately spells out all
 * sixteen architectural truth functions rather than sharing the production
 * condition decoder.
 */
[[nodiscard]] constexpr bool condition_holds(
	std::uint32_t nzcv, unsigned condition)
{
	const bool n = (nzcv & 0x8000'0000U) != 0;
	const bool z = (nzcv & 0x4000'0000U) != 0;
	const bool c = (nzcv & 0x2000'0000U) != 0;
	const bool v = (nzcv & 0x1000'0000U) != 0;
	switch (condition & 15U) {
	case 0: return z;
	case 1: return !z;
	case 2: return c;
	case 3: return !c;
	case 4: return n;
	case 5: return !n;
	case 6: return v;
	case 7: return !v;
	case 8: return c && !z;
	case 9: return !c || z;
	case 10: return n == v;
	case 11: return n != v;
	case 12: return !z && n == v;
	case 13: return z || n != v;
	case 14:
	case 15: return true;
	default: return false;
	}
}

[[nodiscard]] constexpr std::uint32_t encode_select(
	bool wide, SelectOperation operation, unsigned condition, unsigned rd,
	unsigned rn, unsigned rm)
{
	const auto op = static_cast<unsigned>(operation);
	return 0x1a80'0000U | (wide ? 0x8000'0000U : 0U) |
		((op & 2U) << 29U) | ((rm & 31U) << 16U) |
		((condition & 15U) << 12U) | ((op & 1U) << 10U) |
		((rn & 31U) << 5U) | (rd & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_select_alias(
	bool wide, SelectOperation operation, unsigned alias_condition,
	unsigned rd, unsigned source)
{
	return encode_select(wide, operation, alias_condition ^ 1U, rd,
		source, source);
}

[[nodiscard]] constexpr std::uint32_t encode_set_alias(
	bool wide, SelectOperation operation, unsigned alias_condition,
	unsigned rd)
{
	return encode_select(wide, operation, alias_condition ^ 1U, rd, 31U,
		31U);
}

[[nodiscard]] constexpr std::uint64_t read_zero_register(
	const arm64::CpuState &cpu, unsigned index)
{
	return index == 31U ? 0U : cpu.registers[index];
}

[[nodiscard]] inline arm64::CpuState expected_select(
	const arm64::CpuState &initial, std::uint32_t instruction)
{
	const bool wide = (instruction & 0x8000'0000U) != 0;
	const auto mask =
		wide ? ~std::uint64_t{0} : std::uint64_t{0xffff'ffffU};
	const auto operation = static_cast<SelectOperation>(
		((instruction >> 29U) & 2U) |
		((instruction >> 10U) & 1U));
	const unsigned condition = (instruction >> 12U) & 15U;
	const unsigned rn = (instruction >> 5U) & 31U;
	const unsigned rm = (instruction >> 16U) & 31U;
	const unsigned rd = instruction & 31U;

	auto result = read_zero_register(initial, rn);
	if (!condition_holds(initial.nzcv, condition)) {
		result = read_zero_register(initial, rm);
		switch (operation) {
		case SelectOperation::select: break;
		case SelectOperation::increment: result += 1U; break;
		case SelectOperation::invert: result = ~result; break;
		case SelectOperation::negate: result = 0U - result; break;
		}
	}

	auto expected = initial;
	if (rd != 31U)
		expected.registers[rd] = result & mask;
	return expected;
}

[[nodiscard]] constexpr std::uint32_t encode_compare(
	bool wide, bool subtract, bool immediate, unsigned condition,
	unsigned alternative, unsigned rn, unsigned operand)
{
	return 0x3a40'0000U | (wide ? 0x8000'0000U : 0U) |
		(subtract ? 0x4000'0000U : 0U) |
		((operand & 31U) << 16U) | ((condition & 15U) << 12U) |
		(immediate ? 0x0000'0800U : 0U) |
		((rn & 31U) << 5U) | (alternative & 15U);
}

[[nodiscard]] constexpr std::string_view compare_id(
	bool subtract, bool immediate)
{
	if (subtract)
		return immediate ? "BASE-0081 CCMP (immediate)" :
			"BASE-0082 CCMP (register)";
	return immediate ? "BASE-0079 CCMN (immediate)" :
		"BASE-0080 CCMN (register)";
}

[[nodiscard]] constexpr std::uint32_t add_with_carry_flags(
	std::uint64_t left, std::uint64_t right, bool carry_in, bool wide)
{
	const unsigned bits = wide ? 64U : 32U;
	const auto mask =
		wide ? ~std::uint64_t{0} : std::uint64_t{0xffff'ffffU};
	left &= mask;
	right &= mask;
	const auto sum = static_cast<unsigned __int128>(left) + right +
		static_cast<unsigned>(carry_in);
	const auto result = static_cast<std::uint64_t>(sum) & mask;
	const auto sign = std::uint64_t{1} << (bits - 1U);
	const bool n = (result & sign) != 0;
	const bool z = result == 0;
	const bool c = (sum >> bits) != 0;
	const bool left_sign = (left & sign) != 0;
	const bool right_sign = (right & sign) != 0;
	const bool result_sign = (result & sign) != 0;
	const bool v = left_sign == right_sign &&
		result_sign != left_sign;
	return (n ? 0x8000'0000U : 0U) |
		(z ? 0x4000'0000U : 0U) |
		(c ? 0x2000'0000U : 0U) |
		(v ? 0x1000'0000U : 0U);
}

[[nodiscard]] inline arm64::CpuState expected_compare(
	const arm64::CpuState &initial, std::uint32_t instruction)
{
	const bool wide = (instruction & 0x8000'0000U) != 0;
	const bool subtract = (instruction & 0x4000'0000U) != 0;
	const bool immediate = (instruction & 0x0000'0800U) != 0;
	const unsigned condition = (instruction >> 12U) & 15U;
	const unsigned alternative = instruction & 15U;
	const unsigned rn = (instruction >> 5U) & 31U;
	const unsigned operand = (instruction >> 16U) & 31U;
	const auto mask =
		wide ? ~std::uint64_t{0} : std::uint64_t{0xffff'ffffU};

	auto expected = initial;
	if (!condition_holds(initial.nzcv, condition)) {
		expected.nzcv = alternative << 28U;
		return expected;
	}

	const auto left = read_zero_register(initial, rn) & mask;
	const auto right = (immediate ? static_cast<std::uint64_t>(operand) :
		read_zero_register(initial, operand)) & mask;
	expected.nzcv = subtract ?
		add_with_carry_flags(left, (~right) & mask, true, wide) :
		add_with_carry_flags(left, right, false, wide);
	return expected;
}

inline void check_select(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	harness.check_one(name, instruction, initial,
		expected_select(initial, instruction),
		NativeRequirements::baseline());
}

inline void check_compare(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	harness.check_one(name, instruction, initial,
		expected_compare(initial, instruction),
		NativeRequirements::baseline());
}

} // namespace uml::emulator::tests::base_condition
