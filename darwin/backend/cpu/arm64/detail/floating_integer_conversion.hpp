// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "floating_conversion.hpp"

#include <bit>
#include <cstdint>

namespace uml::backend::arm64::detail {

enum class FloatingIntegerRounding : std::uint8_t {
	tie_even,
	plus_infinity,
	minus_infinity,
	zero,
	tie_away,
};

struct FloatingIntegerConversion final {
	std::uint64_t value;
	std::uint32_t exceptions;
};

[[nodiscard]] constexpr FloatingPrecisionConversion
convert_integer_to_floating_bits(std::uint64_t input, unsigned integer_width,
	unsigned destination_width, unsigned fractional_bits,
	bool unsigned_source, std::uint32_t fpcr) noexcept
{
	using namespace floating_conversion_detail;
	const auto integer_mask = low_mask(integer_width);
	input &= integer_mask;
	const bool sign = !unsigned_source &&
		(input >> (integer_width - 1U)) != 0U;
	const std::uint64_t magnitude = sign ?
		(~input + 1U) & integer_mask : input;
	if (magnitude == 0U)
		return {0U, 0U};

	std::uint32_t exceptions{};
	const auto destination = format(destination_width);
	const auto sign_bit = static_cast<std::uint64_t>(sign) <<
		(destination_width - 1U);
	const bool alternative_half = destination_width == 16U &&
		(fpcr & (1U << 26)) != 0U;
	const unsigned rounding = (fpcr >> 22) & 3U;
	const int highest = static_cast<int>(std::bit_width(magnitude)) - 1;
	const int value_exponent = highest - static_cast<int>(fractional_bits);
	const int minimum_exponent = 1 - destination.bias;
	const auto exponent_ones = low_mask(destination.exponent_bits);
	std::uint64_t result{};
	if (value_exponent >= minimum_exponent) {
		auto rounded = round_shift_right(magnitude,
			highest - static_cast<int>(destination.fraction_bits),
			rounding, sign);
		int biased_exponent = value_exponent + destination.bias;
		if (rounded.value ==
		    (std::uint64_t{1} << (destination.fraction_bits + 1U))) {
			rounded.value >>= 1U;
			++biased_exponent;
		}
		const auto overflow_limit = alternative_half ? exponent_ones + 1U :
			exponent_ones;
		if (biased_exponent >= static_cast<int>(overflow_limit)) {
			if (alternative_half) {
				exceptions |= 1U;
				result = sign_bit | low_mask(destination_width - 1U);
			} else {
				const bool to_infinity = rounding == 0U ||
					(rounding == 1U && !sign) ||
					(rounding == 2U && sign);
				result = to_infinity ?
					infinity(destination_width, sign) :
					max_normal(destination_width, sign);
				exceptions |= (1U << 2) | (1U << 4);
			}
		} else {
			result = sign_bit |
				(static_cast<std::uint64_t>(biased_exponent) <<
					destination.fraction_bits) |
				(rounded.value & low_mask(destination.fraction_bits));
			if (rounded.inexact)
				exceptions |= 1U << 4;
		}
	} else {
		const bool ah = (fpcr & 2U) != 0U;
		if (!ah && destination_width != 16U &&
		    (fpcr & (1U << 24)) != 0U) {
			exceptions |= 1U << 3;
			result = sign_bit;
		} else {
			const int unit_exponent = minimum_exponent -
				static_cast<int>(destination.fraction_bits);
			const auto rounded = round_shift_right(magnitude,
				unit_exponent + static_cast<int>(fractional_bits),
				rounding, sign);
			const bool became_normal = rounded.value >=
				(std::uint64_t{1} << destination.fraction_bits);
			if (ah && !became_normal && destination_width != 16U &&
			    (fpcr & (1U << 24)) != 0U) {
				exceptions |= (1U << 3) | (1U << 4);
				result = sign_bit;
			} else {
				result = sign_bit | (became_normal ?
					(std::uint64_t{1} << destination.fraction_bits) :
					rounded.value);
				if (rounded.inexact) {
					exceptions |= 1U << 4;
					if (!ah || !became_normal)
						exceptions |= 1U << 3;
				}
			}
		}
	}
	return {result, exceptions};
}

struct FloatingJavaScriptConversion final {
	std::uint32_t value;
	std::uint32_t exceptions;
	bool sets_z;
};

[[nodiscard]] constexpr FloatingJavaScriptConversion
convert_floating_to_javascript_integer(std::uint64_t input,
	std::uint32_t fpcr) noexcept
{
	using namespace floating_conversion_detail;
	std::uint32_t exceptions{};
	const auto source = unpack(input, 64U, fpcr, exceptions);
	const auto input_denormal_exception = [&]() constexpr {
		if (source.type == Class::denormal && (fpcr & 2U) != 0U)
			exceptions |= 1U << 7;
	};
	if (source.type == Class::quiet_nan ||
	    source.type == Class::signaling_nan ||
	    source.type == Class::infinity) {
		exceptions |= 1U;
		return {0U, exceptions, false};
	}
	if (source.type == Class::zero)
		return {0U, exceptions, !source.sign};

	std::uint64_t magnitude{};
	bool inexact{};
	bool outside_signed_range{};
	if (source.exponent >= 0) {
		const unsigned shift = static_cast<unsigned>(source.exponent);
		magnitude = shift >= 32U ? 0U :
			(source.significand << shift) & 0xffff'ffffU;
		const unsigned bits = std::bit_width(source.significand) + shift;
		outside_signed_range = source.sign ? bits > 32U :
			bits >= 32U;
		if (source.sign && bits == 32U && shift < 64U)
			outside_signed_range =
				(source.significand << shift) > 0x8000'0000ULL;
	} else {
		const unsigned shift = static_cast<unsigned>(-source.exponent);
		magnitude = shift < 64U ? source.significand >> shift : 0U;
		const auto remainder = shift < 64U ?
			source.significand & low_mask(shift) : source.significand;
		inexact = remainder != 0U;
		outside_signed_range = source.sign ?
			magnitude > 0x8000'0000ULL : magnitude > 0x7fff'ffffULL;
	}
	const auto value = static_cast<std::uint32_t>(source.sign ?
		~magnitude + 1U : magnitude);
	input_denormal_exception();
	if (outside_signed_range)
		exceptions |= 1U;
	else if (inexact)
		exceptions |= 1U << 4;
	return {value, exceptions, !outside_signed_range && !inexact};
}

[[nodiscard]] constexpr FloatingIntegerConversion convert_floating_to_integer_bits(
	std::uint64_t input, unsigned source_width, unsigned integer_width,
	unsigned fractional_bits, bool unsigned_result,
	FloatingIntegerRounding rounding, std::uint32_t fpcr) noexcept
{
	using namespace floating_conversion_detail;
	std::uint32_t exceptions{};
	const auto source = unpack(input, source_width, fpcr, exceptions);
	const auto input_denormal_exception = [&]() constexpr {
		if (source_width != 16U && source.type == Class::denormal &&
		    (fpcr & 2U) != 0U)
			exceptions |= 1U << 7;
	};
	const auto invalid = [&]() constexpr {
		input_denormal_exception();
		exceptions |= 1U;
		if (unsigned_result)
			return FloatingIntegerConversion{
				source.sign ? 0U : low_mask(integer_width), exceptions};
		return FloatingIntegerConversion{
			source.sign ? std::uint64_t{1} << (integer_width - 1U) :
				low_mask(integer_width - 1U), exceptions};
	};
	if (source.type == Class::quiet_nan ||
	    source.type == Class::signaling_nan) {
		exceptions |= 1U;
		return {0U, exceptions};
	}
	if (source.type == Class::infinity)
		return invalid();
	if (source.type == Class::zero)
		return {0U, exceptions};

	const int exponent = source.exponent + static_cast<int>(fractional_bits);
	std::uint64_t magnitude{};
	bool overflow{};
	bool inexact{};
	if (exponent >= 0) {
		const unsigned shift = static_cast<unsigned>(exponent);
		overflow = shift >= 64U ||
			std::bit_width(source.significand) + shift > 64U;
		if (!overflow)
			magnitude = source.significand << shift;
	} else {
		const unsigned shift = static_cast<unsigned>(-exponent);
		const std::uint64_t truncated = shift < 64U ?
			source.significand >> shift : 0U;
		const std::uint64_t remainder = shift < 64U ?
			source.significand & low_mask(shift) : source.significand;
		magnitude = truncated;
		inexact = remainder != 0U;
		if (inexact) {
			bool increment{};
			if (rounding == FloatingIntegerRounding::plus_infinity)
				increment = !source.sign;
			else if (rounding == FloatingIntegerRounding::minus_infinity)
				increment = source.sign;
			else if ((rounding == FloatingIntegerRounding::tie_even ||
			          rounding == FloatingIntegerRounding::tie_away) &&
			         shift < 64U) {
				const auto halfway = std::uint64_t{1} << (shift - 1U);
				increment = remainder > halfway ||
					(remainder == halfway &&
					 (rounding == FloatingIntegerRounding::tie_away ||
					  (truncated & 1U) != 0U));
			}
			magnitude += static_cast<unsigned>(increment);
		}
	}

	const auto positive_limit = unsigned_result ? low_mask(integer_width) :
		low_mask(integer_width - 1U);
	const auto negative_limit = unsigned_result ? 0U :
		std::uint64_t{1} << (integer_width - 1U);
	if (overflow || (source.sign ? magnitude > negative_limit :
		magnitude > positive_limit) ||
	    (unsigned_result && source.sign && magnitude != 0U))
		return invalid();
	input_denormal_exception();
	if (inexact)
		exceptions |= 1U << 4;
	const auto value = source.sign && magnitude != 0U ?
		(~magnitude + 1U) & low_mask(integer_width) : magnitude;
	return {value, exceptions};
}

struct FloatingToIntegerDecode final {
	bool recognized{};
	bool valid{};
	bool destination_gpr{};
	bool unsigned_result{};
	bool scalar{};
	FloatingIntegerRounding rounding{};
	unsigned source_width{};
	unsigned integer_width{};
	unsigned vector_bytes{};
	unsigned lanes{};
	unsigned shape{};
	bool fixed_point{};
	unsigned fractional_bits{};
};

struct IntegerToFloatingDecode final {
	bool recognized{};
	bool valid{};
	bool source_gpr{};
	bool unsigned_source{};
	bool scalar{};
	bool fixed_point{};
	unsigned integer_width{};
	unsigned destination_width{};
	unsigned vector_bytes{};
	unsigned lanes{};
	unsigned shape{};
	unsigned fractional_bits{};
};

[[nodiscard]] constexpr IntegerToFloatingDecode decode_integer_to_floating(
	std::uint32_t instruction) noexcept
{
	const unsigned rmode = (instruction >> 19) & 3U;
	const unsigned opcode = (instruction >> 16) & 7U;
	const bool fixed = (instruction & 0x0020'0000U) == 0U;
	if ((instruction & 0x7f00'0000U) == 0x1e00'0000U &&
	    rmode == 0U && (opcode == 2U || opcode == 3U) &&
	    (fixed || (instruction & 0x0000'fc00U) == 0U)) {
		const unsigned type = (instruction >> 22) & 3U;
		const unsigned destination_width =
			type == 3U ? 16U : type == 0U ? 32U : 64U;
		const unsigned integer_width =
			(instruction & 0x8000'0000U) != 0U ? 64U : 32U;
		const unsigned fractional_bits = fixed ?
			64U - ((instruction >> 10) & 63U) : 0U;
		const bool fixed_fields_valid =
			!fixed || fractional_bits <= integer_width;
		return {true, type != 2U && fixed_fields_valid, true,
			opcode == 3U, true, fixed, integer_width, destination_width,
			destination_width / 8U, 1U,
			destination_width == 16U ? 0U :
				destination_width == 32U ? 1U : 2U,
			fractional_bits};
	}

	/* FEAT_FPRCVT cross-width SIMD&FP scalar forms. */
	if ((instruction & 0x7f20'fc00U) == 0x1e20'0000U &&
	    rmode == 3U && (opcode == 4U || opcode == 5U)) {
		const unsigned type = (instruction >> 22) & 3U;
		const unsigned destination_width =
			type == 3U ? 16U : type == 0U ? 32U : 64U;
		const unsigned integer_width =
			(instruction & 0x8000'0000U) != 0U ? 64U : 32U;
		return {true, type != 2U && destination_width != integer_width,
			false, opcode == 5U, true, false, integer_width,
			destination_width, destination_width / 8U, 1U,
			destination_width == 16U ? 0U :
				destination_width == 32U ? 1U : 2U, 0U};
	}

	IntegerToFloatingDecode decoded{};
	const auto match_fixed = [&](bool unsigned_source, bool scalar) constexpr {
		if (decoded.recognized)
			return;
		const std::uint32_t mask = scalar ? 0xff00'fc00U : 0xbf00'fc00U;
		const std::uint32_t value = (scalar ? 0x5f00'e400U :
			0x0f00'e400U) | (unsigned_source ? 0x2000'0000U : 0U);
		if ((instruction & mask) != value)
			return;
		const unsigned immediate = (instruction >> 16) & 127U;
		/* immh == 0000 is not part of the vector fixed-point class: the
		 * same words belong to AdvSIMD modified-immediate (MOVI/MVNI).
		 * Scalar encodings have no such overlap and remain reserved. */
		if (!scalar && immediate < 8U)
			return;
		const unsigned width = immediate >= 64U ? 64U :
			immediate >= 32U ? 32U : 16U;
		const unsigned bytes = scalar ? width / 8U :
			(instruction & 0x4000'0000U) != 0U ? 16U : 8U;
		const unsigned shape = scalar ? (width == 16U ? 0U :
			width == 32U ? 1U : 2U) : width == 16U ?
			((instruction & 0x4000'0000U) != 0U ? 4U : 3U) :
			width == 32U ?
			((instruction & 0x4000'0000U) != 0U ? 6U : 5U) : 7U;
		decoded = {true, immediate >= 16U &&
			(width != 64U || scalar ||
			 (instruction & 0x4000'0000U) != 0U),
			false, unsigned_source, scalar, true, width, width, bytes,
			bytes / (width / 8U), shape, 2U * width - immediate};
	};
	match_fixed(false, true);
	match_fixed(true, true);
	match_fixed(false, false);
	match_fixed(true, false);
	if (decoded.recognized)
		return decoded;

	const auto match = [&](std::uint32_t mask, std::uint32_t value,
		bool unsigned_source, bool scalar, unsigned width,
		unsigned shape) constexpr {
		if (decoded.recognized || (instruction & mask) != value)
			return;
		const unsigned bytes = scalar ? width / 8U :
			(instruction & 0x4000'0000U) != 0U ? 16U : 8U;
		decoded = {true, true, false, unsigned_source, scalar, false,
			width, width, bytes, bytes / (width / 8U), shape, 0U};
	};
#define UML_MATCH_SIMD_CVTF(unsigned_source, half_base, ordinary_base) \
	match(0xffff'fc00U, half_base | 0x5000'0000U, unsigned_source, true, 16U, 0U); \
	match(0xffbf'fc00U, ordinary_base | 0x5000'0000U, unsigned_source, true, \
		(instruction & 0x0040'0000U) != 0U ? 64U : 32U, \
		(instruction & 0x0040'0000U) != 0U ? 2U : 1U); \
	match(0xbfff'fc00U, half_base, unsigned_source, false, 16U, \
		(instruction & 0x4000'0000U) != 0U ? 4U : 3U); \
	match(0xbfbf'fc00U, ordinary_base, unsigned_source, false, \
		(instruction & 0x0040'0000U) != 0U ? 64U : 32U, \
		(instruction & 0x0040'0000U) != 0U ? 7U : \
			(instruction & 0x4000'0000U) != 0U ? 6U : 5U)
	UML_MATCH_SIMD_CVTF(false, 0x0e79'd800U, 0x0e21'd800U);
	UML_MATCH_SIMD_CVTF(true, 0x2e79'd800U, 0x2e21'd800U);
#undef UML_MATCH_SIMD_CVTF
	if (decoded.recognized && !decoded.scalar &&
	    decoded.integer_width == 64U && decoded.vector_bytes != 16U)
		decoded.valid = false;
	return decoded;
}

[[nodiscard]] constexpr std::uintptr_t integer_to_floating_operand(
	std::uint32_t instruction, const IntegerToFloatingDecode &decoded) noexcept
{
	const unsigned destination_code = decoded.destination_width == 16U ? 0U :
		decoded.destination_width == 32U ? 1U : 2U;
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
		(static_cast<std::uintptr_t>(decoded.shape) << 16) |
		(static_cast<std::uintptr_t>(decoded.unsigned_source) << 20) |
		(static_cast<std::uintptr_t>(decoded.source_gpr) << 21) |
		(static_cast<std::uintptr_t>(decoded.integer_width == 64U) << 22) |
		(static_cast<std::uintptr_t>(decoded.scalar) << 23) |
		(static_cast<std::uintptr_t>(destination_code) << 24) |
		(static_cast<std::uintptr_t>(decoded.fractional_bits) << 26) |
		(static_cast<std::uintptr_t>(decoded.fixed_point) << 33) |
		(static_cast<std::uintptr_t>(
			!decoded.source_gpr &&
			decoded.integer_width != decoded.destination_width) << 34);
}

[[nodiscard]] constexpr FloatingToIntegerDecode decode_floating_to_integer(
	std::uint32_t instruction) noexcept
{
	/* Scalar fixed-point FCVTZS/FCVTZU to a general-purpose register. */
	if ((instruction & 0x7f20'0000U) == 0x1e00'0000U &&
	    ((instruction >> 19) & 3U) == 3U &&
	    ((instruction >> 16) & 7U) <= 1U) {
		const unsigned type = (instruction >> 22) & 3U;
		const unsigned width = type == 3U ? 16U : type == 0U ? 32U : 64U;
		const unsigned integer_width =
			(instruction & 0x8000'0000U) != 0U ? 64U : 32U;
		const unsigned fractional_bits =
			64U - ((instruction >> 10) & 63U);
		return {true, type != 2U && fractional_bits <= integer_width,
			true, (instruction & 0x0001'0000U) != 0U, true,
			FloatingIntegerRounding::zero, width, integer_width,
			width / 8U, 1U,
			width == 16U ? 0U : width == 32U ? 1U : 2U,
			true, fractional_bits};
	}

	/* Scalar FP-to-GPR conversions and FEAT_FPRCVT cross-width conversions
	 * share this encoding class. Classify the exact opcode/rmode pairs here so
	 * malformed encodings are rejected before any architectural state read. */
	if ((instruction & 0x7f20'fc00U) == 0x1e20'0000U) {
		const unsigned type = (instruction >> 22) & 3U;
		const unsigned rmode = (instruction >> 19) & 3U;
		const unsigned opcode = (instruction >> 16) & 7U;
		const auto fixed_encoding = instruction & 0xffff'fc00U;
		const bool general_move =
			fixed_encoding == 0x1ee6'0000U ||
			fixed_encoding == 0x9ee6'0000U ||
			fixed_encoding == 0x1ee7'0000U ||
			fixed_encoding == 0x9ee7'0000U ||
			fixed_encoding == 0x9e66'0000U ||
			fixed_encoding == 0x9e67'0000U ||
			fixed_encoding == 0x1e26'0000U ||
			fixed_encoding == 0x1e27'0000U ||
			fixed_encoding == 0x9eae'0000U ||
			fixed_encoding == 0x9eaf'0000U;
		bool destination_gpr{};
		bool recognized{};
		bool opcode_valid{true};
		FloatingIntegerRounding rounding{};
		if (opcode <= 1U) {
			destination_gpr = true;
			recognized = true;
			rounding = rmode == 0U ? FloatingIntegerRounding::tie_even :
				rmode == 1U ? FloatingIntegerRounding::plus_infinity :
				rmode == 2U ? FloatingIntegerRounding::minus_infinity :
					FloatingIntegerRounding::zero;
		} else if ((opcode == 4U || opcode == 5U) && rmode == 0U) {
			destination_gpr = true;
			recognized = true;
			rounding = FloatingIntegerRounding::tie_away;
		} else if ((opcode == 2U || opcode == 3U) && rmode != 0U) {
			recognized = true;
			rounding = rmode == 1U ? FloatingIntegerRounding::tie_even :
				rmode == 2U ? FloatingIntegerRounding::plus_infinity :
					FloatingIntegerRounding::tie_away;
		} else if ((opcode == 4U || opcode == 5U) && rmode == 2U) {
			recognized = true;
			rounding = FloatingIntegerRounding::minus_infinity;
		} else if ((opcode == 6U || opcode == 7U) && rmode == 2U) {
			recognized = true;
			rounding = FloatingIntegerRounding::zero;
		} else if (opcode >= 4U && !general_move) {
			/* Remaining opcode 100..111 combinations are reserved members
			 * of the FP-to-integer encoding space, not integer-to-FP. */
			recognized = true;
			opcode_valid = false;
		}
		if (!recognized)
			return {};
		const unsigned width = type == 3U ? 16U : type == 0U ? 32U : 64U;
		const unsigned integer_width =
			(instruction & 0x8000'0000U) != 0U ? 64U : 32U;
		const bool same_width_fpr = !destination_gpr && width == integer_width;
		return {true, opcode_valid && type != 2U && !same_width_fpr,
			destination_gpr,
			(instruction & 0x0001'0000U) != 0U, true, rounding, width,
			integer_width, integer_width / 8U, 1U,
			width == 16U ? 0U : width == 32U ? 1U : 2U};
	}

	FloatingToIntegerDecode decoded{};
	const auto match_fixed = [&](bool unsigned_result, bool scalar) constexpr {
		if (decoded.recognized)
			return;
		const std::uint32_t mask = scalar ? 0xff00'fc00U : 0xbf00'fc00U;
		const std::uint32_t value = (scalar ? 0x5f00'fc00U :
			0x0f00'fc00U) | (unsigned_result ? 0x2000'0000U : 0U);
		if ((instruction & mask) != value)
			return;
		const unsigned immediate = (instruction >> 16) & 127U;
		/* See decode_integer_to_floating(): immh == 0000 in the vector
		 * encoding is the modified-immediate allocation. */
		if (!scalar && immediate < 8U)
			return;
		const unsigned width = immediate >= 64U ? 64U :
			immediate >= 32U ? 32U : 16U;
		const unsigned bytes = scalar ? width / 8U :
			(instruction & 0x4000'0000U) != 0U ? 16U : 8U;
		const unsigned shape = scalar ? (width == 16U ? 0U :
			width == 32U ? 1U : 2U) : width == 16U ?
			((instruction & 0x4000'0000U) != 0U ? 4U : 3U) :
			width == 32U ?
			((instruction & 0x4000'0000U) != 0U ? 6U : 5U) : 7U;
		decoded = {true, immediate >= 16U &&
			(width != 64U || scalar ||
			 (instruction & 0x4000'0000U) != 0U),
			false, unsigned_result, scalar, FloatingIntegerRounding::zero,
			width, width, bytes, bytes / (width / 8U), shape, true,
			2U * width - immediate};
	};
	match_fixed(false, true);
	match_fixed(true, true);
	match_fixed(false, false);
	match_fixed(true, false);
	if (decoded.recognized)
		return decoded;

	const auto match = [&](std::uint32_t mask, std::uint32_t value,
		bool unsigned_result, FloatingIntegerRounding rounding,
		bool scalar, unsigned width,
		unsigned shape) constexpr {
		if (decoded.recognized || (instruction & mask) != value)
			return;
		const unsigned bytes = scalar ? width / 8U :
			(instruction & 0x4000'0000U) != 0U ? 16U : 8U;
		decoded = {true, true, false, unsigned_result, scalar, rounding,
			width, width, bytes, bytes / (width / 8U), shape};
	};
#define UML_MATCH_SIMD_FCVT(unsigned_result, rounding, half_base, ordinary_base) \
	match(0xffff'fc00U, half_base | 0x5000'0000U, unsigned_result, rounding, true, 16U, 0U); \
	match(0xffbf'fc00U, ordinary_base | 0x5000'0000U, unsigned_result, rounding, true, \
		(instruction & 0x0040'0000U) != 0U ? 64U : 32U, \
		(instruction & 0x0040'0000U) != 0U ? 2U : 1U); \
	match(0xbfff'fc00U, half_base, unsigned_result, rounding, false, 16U, \
		(instruction & 0x4000'0000U) != 0U ? 4U : 3U); \
	match(0xbfbf'fc00U, ordinary_base, unsigned_result, rounding, false, \
		(instruction & 0x0040'0000U) != 0U ? 64U : 32U, \
		(instruction & 0x0040'0000U) != 0U ? 7U : \
			(instruction & 0x4000'0000U) != 0U ? 6U : 5U)
	UML_MATCH_SIMD_FCVT(false, FloatingIntegerRounding::tie_even,
		0x0e79'a800U, 0x0e21'a800U);
	UML_MATCH_SIMD_FCVT(true, FloatingIntegerRounding::tie_even,
		0x2e79'a800U, 0x2e21'a800U);
	UML_MATCH_SIMD_FCVT(false, FloatingIntegerRounding::plus_infinity,
		0x0ef9'a800U, 0x0ea1'a800U);
	UML_MATCH_SIMD_FCVT(true, FloatingIntegerRounding::plus_infinity,
		0x2ef9'a800U, 0x2ea1'a800U);
	UML_MATCH_SIMD_FCVT(false, FloatingIntegerRounding::minus_infinity,
		0x0e79'b800U, 0x0e21'b800U);
	UML_MATCH_SIMD_FCVT(true, FloatingIntegerRounding::minus_infinity,
		0x2e79'b800U, 0x2e21'b800U);
	UML_MATCH_SIMD_FCVT(false, FloatingIntegerRounding::zero,
		0x0ef9'b800U, 0x0ea1'b800U);
	UML_MATCH_SIMD_FCVT(true, FloatingIntegerRounding::zero,
		0x2ef9'b800U, 0x2ea1'b800U);
	UML_MATCH_SIMD_FCVT(false, FloatingIntegerRounding::tie_away,
		0x0e79'c800U, 0x0e21'c800U);
	UML_MATCH_SIMD_FCVT(true, FloatingIntegerRounding::tie_away,
		0x2e79'c800U, 0x2e21'c800U);
#undef UML_MATCH_SIMD_FCVT
	if (decoded.recognized && !decoded.scalar && decoded.source_width == 64U &&
	    decoded.vector_bytes != 16U)
		decoded.valid = false;
	return decoded;
}

[[nodiscard]] constexpr std::uintptr_t floating_to_integer_operand(
	std::uint32_t instruction, const FloatingToIntegerDecode &decoded) noexcept
{
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
		(static_cast<std::uintptr_t>(decoded.shape) << 16) |
		(static_cast<std::uintptr_t>(decoded.unsigned_result) << 20) |
		(static_cast<std::uintptr_t>(decoded.destination_gpr) << 21) |
		(static_cast<std::uintptr_t>(decoded.integer_width == 64U) << 22) |
		(static_cast<std::uintptr_t>(decoded.scalar) << 23) |
		(static_cast<std::uintptr_t>(decoded.rounding) << 24) |
		(static_cast<std::uintptr_t>(
			!decoded.destination_gpr &&
			decoded.source_width != decoded.integer_width) << 27) |
		(static_cast<std::uintptr_t>(decoded.fractional_bits) << 28) |
		(static_cast<std::uintptr_t>(decoded.fixed_point) << 35);
}

} // namespace uml::backend::arm64::detail
