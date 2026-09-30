// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_fp8_convert_up_engine_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

inline constexpr std::uint32_t fpsr_invalid = 1U << 0;
inline constexpr std::uint32_t fpsr_underflow = 1U << 3;
inline constexpr std::uint32_t fpsr_inexact = 1U << 4;

struct Form final {
	bool bfloat;
	bool source2;
	bool upper;
};

struct Registers final {
	unsigned destination;
	unsigned source;
};

enum class Class : std::uint8_t {
	zero,
	finite,
	infinity,
	quiet_nan,
	signaling_nan,
};

struct Unpacked final {
	Class type;
	bool negative;
	std::uint32_t significand;
	int exponent;
};

struct Conversion final {
	std::uint16_t bits;
	std::uint32_t flags;
};

struct Rounded final {
	std::uint32_t bits{};
	bool inexact{};
	bool pre_tiny{};
};

inline constexpr std::array forms{
	Form{false, false, false}, Form{false, true, false},
	Form{false, false, true}, Form{false, true, true},
	Form{true, false, false}, Form{true, true, false},
	Form{true, false, true}, Form{true, true, true},
};

inline constexpr std::array register_edges{
	Registers{0U, 1U}, Registers{1U, 0U}, Registers{1U, 1U},
	Registers{30U, 31U}, Registers{31U, 30U}, Registers{31U, 31U},
};

[[nodiscard]] constexpr std::uint32_t instruction(
	const Form &form, Registers registers = {0U, 1U}) noexcept
{
	const unsigned size =
		(form.bfloat ? 2U : 0U) | (form.source2 ? 1U : 0U);
	return 0x2e21'7800U |
		(form.upper ? 0x4000'0000U : 0U) |
		(size << 22) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

/*
 * Independent transcription of the architectural source-format unpacking.
 * The test oracle deliberately does not include the production decoder or
 * conversion helper.
 */
[[nodiscard]] constexpr Unpacked unpack(
	std::uint8_t bits, unsigned format) noexcept
{
	const bool negative = (bits & 0x80U) != 0U;
	if (format >= 2U)
		return {Class::signaling_nan, negative, 0U, 0};
	const bool e4m3 = format == 1U;
	const unsigned fraction_bits = e4m3 ? 3U : 2U;
	const unsigned exponent_bits = e4m3 ? 4U : 5U;
	const unsigned fraction_mask = (1U << fraction_bits) - 1U;
	const unsigned exponent_mask = (1U << exponent_bits) - 1U;
	const unsigned fraction = bits & fraction_mask;
	const unsigned exponent = (bits >> fraction_bits) & exponent_mask;
	if (exponent == 0U) {
		if (fraction == 0U)
			return {Class::zero, negative, 0U, 0};
		const int minimum_exponent = e4m3 ? -6 : -14;
		return {Class::finite, negative, fraction,
			minimum_exponent - static_cast<int>(fraction_bits)};
	}
	if (e4m3) {
		if (exponent == exponent_mask && fraction == fraction_mask)
			return {Class::signaling_nan, negative, 0U, 0};
		return {Class::finite, negative,
			(1U << fraction_bits) | fraction,
			static_cast<int>(exponent) - 7 -
				static_cast<int>(fraction_bits)};
	}
	if (exponent == exponent_mask) {
		if (fraction == 0U)
			return {Class::infinity, negative, 0U, 0};
		return {(fraction & 2U) != 0U ?
				Class::quiet_nan : Class::signaling_nan,
			negative, 0U, 0};
	}
	return {Class::finite, negative, 4U | fraction,
		static_cast<int>(exponent) - 15 - 2};
}

[[nodiscard]] constexpr std::uint64_t low_mask(unsigned bits) noexcept
{
	return bits >= 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] constexpr std::uint64_t round_ties_to_even(
	std::uint64_t magnitude, unsigned discard, bool &inexact) noexcept
{
	if (discard == 0U)
		return magnitude;
	if (discard >= 64U) {
		inexact = magnitude != 0U;
		return 0U;
	}
	auto retained = magnitude >> discard;
	const auto remainder = magnitude & low_mask(discard);
	const auto halfway = std::uint64_t{1} << (discard - 1U);
	inexact = remainder != 0U;
	if (remainder > halfway ||
	    (remainder == halfway && (retained & 1U) != 0U))
		++retained;
	return retained;
}

[[nodiscard]] constexpr Rounded round_binary(
	std::uint32_t significand, int common_exponent,
	unsigned fraction_bits, int minimum_exponent) noexcept
{
	Rounded result{};
	const unsigned significant_bits = std::bit_width(significand);
	if (significant_bits == 0U)
		return result;
	const int normalized_exponent =
		common_exponent + static_cast<int>(significant_bits) - 1;
	result.pre_tiny = normalized_exponent < minimum_exponent;
	if (!result.pre_tiny) {
		const unsigned shift =
			fraction_bits + 1U - significant_bits;
		const auto normalized =
			static_cast<std::uint64_t>(significand) << shift;
		const unsigned bias =
			static_cast<unsigned>(1 - minimum_exponent);
		result.bits =
			(static_cast<unsigned>(normalized_exponent) + bias)
				<< fraction_bits;
		result.bits |= static_cast<std::uint32_t>(normalized) &
			((1U << fraction_bits) - 1U);
		return result;
	}

	const int subnormal_unit =
		minimum_exponent - static_cast<int>(fraction_bits);
	const int shift = common_exponent - subnormal_unit;
	std::uint64_t rounded{};
	if (shift >= 0)
		rounded = static_cast<std::uint64_t>(significand)
			<< static_cast<unsigned>(shift);
	else
		rounded = round_ties_to_even(significand,
			static_cast<unsigned>(-shift), result.inexact);
	if (rounded >= (std::uint64_t{1} << fraction_bits))
		result.bits = 1U << fraction_bits;
	else
		result.bits = static_cast<std::uint32_t>(rounded);
	return result;
}

[[nodiscard]] constexpr unsigned source_format(
	std::uint64_t fpmr, bool source2) noexcept
{
	return static_cast<unsigned>(
		(fpmr >> (source2 ? 3U : 0U)) & 7U);
}

[[nodiscard]] constexpr unsigned scale(
	std::uint64_t fpmr, const Form &form) noexcept
{
	const unsigned shift = form.source2 ? 32U : 16U;
	return static_cast<unsigned>(
		(fpmr >> shift) & (form.bfloat ? 63U : 15U));
}

[[nodiscard]] constexpr Conversion convert(
	std::uint8_t input, const Form &form, std::uint64_t fpmr) noexcept
{
	const auto value = unpack(input, source_format(fpmr, form.source2));
	const std::uint16_t sign =
		value.negative ? std::uint16_t{0x8000U} : std::uint16_t{};
	if (value.type == Class::quiet_nan ||
	    value.type == Class::signaling_nan) {
		const auto nan = static_cast<std::uint16_t>(
			form.bfloat ? 0x7fc0U : 0x7e00U);
		return {nan, value.type == Class::signaling_nan ?
			fpsr_invalid : 0U};
	}
	if (value.type == Class::infinity)
		return {static_cast<std::uint16_t>(sign |
			(form.bfloat ? 0x7f80U : 0x7c00U)), 0U};
	if (value.type == Class::zero)
		return {sign, 0U};

	const unsigned fraction_bits = form.bfloat ? 7U : 10U;
	const int minimum_exponent = form.bfloat ? -126 : -14;
	const auto rounded = round_binary(
		value.significand,
		value.exponent - static_cast<int>(scale(fpmr, form)),
		fraction_bits, minimum_exponent);
	std::uint32_t flags{};
	if (rounded.inexact)
		flags |= fpsr_inexact;
	if (rounded.inexact && rounded.pre_tiny)
		flags |= fpsr_underflow;
	return {static_cast<std::uint16_t>(sign | rounded.bits), flags};
}

inline void write_lane(
	std::array<std::byte, 16> &bytes, unsigned lane,
	std::uint16_t value) noexcept
{
	bytes[lane * 2U] = static_cast<std::byte>(value);
	bytes[lane * 2U + 1U] = static_cast<std::byte>(value >> 8);
}

inline void fill_source(
	arm64::CpuState &cpu, Registers registers,
	std::span<const std::uint8_t> values)
{
	for (unsigned lane = 0U; lane < 16U; ++lane)
		cpu.vectors[registers.source].bytes[lane] =
			static_cast<std::byte>(values[lane % values.size()]);
}

[[nodiscard]] constexpr std::uint64_t fpmr(
	unsigned source1_format = 0U, unsigned source2_format = 0U,
	unsigned source1_scale = 0U, unsigned source2_scale = 0U) noexcept
{
	return (source1_format & 7U) |
		(static_cast<std::uint64_t>(source2_format & 7U) << 3) |
		(static_cast<std::uint64_t>(source1_scale & 127U) << 16) |
		(static_cast<std::uint64_t>(source2_scale & 63U) << 32);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, const Form &form,
	Registers registers)
{
	const auto source = initial.vectors[registers.source].bytes;
	auto expected = initial;
	std::array<std::byte, 16> output{};
	std::uint32_t flags{};
	const unsigned base = form.upper ? 8U : 0U;
	for (unsigned lane = 0U; lane < 8U; ++lane) {
		const auto converted = convert(
			std::to_integer<std::uint8_t>(source[base + lane]),
			form, initial.fpmr);
		write_lane(output, lane, converted.bits);
		flags |= converted.flags;
	}
	expected.vectors[registers.destination].bytes = output;
	expected.fpsr |= flags;
	return expected;
}

} // namespace arm64_fp8_convert_up_engine_test
