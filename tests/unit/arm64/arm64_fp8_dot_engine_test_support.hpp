// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_bfloat_arithmetic_test_support.hpp"
#include "arm64_fp8_convert_up_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_fp8_dot_engine_test {

namespace shared = arm64_bfloat_arithmetic_test;
namespace source_oracle = arm64_fp8_convert_up_engine_test;
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Form final {
	unsigned width;
	bool by_element;
	bool wide;
	unsigned index;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

enum class Class : std::uint8_t {
	zero,
	finite,
	infinity,
	nan,
};

struct Term final {
	Class type;
	bool negative;
	shared::BigUnsigned magnitude;
	int exponent;
};

inline constexpr std::array register_aliases{
	Registers{0U, 1U, 2U}, Registers{1U, 1U, 2U},
	Registers{2U, 1U, 2U}, Registers{1U, 2U, 2U},
	Registers{3U, 3U, 3U}, Registers{31U, 30U, 29U},
};

[[nodiscard]] constexpr unsigned lanes(const Form &form) noexcept
{
	return form.width == 16U ?
		(form.wide ? 8U : 4U) :
		(form.wide ? 4U : 2U);
}

[[nodiscard]] constexpr unsigned values_per_lane(
	const Form &form) noexcept
{
	return form.width / 8U;
}

[[nodiscard]] constexpr unsigned index_count(const Form &form) noexcept
{
	return form.width == 16U ? 8U : 4U;
}

[[nodiscard]] constexpr std::uint32_t instruction(
	const Form &form, Registers registers = {0U, 1U, 2U}) noexcept
{
	std::uint32_t result =
		(form.by_element ? 0x0f00'0000U : 0x0e00'fc00U) |
		(form.width == 16U ? 0x0040'0000U : 0U) |
		(form.wide ? 0x4000'0000U : 0U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
	if (!form.by_element)
		return result | ((registers.second & 31U) << 16U);
	if (form.width == 16U) {
		result |= (registers.second & 15U) << 16U;
		result |= ((form.index >> 2U) & 1U) << 11U;
		result |= ((form.index >> 1U) & 1U) << 21U;
		result |= (form.index & 1U) << 20U;
	} else {
		result |= (registers.second & 15U) << 16U;
		result |= ((registers.second >> 4U) & 1U) << 20U;
		result |= ((form.index >> 1U) & 1U) << 11U;
		result |= (form.index & 1U) << 21U;
	}
	return result;
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(const Form &form, std::uint32_t fpcr) noexcept
{
	using ::uml::cpu::ArmFeature;
	auto result = engine_tests::NativeRequirements::features(
		form.width == 16U ? ArmFeature::fp8dot2 :
			ArmFeature::fp8dot4);
	if ((fpcr & source_oracle::fpcr_ah) != 0U)
		result = result | ArmFeature::afp;
	return result;
}

[[nodiscard]] constexpr std::uint64_t fpmr(
	unsigned format1 = 0U, unsigned format2 = 0U,
	unsigned scale = 0U, bool saturate = false) noexcept
{
	return (format1 & 7U) |
		(static_cast<std::uint64_t>(format2 & 7U) << 3U) |
		(static_cast<std::uint64_t>(saturate) << 14U) |
		(static_cast<std::uint64_t>(scale & 127U) << 16U);
}

[[nodiscard]] inline Term unpack_source(
	std::uint8_t bits, unsigned format)
{
	const auto unpacked = source_oracle::unpack(bits, format);
	using SourceClass = source_oracle::Class;
	if (unpacked.type == SourceClass::zero)
		return {Class::zero, unpacked.negative, 0U, 0};
	if (unpacked.type == SourceClass::finite)
		return {Class::finite, unpacked.negative,
			unpacked.significand, unpacked.exponent};
	if (unpacked.type == SourceClass::infinity)
		return {Class::infinity, unpacked.negative, 0U, 0};
	return {Class::nan, unpacked.negative, 0U, 0};
}

[[nodiscard]] inline Term unpack_addend(
	std::uint32_t bits, unsigned width)
{
	const bool negative =
		((bits >> (width - 1U)) & 1U) != 0U;
	const unsigned fraction_bits = width == 16U ? 10U : 23U;
	const unsigned exponent_bits = width == 16U ? 5U : 8U;
	const unsigned exponent_mask = (1U << exponent_bits) - 1U;
	const std::uint32_t fraction_mask =
		(std::uint32_t{1} << fraction_bits) - 1U;
	const unsigned exponent =
		(bits >> fraction_bits) & exponent_mask;
	const auto fraction = bits & fraction_mask;
	if (exponent == exponent_mask)
		return {fraction == 0U ? Class::infinity : Class::nan,
			negative, 0U, 0};
	if (exponent == 0U) {
		if (fraction == 0U)
			return {Class::zero, negative, 0U, 0};
		const int minimum = width == 16U ? -14 : -126;
		return {Class::finite, negative, fraction,
			minimum - static_cast<int>(fraction_bits)};
	}
	const int bias = width == 16U ? 15 : 127;
	return {Class::finite, negative,
		(std::uint32_t{1} << fraction_bits) | fraction,
		static_cast<int>(exponent) - bias -
			static_cast<int>(fraction_bits)};
}

[[nodiscard]] inline shared::ExactValue exact(const Term &term)
{
	return {term.negative, term.magnitude, term.exponent};
}

[[nodiscard]] inline std::uint32_t default_nan(
	unsigned width, std::uint32_t fpcr) noexcept
{
	const bool negative =
		(fpcr & source_oracle::fpcr_ah) != 0U;
	return (negative ?
			(width == 16U ? 0x8000U : 0x8000'0000U) : 0U) |
		(width == 16U ? 0x7e00U : 0x7fc0'0000U);
}

[[nodiscard]] inline std::uint32_t infinity(
	unsigned width, bool negative) noexcept
{
	return (negative ?
			(width == 16U ? 0x8000U : 0x8000'0000U) : 0U) |
		(width == 16U ? 0x7c00U : 0x7f80'0000U);
}

[[nodiscard]] inline std::uint32_t round_binary(
	const shared::ExactValue &value, unsigned width,
	bool saturate)
{
	const std::uint32_t sign = value.negative ?
		(width == 16U ? 0x8000U : 0x8000'0000U) : 0U;
	if (value.magnitude.zero())
		return sign;
	const unsigned fraction_bits = width == 16U ? 10U : 23U;
	const int minimum_exponent = width == 16U ? -14 : -126;
	const int maximum_exponent = width == 16U ? 15 : 127;
	const unsigned source_bits = value.magnitude.bit_length();
	int top_exponent =
		value.exponent + static_cast<int>(source_bits) - 1;
	const int target_exponent = top_exponent >= minimum_exponent ?
		top_exponent - static_cast<int>(fraction_bits) :
		minimum_exponent - static_cast<int>(fraction_bits);
	const int signed_shift = target_exponent - value.exponent;
	shared::BigUnsigned retained = value.magnitude;
	unsigned shift{};
	if (signed_shift > 0) {
		shift = static_cast<unsigned>(signed_shift);
		retained = retained.shifted_right(shift);
	} else if (signed_shift < 0) {
		retained = retained.shifted_left(
			static_cast<unsigned>(-signed_shift));
	}
	const bool inexact =
		shift != 0U && value.magnitude.any_low_bits(shift);
	if (inexact) {
		const bool halfway =
			value.magnitude.bit(shift - 1U);
		const bool below_halfway =
			value.magnitude.any_low_bits(shift - 1U);
		if (halfway && (below_halfway || retained.odd()))
			retained.increment();
	}
	if (top_exponent >= minimum_exponent &&
	    retained.compare(shared::BigUnsigned{1}.shifted_left(
		    fraction_bits + 1U)) == 0) {
		retained = retained.shifted_right(1U);
		++top_exponent;
	}
	if (top_exponent > maximum_exponent) {
		if (saturate)
			return sign |
				(width == 16U ? 0x7bffU : 0x7f7f'ffffU);
		return infinity(width, value.negative);
	}

	unsigned exponent_field{};
	std::uint32_t fraction{};
	if (top_exponent >= minimum_exponent) {
		exponent_field = static_cast<unsigned>(
			top_exponent + (width == 16U ? 15 : 127));
		fraction = static_cast<std::uint32_t>(
			retained.low64()) &
			((std::uint32_t{1} << fraction_bits) - 1U);
	} else if (retained.compare(
			   shared::BigUnsigned{1}.shifted_left(
				   fraction_bits)) >= 0) {
		exponent_field = 1U;
	} else {
		fraction = static_cast<std::uint32_t>(
			retained.low64());
	}
	return sign | (exponent_field << fraction_bits) | fraction;
}

[[nodiscard]] inline std::uint32_t dot_add(
	std::uint32_t addend_bits, const Form &form,
	std::span<const std::uint8_t> left,
	std::span<const std::uint8_t> right,
	std::uint32_t fpcr, std::uint64_t control)
{
	const auto addend = unpack_addend(addend_bits, form.width);
	const unsigned format1 =
		static_cast<unsigned>(control & 7U);
	const unsigned format2 =
		static_cast<unsigned>((control >> 3U) & 7U);
	std::array<Term, 8> first{};
	std::array<Term, 8> second{};
	bool any_nan = addend.type == Class::nan;
	for (unsigned index = 0U; index < left.size(); ++index) {
		first[index] = unpack_source(left[index], format1);
		second[index] = unpack_source(right[index], format2);
		any_nan = any_nan || first[index].type == Class::nan ||
			second[index].type == Class::nan;
	}
	if (any_nan)
		return default_nan(form.width, fpcr);

	bool positive_infinity =
		addend.type == Class::infinity && !addend.negative;
	bool negative_infinity =
		addend.type == Class::infinity && addend.negative;
	bool same_signed_zero = addend.type == Class::zero;
	bool invalid{};
	std::array<shared::ExactValue, 8> products{};
	std::array<bool, 8> finite_products{};
	const unsigned scale = form.width == 16U ?
		static_cast<unsigned>((control >> 16U) & 15U) :
		static_cast<unsigned>((control >> 16U) & 127U);
	for (unsigned index = 0U; index < left.size(); ++index) {
		const bool product_negative =
			first[index].negative != second[index].negative;
		const bool product_infinity =
			first[index].type == Class::infinity ||
			second[index].type == Class::infinity;
		const bool product_zero =
			first[index].type == Class::zero ||
			second[index].type == Class::zero;
		invalid = invalid ||
			(first[index].type == Class::infinity &&
			 second[index].type == Class::zero) ||
			(second[index].type == Class::infinity &&
			 first[index].type == Class::zero);
		if (product_infinity && !product_zero) {
			positive_infinity =
				positive_infinity || !product_negative;
			negative_infinity =
				negative_infinity || product_negative;
		}
		same_signed_zero = same_signed_zero && product_zero &&
			addend.negative == product_negative;
		if (!product_infinity && !product_zero) {
			finite_products[index] = true;
			products[index] = {
				product_negative,
				multiply(first[index].magnitude,
					second[index].magnitude),
				first[index].exponent +
					second[index].exponent -
					static_cast<int>(scale),
			};
		}
	}
	invalid = invalid ||
		(positive_infinity && negative_infinity);
	if (invalid)
		return default_nan(form.width, fpcr);
	if (positive_infinity)
		return infinity(form.width, false);
	if (negative_infinity)
		return infinity(form.width, true);
	if (same_signed_zero)
		return addend.negative ?
			(form.width == 16U ? 0x8000U : 0x8000'0000U) : 0U;

	shared::ExactValue sum = addend.type == Class::finite ?
		exact(addend) :
		shared::ExactValue{addend.negative, 0U, 0};
	for (unsigned index = 0U; index < left.size(); ++index)
		if (finite_products[index])
			sum = shared::add_exact(sum, products[index], false);
	if (sum.magnitude.zero())
		return 0U;
	return round_binary(sum, form.width,
		(control & (std::uint64_t{1} << 14U)) != 0U);
}

[[nodiscard]] inline std::uint32_t read_lane(
	const std::array<std::byte, 16> &bytes,
	unsigned lane, unsigned width) noexcept
{
	std::uint32_t result{};
	for (unsigned byte = 0U; byte < width / 8U; ++byte)
		result |= std::to_integer<std::uint32_t>(
			bytes[lane * width / 8U + byte]) << (byte * 8U);
	return result;
}

inline void write_lane(
	std::array<std::byte, 16> &bytes,
	unsigned lane, unsigned width, std::uint32_t value) noexcept
{
	for (unsigned byte = 0U; byte < width / 8U; ++byte)
		bytes[lane * width / 8U + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
{
	return shared::state(seed);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, const Form &form,
	Registers registers)
{
	const auto destination =
		initial.vectors[registers.destination].bytes;
	const auto first = initial.vectors[registers.first].bytes;
	const auto second = initial.vectors[registers.second].bytes;
	auto expected = initial;
	std::array<std::byte, 16> result{};
	const unsigned count = values_per_lane(form);
	for (unsigned lane = 0U; lane < lanes(form); ++lane) {
		const unsigned left_offset = lane * count;
		const unsigned right_offset = form.by_element ?
			form.index * count : left_offset;
		std::array<std::uint8_t, 4> left_values{};
		std::array<std::uint8_t, 4> right_values{};
		for (unsigned index = 0U; index < count; ++index) {
			left_values[index] = std::to_integer<std::uint8_t>(
				first[left_offset + index]);
			right_values[index] = std::to_integer<std::uint8_t>(
				second[right_offset + index]);
		}
		write_lane(result, lane, form.width,
			dot_add(read_lane(destination, lane, form.width),
				form, std::span{left_values}.first(count),
				std::span{right_values}.first(count),
				initial.fpcr, initial.fpmr));
	}
	expected.vectors[registers.destination].bytes = result;
	return expected;
}

inline void check(
	engine_tests::EngineHarness &harness, std::string_view name,
	const Form &form, Registers registers,
	const arm64::CpuState &initial)
{
	harness.check_one(name, instruction(form, registers), initial,
		expected_state(initial, form, registers),
		native_requirements(form, initial.fpcr));
}

inline constexpr std::array<std::uint8_t, 20> representative_inputs{
	0x00U, 0x80U, 0x01U, 0x81U, 0x03U,
	0x04U, 0x3cU, 0xbcU, 0x38U, 0x40U,
	0x7bU, 0xfbU, 0x7cU, 0xfcU, 0x7dU,
	0x7eU, 0x7fU, 0xffU, 0x08U, 0x78U,
};

inline constexpr std::array<std::uint32_t, 12> half_addends{
	0x0000U, 0x8000U, 0x0001U, 0x03ffU,
	0x0400U, 0x3c00U, 0xbc00U, 0x7bffU,
	0x7c00U, 0xfc00U, 0x7e01U, 0x7d01U,
};

inline constexpr std::array<std::uint32_t, 12> single_addends{
	0x0000'0000U, 0x8000'0000U, 0x0000'0001U,
	0x007f'ffffU, 0x0080'0000U, 0x3f80'0000U,
	0xbf80'0000U, 0x7f7f'ffffU, 0x7f80'0000U,
	0xff80'0000U, 0x7fc1'2345U, 0x7f81'2345U,
};

} // namespace arm64_fp8_dot_engine_test
