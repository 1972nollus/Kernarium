// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_fp8_convert_down_engine_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

inline constexpr std::uint32_t fpsr_invalid = 1U << 0;
inline constexpr std::uint32_t fpsr_overflow = 1U << 2;
inline constexpr std::uint32_t fpsr_underflow = 1U << 3;
inline constexpr std::uint32_t fpsr_inexact = 1U << 4;
inline constexpr std::uint32_t fpcr_ah = 1U << 1;

struct Form final {
	unsigned source_width;
	bool upper;
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
	std::uint8_t bits;
	std::uint32_t flags;
};

struct Rounded final {
	std::uint32_t bits{};
	bool inexact{};
	bool pre_tiny{};
	bool post_tiny{};
	bool overflow{};
};

inline constexpr std::array forms{
	Form{16U, false}, Form{16U, true},
	Form{32U, false}, Form{32U, true},
};

inline constexpr std::array register_edges{
	Registers{0U, 1U, 2U}, Registers{1U, 0U, 2U},
	Registers{2U, 1U, 0U}, Registers{1U, 1U, 2U},
	Registers{1U, 2U, 1U}, Registers{1U, 1U, 1U},
	Registers{29U, 30U, 31U}, Registers{31U, 31U, 31U},
};

[[nodiscard]] constexpr unsigned lanes(const Form &form) noexcept
{
	return form.source_width == 16U ? (form.upper ? 8U : 4U) : 4U;
}

[[nodiscard]] constexpr unsigned output_base(const Form &form) noexcept
{
	return form.source_width == 32U && form.upper ? 8U : 0U;
}

[[nodiscard]] constexpr std::uint32_t instruction(
	const Form &form, Registers registers = {0U, 1U, 2U}) noexcept
{
	return (form.source_width == 16U ?
			0x0e40'f400U : 0x0e00'f400U) |
		(form.upper ? 0x4000'0000U : 0U) |
		((registers.second & 31U) << 16) |
		((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements() noexcept
{
	return engine_tests::NativeRequirements::features(
		uml::backend::ArmFeature::fp8);
}

[[nodiscard]] constexpr Unpacked unpack(
	std::uint32_t bits, unsigned width) noexcept
{
	const unsigned fraction_bits = width == 16U ? 10U : 23U;
	const unsigned exponent_bits = width == 16U ? 5U : 8U;
	const auto fraction_mask =
		(std::uint32_t{1} << fraction_bits) - 1U;
	const auto exponent_mask = (1U << exponent_bits) - 1U;
	const bool negative = ((bits >> (width - 1U)) & 1U) != 0U;
	const auto fraction = bits & fraction_mask;
	const auto exponent = (bits >> fraction_bits) & exponent_mask;
	if (exponent == 0U) {
		if (fraction == 0U)
			return {Class::zero, negative, 0U, 0};
		const int minimum = width == 16U ? -14 : -126;
		return {Class::finite, negative, fraction,
			minimum - static_cast<int>(fraction_bits)};
	}
	if (exponent == exponent_mask) {
		if (fraction == 0U)
			return {Class::infinity, negative, 0U, 0};
		const bool quiet = (fraction &
			(std::uint32_t{1} << (fraction_bits - 1U))) != 0U;
		return {quiet ? Class::quiet_nan : Class::signaling_nan,
			negative, 0U, 0};
	}
	const int bias = width == 16U ? 15 : 127;
	return {Class::finite, negative,
		(std::uint32_t{1} << fraction_bits) | fraction,
		static_cast<int>(exponent) - bias -
			static_cast<int>(fraction_bits)};
}

[[nodiscard]] constexpr std::uint64_t low_mask(unsigned bits) noexcept
{
	return bits >= 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] constexpr std::uint64_t rounded_significand(
	std::uint64_t magnitude, int discard, bool &inexact) noexcept
{
	if (discard <= 0)
		return magnitude << static_cast<unsigned>(-discard);
	if (discard >= 64) {
		inexact = magnitude != 0U;
		return 0U;
	}
	const unsigned removed = static_cast<unsigned>(discard);
	auto retained = magnitude >> removed;
	const auto remainder = magnitude & low_mask(removed);
	const auto halfway = std::uint64_t{1} << (removed - 1U);
	inexact = remainder != 0U;
	if (remainder > halfway ||
	    (remainder == halfway && (retained & 1U) != 0U))
		++retained;
	return retained;
}

[[nodiscard]] constexpr Rounded round_to_format(
	std::uint32_t significand, int common_exponent,
	unsigned fraction_bits, int minimum_exponent,
	int maximum_exponent) noexcept
{
	Rounded result{};
	const unsigned precision = fraction_bits + 1U;
	const unsigned significant_bits = std::bit_width(significand);
	if (significant_bits == 0U)
		return result;
	int exponent =
		common_exponent + static_cast<int>(significant_bits) - 1;
	result.pre_tiny = exponent < minimum_exponent;
	std::uint64_t rounded{};
	if (exponent >= minimum_exponent) {
		rounded = rounded_significand(significand,
			static_cast<int>(significant_bits) -
				static_cast<int>(precision),
			result.inexact);
		if (rounded == (std::uint64_t{1} << precision)) {
			rounded >>= 1U;
			++exponent;
		}
		if (exponent > maximum_exponent) {
			result.overflow = result.inexact = true;
			return result;
		}
		const unsigned bias = static_cast<unsigned>(1 - minimum_exponent);
		result.bits =
			(static_cast<unsigned>(exponent) + bias) << fraction_bits;
		result.bits |= static_cast<std::uint32_t>(rounded) &
			((1U << fraction_bits) - 1U);
		return result;
	}
	const int subnormal_unit =
		minimum_exponent - static_cast<int>(fraction_bits);
	rounded = rounded_significand(significand,
		subnormal_unit - common_exponent, result.inexact);
	if (rounded >= (std::uint64_t{1} << fraction_bits))
		result.bits = 1U << fraction_bits;
	else
		result.bits = static_cast<std::uint32_t>(rounded);
	result.post_tiny =
		result.bits != 0U && result.bits < (1U << fraction_bits);
	return result;
}

[[nodiscard]] constexpr Conversion convert(std::uint32_t bits,
	unsigned width, std::uint32_t fpcr, std::uint64_t fpmr) noexcept
{
	const unsigned destination_format =
		static_cast<unsigned>((fpmr >> 6) & 7U);
	if (destination_format >= 2U)
		return {0xffU, fpsr_invalid};
	const bool e4m3 = destination_format == 1U;
	const auto source = unpack(bits, width);
	const auto sign =
		static_cast<std::uint8_t>(source.negative ? 0x80U : 0U);
	if (source.type == Class::quiet_nan ||
	    source.type == Class::signaling_nan)
		return {static_cast<std::uint8_t>(
			((fpcr & fpcr_ah) != 0U ? 0x80U : 0U) |
			(e4m3 ? 0x7fU : 0x7eU)),
			source.type == Class::signaling_nan ? fpsr_invalid : 0U};
	const bool saturate = (fpmr & (std::uint64_t{1} << 15)) != 0U;
	if (source.type == Class::infinity)
		return {static_cast<std::uint8_t>(sign |
			(e4m3 ? (saturate ? 0x7eU : 0x7fU) :
				(saturate ? 0x7bU : 0x7cU))), 0U};
	if (source.type == Class::zero)
		return {sign, 0U};

	int scale = static_cast<std::int8_t>((fpmr >> 24) & 0xffU);
	if (width == 16U) {
		const auto field = static_cast<unsigned>((fpmr >> 24) & 31U);
		scale = (field & 16U) != 0U ?
			static_cast<int>(field) - 32 : static_cast<int>(field);
	}
	const unsigned fraction_bits = e4m3 ? 3U : 2U;
	auto rounded = round_to_format(source.significand,
		source.exponent + scale, fraction_bits,
		e4m3 ? -6 : -14, e4m3 ? 8 : 15);
	if (e4m3 && !rounded.overflow &&
	    (rounded.bits & 0x7fU) == 0x7fU)
		rounded.overflow = rounded.inexact = true;
	const auto value = rounded.overflow ?
		static_cast<std::uint8_t>(sign |
			(e4m3 ? (saturate ? 0x7eU : 0x7fU) :
				(saturate ? 0x7bU : 0x7cU))) :
		static_cast<std::uint8_t>(sign | rounded.bits);
	std::uint32_t flags{};
	if (rounded.overflow)
		flags |= fpsr_overflow;
	if (rounded.inexact)
		flags |= fpsr_inexact;
	if (rounded.inexact &&
	    ((fpcr & fpcr_ah) != 0U ?
			rounded.post_tiny : rounded.pre_tiny))
		flags |= fpsr_underflow;
	return {value, flags};
}

[[nodiscard]] inline std::uint32_t read_lane(
	const std::array<std::byte, 16> &bytes, unsigned lane,
	unsigned width) noexcept
{
	std::uint32_t result{};
	for (unsigned byte = 0U; byte < width / 8U; ++byte)
		result |= static_cast<std::uint32_t>(std::to_integer<unsigned>(
			bytes[lane * width / 8U + byte])) << (byte * 8U);
	return result;
}

inline void write_lane(std::array<std::byte, 16> &bytes, unsigned lane,
	unsigned width, std::uint32_t value) noexcept
{
	for (unsigned byte = 0U; byte < width / 8U; ++byte)
		bytes[lane * width / 8U + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

inline void fill_source(arm64::CpuState &cpu, const Form &form,
	Registers registers, std::span<const std::uint32_t> first,
	std::span<const std::uint32_t> second)
{
	for (unsigned lane = 0U; lane < lanes(form); ++lane) {
		write_lane(cpu.vectors[registers.first].bytes, lane,
			form.source_width, first[lane % first.size()]);
		write_lane(cpu.vectors[registers.second].bytes, lane,
			form.source_width, second[lane % second.size()]);
	}
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0U; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0U;
		     byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] =
				static_cast<std::byte>(seed * 59U + reg * 37U +
					byte * 23U + 11U);
	for (unsigned reg = 0U; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(reg + seed) *
				0x0101'0101'0101'0101ULL);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpsr = (seed & 1U) != 0U ? 1U << 27 : 0U;
	cpu.fpmr = 0U;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	cpu.set_mte_configuration(true, 0xa55aU);
	cpu.set_mte_generator(true, 0x1357U, 9U,
		0x3141'5926'5358'9793ULL);
	return cpu;
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, const Form &form,
	Registers registers)
{
	const auto first = initial.vectors[registers.first].bytes;
	const auto second = initial.vectors[registers.second].bytes;
	auto expected = initial;
	auto output = form.source_width == 32U && form.upper ?
		initial.vectors[registers.destination].bytes :
		std::array<std::byte, 16>{};
	std::uint32_t flags{};
	for (unsigned lane = 0U; lane < lanes(form); ++lane) {
		const auto one = convert(read_lane(first, lane, form.source_width),
			form.source_width, initial.fpcr, initial.fpmr);
		const auto two = convert(read_lane(second, lane, form.source_width),
			form.source_width, initial.fpcr, initial.fpmr);
		output[output_base(form) + lane] =
			static_cast<std::byte>(one.bits);
		output[output_base(form) + lanes(form) + lane] =
			static_cast<std::byte>(two.bits);
		flags |= one.flags | two.flags;
	}
	expected.vectors[registers.destination].bytes = output;
	expected.fpsr |= flags;
	return expected;
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, const Form &form, Registers registers,
	const arm64::CpuState &initial)
{
	harness.check_one(name, instruction(form, registers), initial,
		expected_state(initial, form, registers), native_requirements());
}

[[nodiscard]] constexpr std::array<std::uint32_t, 18>
representative_inputs(unsigned width) noexcept
{
	if (width == 16U)
		return {0x0000U, 0x8000U, 0x0001U, 0x8001U,
			0x03ffU, 0x83ffU, 0x0400U, 0x8400U,
			0x3c00U, 0xbc00U, 0x3c80U, 0x3d80U,
			0x7bffU, 0xfbffU, 0x7c00U, 0xfc00U,
			0x7e01U, 0x7d01U};
	return {0x0000'0000U, 0x8000'0000U,
		0x0000'0001U, 0x8000'0001U,
		0x007f'ffffU, 0x807f'ffffU,
		0x0080'0000U, 0x8080'0000U,
		0x3f80'0000U, 0xbf80'0000U,
		0x3e00'0000U, 0x3fc0'0000U,
		0x7f7f'ffffU, 0xff7f'ffffU,
		0x7f80'0000U, 0xff80'0000U,
		0x7fc1'2345U, 0x7f81'2345U};
}

[[nodiscard]] constexpr std::uint64_t fpmr(
	unsigned destination_format = 0U, int scale = 0,
	bool saturate = false) noexcept
{
	return (static_cast<std::uint64_t>(destination_format & 7U) << 6) |
		(static_cast<std::uint64_t>(
			static_cast<std::uint8_t>(scale)) << 24) |
		(static_cast<std::uint64_t>(saturate) << 15);
}

} // namespace arm64_fp8_convert_down_engine_test
