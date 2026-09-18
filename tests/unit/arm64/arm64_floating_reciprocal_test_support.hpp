// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

namespace arm64_floating_reciprocal_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	reciprocal_estimate,
	reciprocal_step,
	reciprocal_exponent,
	reciprocal_sqrt_estimate,
	reciprocal_sqrt_step,
};

struct Form final {
	Operation operation;
	bool scalar;
	unsigned width;
	unsigned vector_bytes;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

struct Format final {
	unsigned width;
	unsigned exponent_bits;
	unsigned fraction_bits;
	int bias;
};

struct LaneResult final {
	std::uint64_t bits{};
	std::uint32_t flags{};
};

/*
 * A fused double-precision step can align its exact product and constant over
 * about 2,200 binary places. This deliberately tiny test-only unsigned type
 * provides only the shifts, add/subtract, and bit queries needed by FPRound.
 */
struct BigUnsigned final {
	static constexpr unsigned word_bits = 64U;
	static constexpr unsigned words = 36U;
	std::array<std::uint64_t, words> data{};

	[[nodiscard]] bool empty() const noexcept
	{
		for (const auto word : data)
			if (word != 0U)
				return false;
		return true;
	}

	[[nodiscard]] unsigned highest_bit() const
	{
		for (unsigned index = words; index-- != 0U;)
			if (data[index] != 0U)
				return index * word_bits +
					(63U - static_cast<unsigned>(
						std::countl_zero(data[index])));
		std::abort();
	}

	[[nodiscard]] bool bit(unsigned index) const noexcept
	{
		return index < words * word_bits &&
			((data[index / word_bits] >>
			  (index % word_bits)) & 1U) != 0U;
	}

	[[nodiscard]] bool any_below(unsigned limit) const noexcept
	{
		const unsigned whole = std::min(limit / word_bits, words);
		for (unsigned index = 0; index < whole; ++index)
			if (data[index] != 0U)
				return true;
		if (whole < words && (limit % word_bits) != 0U)
			return (data[whole] &
				((std::uint64_t{1} << (limit % word_bits)) -
				 1U)) != 0U;
		return false;
	}

	[[nodiscard]] std::uint64_t shifted_to_u64(unsigned shift) const noexcept
	{
		const unsigned word = shift / word_bits;
		const unsigned bits = shift % word_bits;
		if (word >= words)
			return 0U;
		std::uint64_t result = data[word] >> bits;
		if (bits != 0U && word + 1U < words)
			result |= data[word + 1U] << (word_bits - bits);
		return result;
	}
};

[[nodiscard]] inline BigUnsigned shifted_value(
	unsigned __int128 value, unsigned shift)
{
	BigUnsigned result;
	const unsigned word = shift / BigUnsigned::word_bits;
	const unsigned bits = shift % BigUnsigned::word_bits;
	const std::array source{
		static_cast<std::uint64_t>(value),
		static_cast<std::uint64_t>(value >> 64U),
	};
	for (unsigned index = 0; index < source.size(); ++index) {
		if (word + index < BigUnsigned::words)
			result.data[word + index] |= source[index] << bits;
		if (bits != 0U && word + index + 1U < BigUnsigned::words)
			result.data[word + index + 1U] |=
				source[index] >> (64U - bits);
	}
	return result;
}

[[nodiscard]] inline int compare(const BigUnsigned &left,
	const BigUnsigned &right) noexcept
{
	for (unsigned index = BigUnsigned::words; index-- != 0U;) {
		if (left.data[index] < right.data[index])
			return -1;
		if (left.data[index] > right.data[index])
			return 1;
	}
	return 0;
}

[[nodiscard]] inline BigUnsigned add(const BigUnsigned &left,
	const BigUnsigned &right)
{
	BigUnsigned result;
	unsigned __int128 carry = 0;
	for (unsigned index = 0; index < BigUnsigned::words; ++index) {
		const unsigned __int128 sum =
			static_cast<unsigned __int128>(left.data[index]) +
			right.data[index] + carry;
		result.data[index] = static_cast<std::uint64_t>(sum);
		carry = sum >> 64U;
	}
	if (carry != 0U)
		std::abort();
	return result;
}

[[nodiscard]] inline BigUnsigned subtract(const BigUnsigned &left,
	const BigUnsigned &right)
{
	BigUnsigned result;
	std::uint64_t borrow = 0;
	for (unsigned index = 0; index < BigUnsigned::words; ++index) {
		const auto right_word = right.data[index];
		const auto first = left.data[index] - right_word;
		const bool first_borrow = left.data[index] < right_word;
		const auto second = first - borrow;
		const bool second_borrow = first < borrow;
		result.data[index] = second;
		borrow = static_cast<std::uint64_t>(
			first_borrow || second_borrow);
	}
	if (borrow != 0U)
		std::abort();
	return result;
}

enum class ValueClass {
	zero,
	finite,
	infinity,
	quiet_nan,
	signaling_nan,
};

struct Unpacked final {
	ValueClass type{};
	bool negative{};
	std::uint64_t significand{};
	int exponent{};
	std::uint32_t flags{};
	std::uint64_t raw{};
};

inline constexpr std::uint32_t fpsr_invalid = 1U << 0;
inline constexpr std::uint32_t fpsr_divide_by_zero = 1U << 1;
inline constexpr std::uint32_t fpsr_overflow = 1U << 2;
inline constexpr std::uint32_t fpsr_underflow = 1U << 3;
inline constexpr std::uint32_t fpsr_inexact = 1U << 4;
inline constexpr std::uint32_t fpsr_input_denormal = 1U << 7;

inline constexpr std::uint32_t fpcr_fiz = 1U << 0;
inline constexpr std::uint32_t fpcr_ah = 1U << 1;
inline constexpr std::uint32_t fpcr_fz16 = 1U << 19;
inline constexpr std::uint32_t fpcr_fz = 1U << 24;
inline constexpr std::uint32_t fpcr_dn = 1U << 25;

inline constexpr std::array widths{16U, 32U, 64U};
inline constexpr std::array vector_shapes{
	std::array{16U, 8U}, std::array{16U, 16U},
	std::array{32U, 8U}, std::array{32U, 16U},
	std::array{64U, 16U},
};

[[nodiscard]] constexpr bool unary(Operation operation) noexcept
{
	return operation != Operation::reciprocal_step &&
		operation != Operation::reciprocal_sqrt_step;
}

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto operation : {
		     Operation::reciprocal_estimate,
		     Operation::reciprocal_step,
		     Operation::reciprocal_exponent,
		     Operation::reciprocal_sqrt_estimate,
		     Operation::reciprocal_sqrt_step,
	     }) {
		for (const auto width : widths)
			function(Form{operation, true, width, 16U});
		if (operation == Operation::reciprocal_exponent)
			continue;
		for (const auto shape : vector_shapes)
			function(Form{operation, false, shape[0], shape[1]});
	}
}

[[nodiscard]] constexpr unsigned scalar_shape(unsigned width) noexcept
{
	return width == 16U ? 0U : width == 32U ? 1U : 2U;
}

[[nodiscard]] constexpr unsigned vector_shape(unsigned width,
	unsigned vector_bytes) noexcept
{
	return width == 16U ? (vector_bytes == 16U ? 1U : 0U) :
		width == 32U ? (vector_bytes == 16U ? 3U : 2U) : 4U;
}

/*
 * Direct transcription of the five encoding diagrams. The expected-value
 * model below deliberately neither includes nor calls the production decoder.
 */
[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	constexpr std::array scalar{
		std::array{0x5ef9'd800U, 0x5ea1'd800U, 0x5ee1'd800U},
		std::array{0x5e40'3c00U, 0x5e20'fc00U, 0x5e60'fc00U},
		std::array{0x5ef9'f800U, 0x5ea1'f800U, 0x5ee1'f800U},
		std::array{0x7ef9'd800U, 0x7ea1'd800U, 0x7ee1'd800U},
		std::array{0x5ec0'3c00U, 0x5ea0'fc00U, 0x5ee0'fc00U},
	};
	constexpr std::array vector{
		std::array{0x0ef9'd800U, 0x4ef9'd800U, 0x0ea1'd800U,
			0x4ea1'd800U, 0x4ee1'd800U},
		std::array{0x0e40'3c00U, 0x4e40'3c00U, 0x0e20'fc00U,
			0x4e20'fc00U, 0x4e60'fc00U},
		std::array<std::uint32_t, 5>{},
		std::array{0x2ef9'd800U, 0x6ef9'd800U, 0x2ea1'd800U,
			0x6ea1'd800U, 0x6ee1'd800U},
		std::array{0x0ec0'3c00U, 0x4ec0'3c00U, 0x0ea0'fc00U,
			0x4ea0'fc00U, 0x4ee0'fc00U},
	};
	const unsigned operation = static_cast<unsigned>(form.operation);
	std::uint32_t result = form.scalar ?
		scalar[operation][scalar_shape(form.width)] :
		vector[operation][vector_shape(form.width, form.vector_bytes)];
	result |= registers.destination & 31U;
	result |= (registers.first & 31U) << 5;
	if (!unary(form.operation))
		result |= (registers.second & 31U) << 16;
	return result;
}

[[noreturn]] inline void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

inline void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

[[nodiscard]] constexpr std::uint64_t low_mask(unsigned bits) noexcept
{
	return bits == 64U ? ~std::uint64_t{} :
		(std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] constexpr std::uint64_t sign_mask(
	const Format &value) noexcept
{
	return std::uint64_t{1} << (value.width - 1U);
}

[[nodiscard]] constexpr std::uint64_t fraction_mask(
	const Format &value) noexcept
{
	return low_mask(value.fraction_bits);
}

[[nodiscard]] constexpr std::uint64_t maximum_exponent(
	const Format &value) noexcept
{
	return low_mask(value.exponent_bits);
}

[[nodiscard]] constexpr std::uint64_t exponent_mask(
	const Format &value) noexcept
{
	return maximum_exponent(value) << value.fraction_bits;
}

[[nodiscard]] constexpr std::uint64_t zero_bits(
	const Format &value, bool negative = false) noexcept
{
	return negative ? sign_mask(value) : 0U;
}

[[nodiscard]] constexpr std::uint64_t infinity_bits(
	const Format &value, bool negative = false) noexcept
{
	return zero_bits(value, negative) | exponent_mask(value);
}

[[nodiscard]] constexpr std::uint64_t maximum_finite_bits(
	const Format &value, bool negative = false) noexcept
{
	return zero_bits(value, negative) |
		((maximum_exponent(value) - 1U) << value.fraction_bits) |
		fraction_mask(value);
}

[[nodiscard]] constexpr std::uint64_t default_nan_bits(
	const Format &value, std::uint32_t fpcr) noexcept
{
	return zero_bits(value, (fpcr & fpcr_ah) != 0U) |
		exponent_mask(value) |
		(std::uint64_t{1} << (value.fraction_bits - 1U));
}

[[nodiscard]] constexpr std::uint64_t normal_bits(const Format &value,
	unsigned exponent, std::uint64_t fraction,
	bool negative = false) noexcept
{
	return zero_bits(value, negative) |
		(static_cast<std::uint64_t>(exponent) << value.fraction_bits) |
		(fraction & fraction_mask(value));
}

[[nodiscard]] inline Unpacked unpack(std::uint64_t raw, const Format &value,
	std::uint32_t fpcr, bool suppress_exceptions)
{
	raw &= low_mask(value.width);
	const bool negative = (raw & sign_mask(value)) != 0U;
	const auto exponent =
		(raw & exponent_mask(value)) >> value.fraction_bits;
	const auto fraction = raw & fraction_mask(value);
	if (exponent == maximum_exponent(value)) {
		if (fraction == 0U)
			return {ValueClass::infinity, negative, 0, 0, 0, raw};
		const auto quiet =
			std::uint64_t{1} << (value.fraction_bits - 1U);
		return {
			(fraction & quiet) != 0U ?
				ValueClass::quiet_nan : ValueClass::signaling_nan,
			negative, 0, 0, 0, raw,
		};
	}
	if (exponent == 0U && fraction == 0U)
		return {ValueClass::zero, negative, 0, 0, 0, raw};

	const bool subnormal = exponent == 0U;
	if (subnormal && value.width == 16U &&
	    (fpcr & fpcr_fz16) != 0U)
		return {ValueClass::zero, negative, 0, 0, 0, raw};
	if (subnormal && value.width != 16U) {
		const bool alternative = (fpcr & fpcr_ah) != 0U;
		const bool classic_flush =
			(fpcr & fpcr_fz) != 0U && !alternative;
		const bool silent_flush =
			(fpcr & fpcr_fiz) != 0U || alternative;
		if (classic_flush || silent_flush)
			return {
				ValueClass::zero, negative, 0, 0,
				classic_flush && !suppress_exceptions ?
					fpsr_input_denormal : 0U,
				raw,
			};
	}

	const auto significand = subnormal ? fraction :
		(std::uint64_t{1} << value.fraction_bits) + fraction;
	const int binary_exponent = subnormal ?
		1 - value.bias - static_cast<int>(value.fraction_bits) :
		static_cast<int>(exponent) - value.bias -
			static_cast<int>(value.fraction_bits);
	return {ValueClass::finite, negative, significand,
		binary_exponent, 0, raw};
}

[[nodiscard]] constexpr bool is_nan(ValueClass type) noexcept
{
	return type == ValueClass::quiet_nan ||
		type == ValueClass::signaling_nan;
}

[[nodiscard]] constexpr LaneResult process_nan(std::uint64_t raw,
	const Format &value, std::uint32_t fpcr, bool signaling,
	bool suppress_exceptions) noexcept
{
	const auto quiet =
		std::uint64_t{1} << (value.fraction_bits - 1U);
	return {
		(fpcr & fpcr_dn) != 0U ?
			default_nan_bits(value, fpcr) : raw | quiet,
		signaling && !suppress_exceptions ? fpsr_invalid : 0U,
	};
}

[[nodiscard]] inline LaneResult process_binary_nan(const Unpacked &first,
	const Unpacked &second, const Format &value, std::uint32_t fpcr,
	bool suppress_exceptions)
{
	const bool alternative = (fpcr & fpcr_ah) != 0U;
	const bool first_nan = is_nan(first.type);
	const bool second_nan = is_nan(second.type);
	const bool any_signaling =
		first.type == ValueClass::signaling_nan ||
		second.type == ValueClass::signaling_nan;
	if (alternative && first_nan && second_nan)
		return process_nan(first.raw, value, fpcr, any_signaling,
			suppress_exceptions);
	if (first.type == ValueClass::signaling_nan)
		return process_nan(first.raw, value, fpcr, true,
			suppress_exceptions);
	if (second.type == ValueClass::signaling_nan)
		return process_nan(second.raw, value, fpcr, true,
			suppress_exceptions);
	if (first_nan)
		return process_nan(first.raw, value, fpcr, false,
			suppress_exceptions);
	return process_nan(second.raw, value, fpcr, false,
		suppress_exceptions);
}

[[nodiscard]] inline unsigned reciprocal_estimate_integer(unsigned scaled)
{
	const unsigned rounded = scaled * 2U + 1U;
	const unsigned quotient = (1U << 19) / rounded;
	return (quotient + 1U) / 2U;
}

[[nodiscard]] inline unsigned root_estimate_integer(unsigned scaled)
{
	if (scaled < 256U)
		scaled = scaled * 2U + 1U;
	else
		scaled = ((scaled >> 1U) * 2U + 1U) * 2U;
	unsigned estimate = 512U;
	while (static_cast<std::uint64_t>(scaled) *
		       (estimate + 1U) * (estimate + 1U) <
	       (std::uint64_t{1} << 28))
		++estimate;
	return (estimate + 1U) / 2U;
}

[[nodiscard]] inline LaneResult reciprocal_estimate(std::uint64_t raw,
	const Format &value, std::uint32_t fpcr)
{
	const bool alternative = (fpcr & fpcr_ah) != 0U;
	const bool suppress = alternative;
	auto decoded = unpack(raw, value, fpcr, suppress);
	std::uint32_t flags = decoded.flags;
	if (is_nan(decoded.type)) {
		auto result = process_nan(decoded.raw, value, fpcr,
			decoded.type == ValueClass::signaling_nan, suppress);
		result.flags |= flags;
		return result;
	}
	if (decoded.type == ValueClass::infinity)
		return {zero_bits(value, decoded.negative), flags};
	if (decoded.type == ValueClass::zero)
		return {
			infinity_bits(value, decoded.negative),
			flags | (suppress ? 0U : fpsr_divide_by_zero),
		};

	const auto exponent =
		(raw & exponent_mask(value)) >> value.fraction_bits;
	const auto fraction = raw & fraction_mask(value);
	if (exponent == 0U &&
	    fraction < (std::uint64_t{1} << (value.fraction_bits - 2U))) {
		const unsigned rounding = alternative ? 0U : (fpcr >> 22U) & 3U;
		const bool to_infinity = rounding == 0U ||
			(rounding == 1U && !decoded.negative) ||
			(rounding == 2U && decoded.negative);
		return {
			to_infinity ? infinity_bits(value, decoded.negative) :
				maximum_finite_bits(value, decoded.negative),
			flags | (suppress ? 0U :
				fpsr_overflow | fpsr_inexact),
		};
	}

	const bool output_flush = value.width == 16U ?
		(fpcr & fpcr_fz16) != 0U :
		((fpcr & fpcr_fz) != 0U || alternative);
	const unsigned flush_exponent = value.width == 16U ? 29U :
		value.width == 32U ? 253U : 2045U;
	if (output_flush && exponent >= flush_exponent)
		return {
			zero_bits(value, decoded.negative),
			flags | (suppress ? 0U : fpsr_underflow),
		};

	std::uint64_t fraction52 =
		fraction << (52U - value.fraction_bits);
	int adjusted_exponent = static_cast<int>(exponent);
	if (adjusted_exponent == 0) {
		if ((fraction52 & (std::uint64_t{1} << 51U)) == 0U) {
			adjusted_exponent = -1;
			fraction52 = (fraction52 << 2U) & low_mask(52U);
		} else {
			fraction52 = (fraction52 << 1U) & low_mask(52U);
		}
	}
	const unsigned scaled =
		256U | static_cast<unsigned>(fraction52 >> 44U);
	const unsigned estimate = reciprocal_estimate_integer(scaled);
	int result_exponent = value.width == 16U ? 29 - adjusted_exponent :
		value.width == 32U ? 253 - adjusted_exponent :
		2045 - adjusted_exponent;
	fraction52 = static_cast<std::uint64_t>(estimate & 0xffU) << 44U;
	if (result_exponent == 0) {
		fraction52 =
			(std::uint64_t{1} << 51U) | (fraction52 >> 1U);
	} else if (result_exponent == -1) {
		fraction52 =
			(std::uint64_t{1} << 50U) | (fraction52 >> 2U);
		result_exponent = 0;
	}
	return {
		zero_bits(value, decoded.negative) |
			(static_cast<std::uint64_t>(result_exponent) <<
				value.fraction_bits) |
			(fraction52 >> (52U - value.fraction_bits)),
		flags,
	};
}

[[nodiscard]] inline LaneResult reciprocal_sqrt_estimate(
	std::uint64_t raw, const Format &value, std::uint32_t fpcr)
{
	const bool alternative = (fpcr & fpcr_ah) != 0U;
	const bool suppress = alternative;
	auto decoded = unpack(raw, value, fpcr, suppress);
	std::uint32_t flags = decoded.flags;
	if (is_nan(decoded.type)) {
		auto result = process_nan(decoded.raw, value, fpcr,
			decoded.type == ValueClass::signaling_nan, suppress);
		result.flags |= flags;
		return result;
	}
	if (decoded.type == ValueClass::zero)
		return {
			infinity_bits(value, decoded.negative),
			flags | (suppress ? 0U : fpsr_divide_by_zero),
		};
	if (decoded.negative)
		return {
			default_nan_bits(value, fpcr),
			flags | (suppress ? 0U : fpsr_invalid),
		};
	if (decoded.type == ValueClass::infinity)
		return {zero_bits(value), flags};

	const auto exponent =
		(raw & exponent_mask(value)) >> value.fraction_bits;
	const auto fraction = raw & fraction_mask(value);
	std::uint64_t fraction52 =
		fraction << (52U - value.fraction_bits);
	int adjusted_exponent = static_cast<int>(exponent);
	if (adjusted_exponent == 0) {
		while ((fraction52 & (std::uint64_t{1} << 51U)) == 0U) {
			fraction52 = (fraction52 << 1U) & low_mask(52U);
			--adjusted_exponent;
		}
		fraction52 = (fraction52 << 1U) & low_mask(52U);
	}
	const unsigned scaled = (adjusted_exponent & 1) == 0 ?
		256U | static_cast<unsigned>(fraction52 >> 44U) :
		128U | static_cast<unsigned>(fraction52 >> 45U);
	const int numerator = value.width == 16U ? 44 - adjusted_exponent :
		value.width == 32U ? 380 - adjusted_exponent :
		3068 - adjusted_exponent;
	const unsigned result_exponent =
		static_cast<unsigned>(numerator / 2);
	const unsigned estimate = root_estimate_integer(scaled);
	return {
		static_cast<std::uint64_t>(result_exponent) <<
			value.fraction_bits |
			static_cast<std::uint64_t>(estimate & 0xffU) <<
				(value.fraction_bits - 8U),
		flags,
	};
}

[[nodiscard]] inline LaneResult reciprocal_exponent(std::uint64_t raw,
	const Format &value, std::uint32_t fpcr)
{
	const bool suppress = (fpcr & fpcr_ah) != 0U;
	auto decoded = unpack(raw, value, fpcr, suppress);
	if (is_nan(decoded.type)) {
		auto result = process_nan(decoded.raw, value, fpcr,
			decoded.type == ValueClass::signaling_nan, suppress);
		result.flags |= decoded.flags;
		return result;
	}
	const auto exponent =
		(raw & exponent_mask(value)) >> value.fraction_bits;
	const auto result_exponent = exponent == 0U ?
		maximum_exponent(value) - 1U :
		(~exponent) & maximum_exponent(value);
	return {
		zero_bits(value, decoded.negative) |
			(result_exponent << value.fraction_bits),
		decoded.flags,
	};
}

[[nodiscard]] inline LaneResult round_exact(
	const BigUnsigned &magnitude, bool negative, int binary_exponent,
	const Format &value, std::uint32_t fpcr)
{
	const bool alternative = (fpcr & fpcr_ah) != 0U;
	const unsigned rounding = alternative ? 0U : (fpcr >> 22U) & 3U;
	if (magnitude.empty())
		return {zero_bits(value, rounding == 2U), 0U};
	const int highest = static_cast<int>(magnitude.highest_bit());
	const int real_exponent = highest + binary_exponent;
	const int minimum_normal = 1 - value.bias;
	const int target_unit = std::max(
		real_exponent - static_cast<int>(value.fraction_bits),
		minimum_normal - static_cast<int>(value.fraction_bits));
	const int shift = target_unit - binary_exponent;
	std::uint64_t retained;
	bool inexact = false;
	if (shift > 0) {
		retained = magnitude.shifted_to_u64(
			static_cast<unsigned>(shift));
		inexact = magnitude.any_below(static_cast<unsigned>(shift));
	} else {
		retained = magnitude.shifted_to_u64(0U) <<
			static_cast<unsigned>(-shift);
	}

	const bool output_flush = value.width == 16U ?
		(fpcr & fpcr_fz16) != 0U :
		(fpcr & fpcr_fz) != 0U || alternative;
	if (!alternative && output_flush && real_exponent < minimum_normal)
		return {
			zero_bits(value, negative),
			fpsr_underflow,
		};

	bool increment = false;
	if (inexact) {
		switch (rounding) {
		case 0: {
			const auto halfway_bit =
				static_cast<unsigned>(shift - 1);
			const bool halfway = magnitude.bit(halfway_bit);
			const bool below_halfway =
				magnitude.any_below(halfway_bit);
			increment = halfway &&
				(below_halfway || (retained & 1U) != 0U);
			break;
		}
		case 1: increment = !negative; break;
		case 2: increment = negative; break;
		default: break;
		}
	}
	if (increment)
		++retained;
	if (retained == 0)
		return {
			zero_bits(value, negative),
			alternative ? 0U :
				(inexact ? fpsr_underflow | fpsr_inexact : 0U),
		};

	int rounded_highest = 63 - static_cast<int>(
		std::countl_zero(retained));
	int rounded_exponent = rounded_highest + target_unit;
	if (rounded_exponent >= minimum_normal &&
	    rounded_highest > static_cast<int>(value.fraction_bits)) {
		retained >>= rounded_highest -
			static_cast<int>(value.fraction_bits);
		rounded_highest = static_cast<int>(value.fraction_bits);
	}
	const auto biased_exponent = rounded_exponent + value.bias;
	if (biased_exponent >=
	    static_cast<int>(maximum_exponent(value))) {
		const bool to_infinity = rounding == 0U ||
			(rounding == 1U && !negative) ||
			(rounding == 2U && negative);
		return {
			to_infinity ? infinity_bits(value, negative) :
				maximum_finite_bits(value, negative),
			alternative ? 0U : fpsr_overflow | fpsr_inexact,
		};
	}

	std::uint64_t result;
	if (rounded_exponent >= minimum_normal) {
		result = zero_bits(value, negative) |
			(static_cast<std::uint64_t>(biased_exponent) <<
				value.fraction_bits) |
			(retained & fraction_mask(value));
	} else {
		const auto fraction = retained;
		if (alternative && output_flush && fraction != 0U)
			return {zero_bits(value, negative), 0U};
		result = zero_bits(value, negative) | fraction;
	}
	std::uint32_t flags = 0;
	if (!alternative && inexact) {
		flags |= fpsr_inexact;
		if (real_exponent < minimum_normal)
			flags |= fpsr_underflow;
	}
	return {result, flags};
}

[[nodiscard]] inline LaneResult reciprocal_step(std::uint64_t first_raw,
	std::uint64_t second_raw, const Format &value, std::uint32_t fpcr,
	bool square_root)
{
	const bool alternative = (fpcr & fpcr_ah) != 0U;
	const auto raw_mask = low_mask(value.width);
	const auto first_unnegated = first_raw & raw_mask;
	const auto first_fraction =
		first_unnegated & fraction_mask(value);
	const auto first_exponent =
		(first_unnegated & exponent_mask(value)) >>
			value.fraction_bits;
	const bool first_nan =
		first_exponent == maximum_exponent(value) &&
		first_fraction != 0U;
	const auto first_negated = alternative && first_nan ?
		first_unnegated : first_unnegated ^ sign_mask(value);
	auto first = unpack(first_negated, value, fpcr, alternative);
	auto second = unpack(second_raw, value, fpcr, alternative);
	std::uint32_t flags = first.flags | second.flags;
	if (is_nan(first.type) || is_nan(second.type)) {
		auto result = process_binary_nan(first, second, value, fpcr,
			alternative);
		result.flags |= flags;
		return result;
	}

	const bool first_infinity = first.type == ValueClass::infinity;
	const bool second_infinity = second.type == ValueClass::infinity;
	const bool first_zero = first.type == ValueClass::zero;
	const bool second_zero = second.type == ValueClass::zero;
	if ((first_infinity && second_zero) ||
	    (first_zero && second_infinity))
		return {
			square_root ?
				normal_bits(value,
					static_cast<unsigned>(value.bias),
					std::uint64_t{1} <<
						(value.fraction_bits - 1U)) :
				normal_bits(value,
					static_cast<unsigned>(value.bias + 1), 0U),
			flags,
		};
	if (first_infinity || second_infinity)
		return {
			infinity_bits(value,
				first.negative != second.negative),
			flags,
		};

	const unsigned __int128 product =
		static_cast<unsigned __int128>(first.significand) *
		second.significand;
	const bool product_negative =
		first.negative != second.negative;
	const int product_exponent = first.exponent + second.exponent;
	const int common_exponent = std::min(product_exponent, 0);
	const auto product_value = shifted_value(product,
		static_cast<unsigned>(product_exponent - common_exponent));
	const auto constant_value = shifted_value(
		static_cast<unsigned __int128>(square_root ? 3U : 2U),
		static_cast<unsigned>(-common_exponent));
	BigUnsigned exact;
	bool exact_negative = false;
	if (!product_negative) {
		exact = add(product_value, constant_value);
	} else {
		const int order = compare(constant_value, product_value);
		if (order >= 0)
			exact = subtract(constant_value, product_value);
		else {
			exact = subtract(product_value, constant_value);
			exact_negative = true;
		}
	}
	auto result = round_exact(exact, exact_negative,
		common_exponent - (square_root ? 1 : 0), value, fpcr);
	result.flags |= flags;
	return result;
}

} // namespace arm64_floating_reciprocal_test
