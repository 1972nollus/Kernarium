// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::backend::arm64::detail {

enum class Fp8Operation : std::uint8_t {
	convert_to_half,
	convert_to_bfloat,
	convert_from_half,
	convert_from_single,
	dot_half,
	dot_single,
	multiply_half,
	multiply_single,
	matrix_half,
	matrix_single,
};

struct Fp8Instruction final {
	bool recognized{};
	bool valid{};
	Fp8Operation operation{};
	bool upper{};
	bool source2{};
	bool by_element{};
	unsigned lanes{};
	unsigned selection{};
	unsigned index{};
	unsigned destination{};
	unsigned first{};
	unsigned second{};
};

[[nodiscard]] constexpr Fp8Instruction decode_fp8(
	std::uint32_t instruction) noexcept
{
	Fp8Instruction result{};
	result.destination = instruction & 31U;
	result.first = (instruction >> 5) & 31U;
	result.second = (instruction >> 16) & 31U;
	const bool q = (instruction & 0x4000'0000U) != 0;

	/* FP8/BFloat16 convert long.  All four size values are allocated here. */
	if ((instruction & 0xbf3f'fc00U) == 0x2e21'7800U) {
		const unsigned size = (instruction >> 22) & 3U;
		result.recognized = result.valid = true;
		result.operation = size >= 2U ? Fp8Operation::convert_to_bfloat :
			Fp8Operation::convert_to_half;
		result.source2 = (size & 1U) != 0;
		result.upper = q;
		return result;
	}
	/* Two-source half/single narrow forms. */
	if ((instruction & 0xbfe0'fc00U) == 0x0e40'f400U ||
	    (instruction & 0xbfe0'fc00U) == 0x0e00'f400U) {
		result.recognized = result.valid = true;
		result.operation = (instruction & 0x0040'0000U) != 0 ?
			Fp8Operation::convert_from_half :
			Fp8Operation::convert_from_single;
		result.upper = q;
		result.lanes = result.operation == Fp8Operation::convert_from_half ?
			(q ? 8U : 4U) : 4U;
		return result;
	}
	/* Indexed dot products. */
	if ((instruction & 0xbfc0'f400U) == 0x0f40'0000U ||
	    (instruction & 0xbfc0'f400U) == 0x0f00'0000U) {
		const bool half = (instruction & 0x0040'0000U) != 0;
		result.recognized = result.valid = true;
		result.operation = half ? Fp8Operation::dot_half :
			Fp8Operation::dot_single;
		result.by_element = true;
		result.lanes = half ? (q ? 8U : 4U) : (q ? 4U : 2U);
		result.index = half ? (((instruction >> 11) & 1U) << 2) |
			(((instruction >> 21) & 1U) << 1) |
			((instruction >> 20) & 1U) :
			(((instruction >> 11) & 1U) << 1) |
			((instruction >> 21) & 1U);
		result.second = half ? ((instruction >> 16) & 15U) :
			(((instruction >> 20) & 1U) << 4) |
			((instruction >> 16) & 15U);
		return result;
	}
	/* Vector dot products. */
	if ((instruction & 0xbfe0'fc00U) == 0x0e40'fc00U ||
	    (instruction & 0xbfe0'fc00U) == 0x0e00'fc00U) {
		const bool half = (instruction & 0x0040'0000U) != 0;
		result.recognized = result.valid = true;
		result.operation = half ? Fp8Operation::dot_half :
			Fp8Operation::dot_single;
		result.lanes = half ? (q ? 8U : 4U) : (q ? 4U : 2U);
		return result;
	}
	/* FP8 to half, one selected byte from each 16-bit container. */
	if ((instruction & 0xbfc0'f400U) == 0x0fc0'0000U) {
		result.recognized = result.valid = true;
		result.operation = Fp8Operation::multiply_half;
		result.by_element = true;
		result.selection = q;
		result.index = (((instruction >> 11) & 1U) << 3) |
			(((instruction >> 21) & 1U) << 2) |
			(((instruction >> 20) & 1U) << 1) |
			((instruction >> 19) & 1U);
		result.second = (instruction >> 16) & 7U;
		return result;
	}
	if ((instruction & 0xbfe0'fc00U) == 0x0ec0'fc00U) {
		result.recognized = result.valid = true;
		result.operation = Fp8Operation::multiply_half;
		result.selection = q;
		return result;
	}
	/* FP8 to single, one selected byte from each 32-bit container. */
	if ((instruction & 0xbf80'f400U) == 0x2f00'8000U) {
		result.recognized = result.valid = true;
		result.operation = Fp8Operation::multiply_single;
		result.by_element = true;
		result.selection = (q ? 2U : 0U) | ((instruction >> 22) & 1U);
		result.index = (((instruction >> 11) & 1U) << 3) |
			(((instruction >> 21) & 1U) << 2) |
			(((instruction >> 20) & 1U) << 1) |
			((instruction >> 19) & 1U);
		result.second = (instruction >> 16) & 7U;
		return result;
	}
	if ((instruction & 0xbfa0'fc00U) == 0x0e00'c400U) {
		result.recognized = result.valid = true;
		result.operation = Fp8Operation::multiply_single;
		result.selection = (q ? 2U : 0U) | ((instruction >> 22) & 1U);
		return result;
	}
	/* Fixed-width FP8 matrix forms. */
	if ((instruction & 0xffe0'fc00U) == 0x6e00'ec00U ||
	    (instruction & 0xffe0'fc00U) == 0x6e80'ec00U) {
		result.recognized = result.valid = true;
		result.operation = (instruction & 0x0080'0000U) != 0 ?
			Fp8Operation::matrix_single : Fp8Operation::matrix_half;
		return result;
	}
	return result;
}

[[nodiscard]] constexpr std::uintptr_t fp8_operand(
	const Fp8Instruction &decoded) noexcept
{
	return decoded.destination |
		(static_cast<std::uintptr_t>(decoded.first) << 8) |
		(static_cast<std::uintptr_t>(decoded.second) << 16) |
		(static_cast<std::uintptr_t>(decoded.index) << 24);
}

enum class FpClass : std::uint8_t { zero, finite, infinity, qnan, snan };
enum class Fp8Format : std::uint8_t { e5m2, e4m3, unsupported };

struct ExactTerm final {
	FpClass type{};
	bool negative{};
	std::uint32_t significand{};
	int exponent{}; // value = significand * 2^exponent
};

[[nodiscard]] constexpr Fp8Format fp8_format(unsigned field) noexcept
{
	return field == 0U ? Fp8Format::e5m2 :
		field == 1U ? Fp8Format::e4m3 : Fp8Format::unsupported;
}

[[nodiscard]] constexpr Fp8Format fp8_source_format(
	std::uint64_t fpmr, bool second) noexcept
{
	return fp8_format(static_cast<unsigned>((fpmr >> (second ? 3U : 0U)) & 7U));
}

[[nodiscard]] constexpr Fp8Format fp8_destination_format(
	std::uint64_t fpmr) noexcept
{
	return fp8_format(static_cast<unsigned>((fpmr >> 6) & 7U));
}

[[nodiscard]] constexpr ExactTerm unpack_fp8(
	std::uint8_t bits, Fp8Format format) noexcept
{
	const bool negative = (bits & 0x80U) != 0;
	if (format == Fp8Format::unsupported)
		return {FpClass::snan, negative, 0, 0};
	const unsigned fraction_bits = format == Fp8Format::e4m3 ? 3U : 2U;
	const unsigned exponent_bits = 7U - fraction_bits;
	const unsigned fraction_mask = (1U << fraction_bits) - 1U;
	const unsigned exponent_mask = (1U << exponent_bits) - 1U;
	const unsigned fraction = bits & fraction_mask;
	const unsigned exponent = (bits >> fraction_bits) & exponent_mask;
	if (exponent == 0U) {
		if (fraction == 0U)
			return {FpClass::zero, negative, 0, 0};
		const int minimum = format == Fp8Format::e4m3 ? -6 : -14;
		return {FpClass::finite, negative, fraction,
			minimum - static_cast<int>(fraction_bits)};
	}
	if (format == Fp8Format::e4m3) {
		if (exponent == 15U && fraction == 7U)
			return {FpClass::snan, negative, 0, 0};
		return {FpClass::finite, negative,
			(1U << fraction_bits) | fraction,
			static_cast<int>(exponent) - 7 -
				static_cast<int>(fraction_bits)};
	}
	if (exponent == 31U) {
		if (fraction == 0U)
			return {FpClass::infinity, negative, 0, 0};
		return {fraction >= 2U ? FpClass::qnan : FpClass::snan,
			negative, 0, 0};
	}
	return {FpClass::finite, negative, 4U | fraction,
		static_cast<int>(exponent) - 15 - 2};
}

[[nodiscard]] constexpr ExactTerm unpack_binary(
	std::uint32_t bits, unsigned width) noexcept
{
	const unsigned fraction_bits = width == 16U ? 10U : 23U;
	const unsigned exponent_bits = width == 16U ? 5U : 8U;
	const std::uint32_t fraction_mask = (std::uint32_t{1} << fraction_bits) - 1U;
	const unsigned exponent_mask = (1U << exponent_bits) - 1U;
	const bool negative = ((bits >> (width - 1U)) & 1U) != 0;
	const unsigned fraction = bits & fraction_mask;
	const unsigned exponent = (bits >> fraction_bits) & exponent_mask;
	if (exponent == 0U) {
		if (fraction == 0U)
			return {FpClass::zero, negative, 0, 0};
		const int minimum = width == 16U ? -14 : -126;
		return {FpClass::finite, negative, fraction,
			minimum - static_cast<int>(fraction_bits)};
	}
	if (exponent == exponent_mask) {
		if (fraction == 0U)
			return {FpClass::infinity, negative, 0, 0};
		const bool quiet = (fraction & (std::uint32_t{1} <<
			(fraction_bits - 1U))) != 0;
		return {quiet ? FpClass::qnan : FpClass::snan,
			negative, 0, 0};
	}
	const int bias = width == 16U ? 15 : 127;
	return {FpClass::finite, negative,
		(std::uint32_t{1} << fraction_bits) | fraction,
		static_cast<int>(exponent) - bias -
			static_cast<int>(fraction_bits)};
}

class Magnitude final {
public:
	static constexpr unsigned limb_count = 8;

	[[nodiscard]] bool zero() const noexcept
	{
		for (const auto limb : limbs_)
			if (limb != 0)
				return false;
		return true;
	}

	[[nodiscard]] unsigned bit_length() const noexcept
	{
		for (unsigned i = limb_count; i-- != 0;)
			if (limbs_[i] != 0)
				return i * 64U + 64U - std::countl_zero(limbs_[i]);
		return 0;
	}

	[[nodiscard]] bool bit(unsigned index) const noexcept
	{
		return index < limb_count * 64U &&
			((limbs_[index / 64U] >> (index & 63U)) & 1U) != 0;
	}

	[[nodiscard]] bool any_below(unsigned count) const noexcept
	{
		const unsigned full = std::min(count / 64U, limb_count);
		for (unsigned i = 0; i < full; ++i)
			if (limbs_[i] != 0)
				return true;
		if (full < limb_count && (count & 63U) != 0) {
			const auto mask = (std::uint64_t{1} << (count & 63U)) - 1U;
			return (limbs_[full] & mask) != 0;
		}
		return false;
	}

	[[nodiscard]] std::uint64_t low_after_shift(unsigned shift) const noexcept
	{
		const unsigned word = shift / 64U;
		const unsigned bit_shift = shift & 63U;
		if (word >= limb_count)
			return 0;
		std::uint64_t result = limbs_[word] >> bit_shift;
		if (bit_shift != 0 && word + 1U < limb_count)
			result |= limbs_[word + 1U] << (64U - bit_shift);
		return result;
	}

	[[nodiscard]] static Magnitude shifted(
		std::uint32_t value, unsigned shift) noexcept
	{
		Magnitude result;
		const unsigned word = shift / 64U;
		const unsigned bit_shift = shift & 63U;
		if (word < limb_count)
			result.limbs_[word] = static_cast<std::uint64_t>(value) << bit_shift;
		if (bit_shift != 0 && word + 1U < limb_count)
			result.limbs_[word + 1U] =
				static_cast<std::uint64_t>(value) >> (64U - bit_shift);
		return result;
	}

	[[nodiscard]] int compare(const Magnitude &other) const noexcept
	{
		for (unsigned i = limb_count; i-- != 0;)
			if (limbs_[i] != other.limbs_[i])
				return limbs_[i] < other.limbs_[i] ? -1 : 1;
		return 0;
	}

	void add(const Magnitude &other) noexcept
	{
		std::uint64_t carry = 0;
		for (unsigned i = 0; i < limb_count; ++i) {
			const auto first = limbs_[i] + carry;
			const bool carry1 = first < limbs_[i];
			const auto sum = first + other.limbs_[i];
			const bool carry2 = sum < first;
			limbs_[i] = sum;
			carry = carry1 || carry2;
		}
	}

	void subtract(const Magnitude &other) noexcept
	{
		std::uint64_t borrow = 0;
		for (unsigned i = 0; i < limb_count; ++i) {
			const auto first = limbs_[i] - borrow;
			const bool borrow1 = limbs_[i] < borrow;
			const auto difference = first - other.limbs_[i];
			const bool borrow2 = first < other.limbs_[i];
			limbs_[i] = difference;
			borrow = borrow1 || borrow2;
		}
	}

private:
	std::array<std::uint64_t, limb_count> limbs_{};
};

struct SignedExact final {
	Magnitude magnitude{};
	bool negative{};

	void add(const ExactTerm &term, int common_exponent) noexcept
	{
		if (term.type != FpClass::finite || term.significand == 0)
			return;
		const auto operand = Magnitude::shifted(term.significand,
			static_cast<unsigned>(term.exponent - common_exponent));
		if (magnitude.zero()) {
			magnitude = operand;
			negative = term.negative;
			return;
		}
		if (negative == term.negative) {
			magnitude.add(operand);
			return;
		}
		const int order = magnitude.compare(operand);
		if (order >= 0)
			magnitude.subtract(operand);
		else {
			auto replacement = operand;
			replacement.subtract(magnitude);
			magnitude = replacement;
			negative = term.negative;
		}
	}
};

struct RoundResult final {
	std::uint32_t bits{};
	bool inexact{};
	bool pre_tiny{};
	bool post_tiny{};
	bool overflow{};
};

[[nodiscard]] inline std::uint64_t rounded_significand(
	const Magnitude &magnitude, int discard, bool &inexact) noexcept
{
	if (discard <= 0) {
		const auto value = magnitude.low_after_shift(0);
		return value << static_cast<unsigned>(-discard);
	}
	const unsigned removed = static_cast<unsigned>(discard);
	auto value = magnitude.low_after_shift(removed);
	const bool round = magnitude.bit(removed - 1U);
	const bool sticky = magnitude.any_below(removed - 1U);
	inexact = round || sticky;
	if (round && (sticky || (value & 1U) != 0))
		++value;
	return value;
}

[[nodiscard]] inline RoundResult round_binary(
	const Magnitude &magnitude, int common_exponent, unsigned fraction_bits,
	int minimum_exponent, int maximum_exponent, bool saturate) noexcept
{
	RoundResult result{};
	const unsigned precision = fraction_bits + 1U;
	const unsigned bits = magnitude.bit_length();
	if (bits == 0)
		return result;
	int exponent = common_exponent + static_cast<int>(bits) - 1;
	result.pre_tiny = exponent < minimum_exponent;
	std::uint64_t significand;
	if (exponent >= minimum_exponent) {
		const int discard = static_cast<int>(bits) -
			static_cast<int>(precision);
		significand = rounded_significand(magnitude, discard, result.inexact);
		if (significand == (std::uint64_t{1} << precision)) {
			significand >>= 1;
			++exponent;
		}
		if (exponent > maximum_exponent) {
			result.overflow = result.inexact = true;
			const unsigned exponent_bits = fraction_bits == 10U ? 5U : 8U;
			const std::uint32_t maximum_field =
				(1U << exponent_bits) - 1U;
			result.bits = saturate ?
				((maximum_field - 1U) << fraction_bits) |
					((1U << fraction_bits) - 1U) :
				maximum_field << fraction_bits;
			return result;
		}
		const unsigned bias = static_cast<unsigned>(1 - minimum_exponent);
		result.bits = (static_cast<unsigned>(exponent) + bias) <<
			fraction_bits;
		result.bits |= static_cast<std::uint32_t>(significand) &
			((1U << fraction_bits) - 1U);
		return result;
	}
	const int subnormal_unit = minimum_exponent -
		static_cast<int>(fraction_bits);
	const int discard = subnormal_unit - common_exponent;
	significand = rounded_significand(magnitude, discard, result.inexact);
	if (significand >= (std::uint64_t{1} << fraction_bits))
		result.bits = 1U << fraction_bits;
	else
		result.bits = static_cast<std::uint32_t>(significand);
	result.post_tiny = result.bits != 0 && result.bits < (1U << fraction_bits);
	return result;
}

struct Converted16 final {
	std::uint16_t bits{};
	std::uint32_t exceptions{};
};

[[nodiscard]] inline Converted16 fp8_to_binary(
	std::uint8_t input, bool source2, bool bfloat, std::uint32_t fpcr,
	std::uint64_t fpmr) noexcept
{
	const auto value = unpack_fp8(input, fp8_source_format(fpmr, source2));
	if (value.type == FpClass::snan || value.type == FpClass::qnan)
		return {static_cast<std::uint16_t>(bfloat ? 0x7fc0U : 0x7e00U),
			value.type == FpClass::snan ? 1U : 0U};
	if (value.type == FpClass::infinity)
		return {static_cast<std::uint16_t>((value.negative ? 0x8000U : 0U) |
			(bfloat ? 0x7f80U : 0x7c00U)), 0};
	if (value.type == FpClass::zero)
		return {static_cast<std::uint16_t>(value.negative ? 0x8000U : 0U), 0};
	const unsigned scale = source2 ?
		static_cast<unsigned>((fpmr >> 32) & (bfloat ? 63U : 15U)) :
		static_cast<unsigned>((fpmr >> 16) & (bfloat ? 63U : 15U));
	const auto magnitude = Magnitude::shifted(value.significand, 0);
	const auto rounded = round_binary(magnitude,
		value.exponent - static_cast<int>(scale), bfloat ? 7U : 10U,
		bfloat ? -126 : -14, bfloat ? 127 : 15, false);
	std::uint32_t exceptions = 0;
	if (rounded.overflow)
		exceptions |= 4U;
	if (rounded.inexact)
		exceptions |= 16U;
	const bool alternate = (fpcr & 2U) != 0;
	if (rounded.inexact && (alternate ? rounded.post_tiny : rounded.pre_tiny))
		exceptions |= 8U;
	return {static_cast<std::uint16_t>(rounded.bits |
		(value.negative ? 0x8000U : 0U)), exceptions};
}

struct Converted8 final {
	std::uint8_t bits{};
	std::uint32_t exceptions{};
};

[[nodiscard]] inline Converted8 binary_to_fp8(
	std::uint32_t input, unsigned width, std::uint32_t fpcr,
	std::uint64_t fpmr) noexcept
{
	const auto format = fp8_destination_format(fpmr);
	if (format == Fp8Format::unsupported)
		return {0xffU, 1U};
	const auto value = unpack_binary(input, width);
	const bool nan_sign = (fpcr & 2U) != 0;
	const std::uint8_t sign = value.negative ? 0x80U : 0U;
	const bool e4 = format == Fp8Format::e4m3;
	if (value.type == FpClass::snan || value.type == FpClass::qnan)
		return {static_cast<std::uint8_t>((nan_sign ? 0x80U : 0U) |
			(e4 ? 0x7fU : 0x7eU)),
			value.type == FpClass::snan ? 1U : 0U};
	const bool saturate = (fpmr & (std::uint64_t{1} << 15)) != 0;
	if (value.type == FpClass::infinity)
		return {static_cast<std::uint8_t>(sign |
			(e4 ? (saturate ? 0x7eU : 0x7fU) :
				(saturate ? 0x7bU : 0x7cU))), 0};
	if (value.type == FpClass::zero)
		return {sign, 0};
	int scale = static_cast<std::int8_t>((fpmr >> 24) & 0xffU);
	if (width == 16U) {
		const unsigned low = static_cast<unsigned>((fpmr >> 24) & 31U);
		scale = (low & 16U) != 0 ? static_cast<int>(low) - 32 :
			static_cast<int>(low);
	}
	const auto magnitude = Magnitude::shifted(value.significand, 0);
	const unsigned fraction_bits = e4 ? 3U : 2U;
	const int minimum = e4 ? -6 : -14;
	/* E4M3 has one extra finite exponent, but 0x7f is reserved as SNaN. */
	auto rounded = round_binary(magnitude, value.exponent + scale,
		fraction_bits, minimum, e4 ? 8 : 15, false);
	if (e4 && !rounded.overflow &&
	    (rounded.bits & 0x7fU) == 0x7fU)
		rounded.overflow = rounded.inexact = true;
	std::uint8_t result;
	if (rounded.overflow)
		result = static_cast<std::uint8_t>(sign |
			(e4 ? (saturate ? 0x7eU : 0x7fU) :
				(saturate ? 0x7bU : 0x7cU)));
	else
		result = static_cast<std::uint8_t>(sign | rounded.bits);
	std::uint32_t exceptions = 0;
	if (rounded.overflow)
		exceptions |= 4U;
	if (rounded.inexact)
		exceptions |= 16U;
	const bool alternate = (fpcr & 2U) != 0;
	if (rounded.inexact && (alternate ? rounded.post_tiny : rounded.pre_tiny))
		exceptions |= 8U;
	return {result, exceptions};
}

[[nodiscard]] constexpr std::uint32_t binary_default_nan(unsigned width) noexcept
{
	return width == 16U ? 0x7e00U : 0x7fc0'0000U;
}

[[nodiscard]] constexpr std::uint32_t binary_infinity(
	unsigned width, bool negative) noexcept
{
	return (negative ? (width == 16U ? 0x8000U : 0x8000'0000U) : 0U) |
		(width == 16U ? 0x7c00U : 0x7f80'0000U);
}

[[nodiscard]] inline std::uint32_t fp8_dot_add(
	std::uint32_t addend_bits, unsigned width,
	std::span<const std::uint8_t> left, std::span<const std::uint8_t> right,
	std::uint32_t, std::uint64_t fpmr) noexcept
{
	const auto addend = unpack_binary(addend_bits, width);
	const auto format1 = fp8_source_format(fpmr, false);
	const auto format2 = fp8_source_format(fpmr, true);
	std::array<ExactTerm, 8> first{};
	std::array<ExactTerm, 8> second{};
	bool any_nan = addend.type == FpClass::snan || addend.type == FpClass::qnan;
	for (std::size_t i = 0; i < left.size(); ++i) {
		first[i] = unpack_fp8(left[i], format1);
		second[i] = unpack_fp8(right[i], format2);
		any_nan = any_nan || first[i].type == FpClass::snan ||
			first[i].type == FpClass::qnan ||
			second[i].type == FpClass::snan ||
			second[i].type == FpClass::qnan;
	}
	if (any_nan)
		return binary_default_nan(width);
	bool positive_infinity = addend.type == FpClass::infinity && !addend.negative;
	bool negative_infinity = addend.type == FpClass::infinity && addend.negative;
	bool same_signed_zero = addend.type == FpClass::zero;
	bool invalid = false;
	std::array<ExactTerm, 8> products{};
	const unsigned scale = width == 16U ?
		static_cast<unsigned>((fpmr >> 16) & 15U) :
		static_cast<unsigned>((fpmr >> 16) & 127U);
	for (std::size_t i = 0; i < left.size(); ++i) {
		const bool product_negative = first[i].negative != second[i].negative;
		const bool product_infinity = first[i].type == FpClass::infinity ||
			second[i].type == FpClass::infinity;
		const bool product_zero = first[i].type == FpClass::zero ||
			second[i].type == FpClass::zero;
		invalid = invalid ||
			(first[i].type == FpClass::infinity && second[i].type == FpClass::zero) ||
			(second[i].type == FpClass::infinity && first[i].type == FpClass::zero);
		if (product_infinity && !product_zero) {
			positive_infinity = positive_infinity || !product_negative;
			negative_infinity = negative_infinity || product_negative;
		}
		same_signed_zero = same_signed_zero && product_zero &&
			(addend.negative == product_negative);
		if (!product_infinity && !product_zero) {
			products[i] = {FpClass::finite, product_negative,
				first[i].significand * second[i].significand,
				first[i].exponent + second[i].exponent -
					static_cast<int>(scale)};
		}
	}
	invalid = invalid || (positive_infinity && negative_infinity);
	if (invalid)
		return binary_default_nan(width);
	if (positive_infinity)
		return binary_infinity(width, false);
	if (negative_infinity)
		return binary_infinity(width, true);
	if (same_signed_zero)
		return addend.negative ? (width == 16U ? 0x8000U : 0x8000'0000U) : 0U;
	int common_exponent = 1000000;
	if (addend.type == FpClass::finite)
		common_exponent = addend.exponent;
	for (std::size_t i = 0; i < left.size(); ++i)
		if (products[i].type == FpClass::finite)
			common_exponent = std::min(common_exponent, products[i].exponent);
	SignedExact exact;
	if (addend.type == FpClass::finite)
		exact.add(addend, common_exponent);
	for (std::size_t i = 0; i < left.size(); ++i)
		exact.add(products[i], common_exponent);
	if (exact.magnitude.zero())
		return 0;
	const bool saturate = (fpmr & (std::uint64_t{1} << 14)) != 0;
	const auto rounded = round_binary(exact.magnitude, common_exponent,
		width == 16U ? 10U : 23U, width == 16U ? -14 : -126,
		width == 16U ? 15 : 127, saturate);
	return rounded.bits | (exact.negative ?
		(width == 16U ? 0x8000U : 0x8000'0000U) : 0U);
}

} // namespace uml::backend::arm64::detail
