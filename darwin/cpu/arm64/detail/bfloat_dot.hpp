// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

namespace uml::cpu::arm64::detail {

struct BFloatDot final {
	bool recognized{};
	bool by_element{};
	bool wide{};
	unsigned index{};
};

[[nodiscard]] constexpr BFloatDot decode_bfloat_dot(
	std::uint32_t instruction) noexcept
{
	if ((instruction & 0xbfe0'fc00U) == 0x2e40'fc00U)
		return {true, false, (instruction & 0x4000'0000U) != 0U, 0U};
	if ((instruction & 0xbfc0'f400U) == 0x0f40'f000U) {
		const unsigned index = ((instruction >> 21) & 1U) |
			((instruction >> 10) & 2U);
		return {true, true, (instruction & 0x4000'0000U) != 0U, index};
	}
	return {};
}

namespace bfloat_dot_detail {

enum class Class : unsigned { zero, finite, infinity, nan };

struct Value final {
	Class type{};
	bool sign{};
	std::uint64_t significand{};
	int exponent{};
};

[[nodiscard]] constexpr std::uint32_t default_nan(std::uint32_t fpcr) noexcept
{
	return (fpcr & 2U) != 0U ? 0xffc0'0000U : 0x7fc0'0000U;
}

[[nodiscard]] constexpr Value unpack_standard_bfloat(
	std::uint16_t bits) noexcept
{
	const bool sign = (bits >> 15) != 0U;
	const unsigned exponent = (bits >> 7) & 0xffU;
	const unsigned fraction = bits & 0x7fU;
	if (exponent == 0U)
		return {Class::zero, sign, 0U, 0};
	if (exponent == 0xffU)
		return {fraction == 0U ? Class::infinity : Class::nan,
			sign, 0U, 0};
	return {Class::finite, sign, 0x80U | fraction,
		static_cast<int>(exponent) - 127 - 7};
}

[[nodiscard]] constexpr Value unpack_extended_bfloat(std::uint16_t bits,
	std::uint32_t fpcr) noexcept
{
	const bool sign = (bits >> 15) != 0U;
	const unsigned exponent = (bits >> 7) & 0xffU;
	const unsigned fraction = bits & 0x7fU;
	if (exponent == 0U) {
		if (fraction == 0U || (fpcr & 1U) != 0U ||
		    ((fpcr & (1U << 24)) != 0U && (fpcr & 2U) == 0U))
			return {Class::zero, sign, 0U, 0};
		return {Class::finite, sign, fraction, -126 - 7};
	}
	if (exponent == 0xffU)
		return {fraction == 0U ? Class::infinity : Class::nan,
			sign, 0U, 0};
	return {Class::finite, sign, 0x80U | fraction,
		static_cast<int>(exponent) - 127 - 7};
}

[[nodiscard]] constexpr Value unpack_standard_single(
	std::uint32_t bits) noexcept
{
	const bool sign = (bits >> 31) != 0U;
	const unsigned exponent = (bits >> 23) & 0xffU;
	const unsigned fraction = bits & 0x7f'ffffU;
	if (exponent == 0U)
		return {Class::zero, sign, 0U, 0};
	if (exponent == 0xffU)
		return {fraction == 0U ? Class::infinity : Class::nan,
			sign, 0U, 0};
	return {Class::finite, sign, 0x80'0000U | fraction,
		static_cast<int>(exponent) - 127 - 23};
}

[[nodiscard]] constexpr Value unpack_extended_single(std::uint32_t bits,
	std::uint32_t fpcr) noexcept
{
	const bool sign = (bits >> 31) != 0U;
	const unsigned exponent = (bits >> 23) & 0xffU;
	const unsigned fraction = bits & 0x7f'ffffU;
	if (exponent == 0U) {
		if (fraction == 0U || (fpcr & 1U) != 0U ||
		    ((fpcr & (1U << 24)) != 0U && (fpcr & 2U) == 0U))
			return {Class::zero, sign, 0U, 0};
		return {Class::finite, sign, fraction, -149};
	}
	if (exponent == 0xffU)
		return {fraction == 0U ? Class::infinity : Class::nan,
			sign, 0U, 0};
	return {Class::finite, sign, 0x80'0000U | fraction,
		static_cast<int>(exponent) - 127 - 23};
}

[[nodiscard]] constexpr Value product(const Value &left,
	const Value &right) noexcept
{
	return {Class::finite, left.sign != right.sign,
		left.significand * right.significand,
		left.exponent + right.exponent};
}

[[nodiscard]] constexpr unsigned bit_width128(unsigned __int128 value) noexcept
{
	const auto high = static_cast<std::uint64_t>(value >> 64);
	return high != 0U ? 64U + std::bit_width(high) :
		std::bit_width(static_cast<std::uint64_t>(value));
}

[[nodiscard]] constexpr unsigned __int128 shift_right_jam(
	unsigned __int128 value, unsigned shift) noexcept
{
	if (shift == 0U)
		return value;
	if (shift >= 128U)
		return value != 0U ? 1U : 0U;
	const auto shifted = value >> shift;
	const auto mask = (static_cast<unsigned __int128>(1U) << shift) - 1U;
	return shifted | static_cast<unsigned>(value & mask ? 1U : 0U);
}

struct Rounded final {
	unsigned __int128 value{};
	bool inexact{};
};

[[nodiscard]] constexpr Rounded round_shift(unsigned __int128 value,
	unsigned shift, unsigned rounding, bool sign) noexcept
{
	if (shift == 0U)
		return {value, false};
	unsigned __int128 truncated{};
	unsigned __int128 remainder{};
	if (shift < 128U) {
		truncated = value >> shift;
		remainder = value &
			((static_cast<unsigned __int128>(1U) << shift) - 1U);
	} else {
		remainder = value;
	}
	if (remainder == 0U)
		return {truncated, false};
	if (rounding == 4U)
		return {truncated | 1U, true};
	bool increment{};
	if (rounding == 0U && shift <= 128U) {
		const auto halfway = static_cast<unsigned __int128>(1U) <<
			(shift - 1U);
		increment = remainder > halfway ||
			(remainder == halfway && (truncated & 1U) != 0U);
	} else if (rounding == 1U) {
		increment = !sign;
	} else if (rounding == 2U) {
		increment = sign;
	}
	return {truncated + static_cast<unsigned>(increment), true};
}

[[nodiscard]] constexpr std::uint32_t round_finite(unsigned __int128 magnitude,
	int unit_exponent, bool sign, std::uint32_t fpcr, unsigned rounding,
	bool fixed_bfloat_behavior) noexcept
{
	const unsigned high = bit_width128(magnitude) - 1U;
	int value_exponent = unit_exponent + static_cast<int>(high);
	if (fixed_bfloat_behavior && value_exponent < -126)
		return static_cast<std::uint32_t>(sign) << 31;
	const bool alternative = (fpcr & 2U) != 0U;
	if (!fixed_bfloat_behavior && !alternative &&
	    (fpcr & (1U << 24)) != 0U && value_exponent < -126)
		return static_cast<std::uint32_t>(sign) << 31;

	const bool normal = value_exponent >= -126;
	const int signed_shift = normal ? static_cast<int>(high) - 23 :
		-149 - unit_exponent;
	unsigned __int128 unrounded = magnitude;
	unsigned shift{};
	if (signed_shift < 0)
		unrounded <<= static_cast<unsigned>(-signed_shift);
	else
		shift = static_cast<unsigned>(signed_shift);
	auto rounded = round_shift(unrounded, shift, rounding, sign);
	std::uint64_t significand = static_cast<std::uint64_t>(rounded.value);
	unsigned exponent_field{};
	if (normal) {
		exponent_field = static_cast<unsigned>(value_exponent + 127);
		if (significand >= 0x100'0000U) {
			significand >>= 1U;
			++exponent_field;
		}
	} else if (significand >= 0x80'0000U) {
		exponent_field = 1U;
		significand = 0U;
	}

	if (exponent_field >= 0xffU) {
		if (fixed_bfloat_behavior)
			return (static_cast<std::uint32_t>(sign) << 31) |
				0x7f80'0000U;
		const bool to_infinity = rounding == 0U ||
			(rounding == 1U && !sign) || (rounding == 2U && sign);
		return (static_cast<std::uint32_t>(sign) << 31) |
			(to_infinity ? 0x7f80'0000U : 0x7f7f'ffffU);
	}
	std::uint32_t result = (static_cast<std::uint32_t>(sign) << 31) |
		(exponent_field << 23) |
		(static_cast<std::uint32_t>(significand) & 0x7f'ffffU);
	if (!fixed_bfloat_behavior && alternative &&
	    (fpcr & (1U << 24)) != 0U && exponent_field == 0U &&
	    (result & 0x7f'ffffU) != 0U)
		result = static_cast<std::uint32_t>(sign) << 31;
	return result;
}

[[nodiscard]] constexpr std::uint32_t add_finite(Value left, Value right,
	std::uint32_t fpcr, unsigned rounding, bool fixed_bfloat_behavior) noexcept
{
	if (left.significand == 0U && right.significand == 0U) {
		const bool sign = left.sign == right.sign ? left.sign : rounding == 2U;
		return static_cast<std::uint32_t>(sign) << 31;
	}
	constexpr unsigned target = 120U;
	const int left_top = left.significand == 0U ? -10000 : left.exponent +
		static_cast<int>(std::bit_width(left.significand)) - 1;
	const int right_top = right.significand == 0U ? -10000 : right.exponent +
		static_cast<int>(std::bit_width(right.significand)) - 1;
	const int top = std::max(left_top, right_top);
	const auto align = [top](const Value &value) constexpr {
		if (value.significand == 0U)
			return static_cast<unsigned __int128>(0U);
		const unsigned high = std::bit_width(value.significand) - 1U;
		auto aligned = static_cast<unsigned __int128>(value.significand) <<
			(target - high);
		const unsigned difference = static_cast<unsigned>(top -
			(value.exponent + static_cast<int>(high)));
		return shift_right_jam(aligned, difference);
	};
	const auto left_magnitude = align(left);
	const auto right_magnitude = align(right);
	unsigned __int128 magnitude;
	bool sign;
	if (left.sign == right.sign) {
		magnitude = left_magnitude + right_magnitude;
		sign = left.sign;
	} else if (left_magnitude >= right_magnitude) {
		magnitude = left_magnitude - right_magnitude;
		sign = left.sign;
	} else {
		magnitude = right_magnitude - left_magnitude;
		sign = right.sign;
	}
	if (magnitude == 0U)
		return static_cast<std::uint32_t>(rounding == 2U) << 31;
	return round_finite(magnitude, top - static_cast<int>(target), sign,
		fpcr, rounding, fixed_bfloat_behavior);
}

[[nodiscard]] constexpr std::uint32_t standard_multiply(std::uint16_t left_bits,
	std::uint16_t right_bits, std::uint32_t fpcr) noexcept
{
	const auto left = unpack_standard_bfloat(left_bits);
	const auto right = unpack_standard_bfloat(right_bits);
	if (left.type == Class::nan || right.type == Class::nan)
		return default_nan(fpcr);
	if ((left.type == Class::infinity && right.type == Class::zero) ||
	    (left.type == Class::zero && right.type == Class::infinity))
		return default_nan(fpcr);
	const bool sign = left.sign != right.sign;
	if (left.type == Class::infinity || right.type == Class::infinity)
		return (static_cast<std::uint32_t>(sign) << 31) | 0x7f80'0000U;
	if (left.type == Class::zero || right.type == Class::zero)
		return static_cast<std::uint32_t>(sign) << 31;
	const auto multiplied = product(left, right);
	return round_finite(multiplied.significand, multiplied.exponent, sign,
		fpcr, 4U, true);
}

[[nodiscard]] constexpr std::uint32_t standard_add(std::uint32_t left_bits,
	std::uint32_t right_bits, std::uint32_t fpcr) noexcept
{
	const auto left = unpack_standard_single(left_bits);
	const auto right = unpack_standard_single(right_bits);
	if (left.type == Class::nan || right.type == Class::nan)
		return default_nan(fpcr);
	if (left.type == Class::infinity && right.type == Class::infinity &&
	    left.sign != right.sign)
		return default_nan(fpcr);
	if (left.type == Class::infinity)
		return left_bits;
	if (right.type == Class::infinity)
		return right_bits;
	return add_finite(left, right, fpcr, 4U, true);
}

[[nodiscard]] constexpr std::uint32_t extended_product_sum(
	std::uint16_t left_a_bits, std::uint16_t left_b_bits,
	std::uint16_t right_a_bits, std::uint16_t right_b_bits,
	std::uint32_t fpcr) noexcept
{
	const auto left_a = unpack_extended_bfloat(left_a_bits, fpcr);
	const auto left_b = unpack_extended_bfloat(left_b_bits, fpcr);
	const auto right_a = unpack_extended_bfloat(right_a_bits, fpcr);
	const auto right_b = unpack_extended_bfloat(right_b_bits, fpcr);
	if (left_a.type == Class::nan || left_b.type == Class::nan ||
	    right_a.type == Class::nan || right_b.type == Class::nan)
		return default_nan(fpcr);
	const bool zero_a = left_a.type == Class::zero ||
		right_a.type == Class::zero;
	const bool zero_b = left_b.type == Class::zero ||
		right_b.type == Class::zero;
	const bool infinity_a = left_a.type == Class::infinity ||
		right_a.type == Class::infinity;
	const bool infinity_b = left_b.type == Class::infinity ||
		right_b.type == Class::infinity;
	const bool sign_a = left_a.sign != right_a.sign;
	const bool sign_b = left_b.sign != right_b.sign;
	if ((zero_a && infinity_a) || (zero_b && infinity_b) ||
	    (infinity_a && infinity_b && sign_a != sign_b))
		return default_nan(fpcr);
	if (infinity_a || infinity_b) {
		const bool sign = infinity_a ? sign_a : sign_b;
		return (static_cast<std::uint32_t>(sign) << 31) | 0x7f80'0000U;
	}
	Value product_a{Class::finite, sign_a, 0U, 0};
	Value product_b{Class::finite, sign_b, 0U, 0};
	if (!zero_a)
		product_a = product(left_a, right_a);
	if (!zero_b)
		product_b = product(left_b, right_b);
	return add_finite(product_a, product_b, fpcr, (fpcr >> 22) & 3U,
		false);
}

[[nodiscard]] constexpr std::uint32_t extended_add(std::uint32_t left_bits,
	std::uint32_t right_bits, std::uint32_t fpcr) noexcept
{
	const auto left = unpack_extended_single(left_bits, fpcr);
	const auto right = unpack_extended_single(right_bits, fpcr);
	if (left.type == Class::nan || right.type == Class::nan)
		return default_nan(fpcr);
	if (left.type == Class::infinity && right.type == Class::infinity &&
	    left.sign != right.sign)
		return default_nan(fpcr);
	if (left.type == Class::infinity)
		return left_bits;
	if (right.type == Class::infinity)
		return right_bits;
	return add_finite(left, right, fpcr, (fpcr >> 22) & 3U, false);
}

} // namespace bfloat_dot_detail

[[nodiscard]] constexpr std::uint32_t bfloat_dot_add(std::uint32_t addend,
	std::uint16_t left_a, std::uint16_t left_b, std::uint16_t right_a,
	std::uint16_t right_b, std::uint32_t fpcr) noexcept
{
	using namespace bfloat_dot_detail;
	if ((fpcr & (1U << 13)) == 0U) {
		const auto product_a = standard_multiply(left_a, right_a, fpcr);
		const auto product_b = standard_multiply(left_b, right_b, fpcr);
		return standard_add(addend,
			standard_add(product_a, product_b, fpcr), fpcr);
	}
	const auto product_sum = extended_product_sum(left_a, left_b,
		right_a, right_b, fpcr | (1U << 25));
	return extended_add(addend, product_sum, fpcr | (1U << 25));
}

} // namespace uml::cpu::arm64::detail
