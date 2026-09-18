// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::emulator::tests::base_arithmetic {

namespace arm64 = uml::cpu::arm64;

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

} // namespace uml::emulator::tests::base_arithmetic
