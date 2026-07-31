// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace arm64_floating_scale_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

inline constexpr std::uint32_t fpsr_invalid = 1U << 0;
inline constexpr std::uint32_t fpsr_overflow = 1U << 2;
inline constexpr std::uint32_t fpsr_underflow = 1U << 3;
inline constexpr std::uint32_t fpsr_inexact = 1U << 4;
inline constexpr std::uint32_t fpsr_input_denormal = 1U << 7;
inline constexpr std::uint32_t fpsr_saturation = 1U << 27;

inline constexpr std::uint32_t fpcr_fiz = 1U << 0;
inline constexpr std::uint32_t fpcr_ah = 1U << 1;
inline constexpr std::uint32_t fpcr_fz16 = 1U << 19;
inline constexpr std::uint32_t fpcr_fz = 1U << 24;
inline constexpr std::uint32_t fpcr_dn = 1U << 25;

struct Form final {
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

struct ScaleResult final {
	std::uint64_t bits;
	std::uint32_t flags;
};

inline constexpr std::array forms{
	Form{16U, 8U}, Form{16U, 16U},
	Form{32U, 8U}, Form{32U, 16U},
	Form{64U, 16U},
};

inline constexpr std::array register_cases{
	Registers{0, 1, 2},
	Registers{1, 1, 2},
	Registers{2, 1, 2},
	Registers{0, 1, 1},
	Registers{1, 1, 1},
	Registers{31, 30, 29},
	Registers{30, 31, 30},
	Registers{31, 31, 31},
};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto &form : forms)
		function(form);
}

[[nodiscard]] constexpr unsigned shape(const Form &form) noexcept
{
	return form.width == 16U ? (form.vector_bytes == 16U ? 1U : 0U) :
		form.width == 32U ? (form.vector_bytes == 16U ? 3U : 2U) : 4U;
}

/*
 * Direct transcription of the two encoding diagrams. The expected-value
 * model below deliberately does not include or call the production decoder.
 */
[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	constexpr std::array bases{
		0x2ec0'3c00U, 0x6ec0'3c00U,
		0x2ea0'fc00U, 0x6ea0'fc00U, 0x6ee0'fc00U,
	};
	return bases[shape(form)] | (registers.destination & 31U) |
		((registers.first & 31U) << 5) |
		((registers.second & 31U) << 16);
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		uml::backend::ArmFeature::fp8);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed * 67U + reg * 29U + byte * 17U + 3U);
	for (unsigned reg = 0; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(reg + seed) *
			 0x0101'0101'0101'0101ULL);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0;
	cpu.fpsr = (seed & 1U) != 0U ? fpsr_saturation : 0U;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	cpu.set_mte_configuration(true, 0xa55aU);
	cpu.set_mte_generator(true, 0x1357U, 9U,
		0x3141'5926'5358'9793ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0x89ab'cdefU;
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_sm = true;
	cpu.pstate_za = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xbU;
	cpu.exclusive_address = 0x1234'5000ULL;
	cpu.exclusive_key = 0x2468U;
	cpu.exclusive_value = 0x1122'3344'5566'7788ULL;
	cpu.exclusive_value2 = 0x8877'6655'4433'2211ULL;
	cpu.exclusive_version = 0x1020'3040'5060'7080ULL;
	cpu.exclusive_size = 8;
	return cpu;
}

[[nodiscard]] constexpr Format format(unsigned width) noexcept
{
	return width == 16U ? Format{16U, 5U, 10U, 15} :
		width == 32U ? Format{32U, 8U, 23U, 127} :
		Format{64U, 11U, 52U, 1023};
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

[[nodiscard]] constexpr std::uint64_t minimum_normal_bits(
	const Format &value, bool negative = false) noexcept
{
	return zero_bits(value, negative) |
		(std::uint64_t{1} << value.fraction_bits);
}

[[nodiscard]] constexpr std::uint64_t minimum_subnormal_bits(
	const Format &value, bool negative = false) noexcept
{
	return zero_bits(value, negative) | 1U;
}

[[nodiscard]] constexpr std::uint64_t maximum_subnormal_bits(
	const Format &value, bool negative = false) noexcept
{
	return zero_bits(value, negative) | fraction_mask(value);
}

[[nodiscard]] constexpr std::uint64_t quiet_nan_bits(
	const Format &value, bool negative = false) noexcept
{
	return infinity_bits(value, negative) |
		(std::uint64_t{1} << (value.fraction_bits - 1U)) | 0x55U;
}

[[nodiscard]] constexpr std::uint64_t signaling_nan_bits(
	const Format &value, bool negative = false) noexcept
{
	return infinity_bits(value, negative) | 0x55U;
}

[[nodiscard]] constexpr std::uint64_t default_nan_bits(
	const Format &value, bool negative = false) noexcept
{
	return infinity_bits(value, negative) |
		(std::uint64_t{1} << (value.fraction_bits - 1U));
}

[[nodiscard]] constexpr std::uint64_t normal_bits(const Format &value,
	unsigned exponent_field, std::uint64_t fraction,
	bool negative = false) noexcept
{
	return zero_bits(value, negative) |
		(static_cast<std::uint64_t>(exponent_field) <<
			value.fraction_bits) |
		(fraction & fraction_mask(value));
}

[[nodiscard]] constexpr std::uint64_t scale_bits(
	std::int64_t scale, unsigned width) noexcept
{
	return static_cast<std::uint64_t>(scale) & low_mask(width);
}

[[nodiscard]] constexpr std::int64_t signed_scale(
	std::uint64_t raw, unsigned width) noexcept
{
	raw &= low_mask(width);
	if (width == 64U)
		return std::bit_cast<std::int64_t>(raw);
	const auto sign = std::uint64_t{1} << (width - 1U);
	return static_cast<std::int64_t>((raw ^ sign) - sign);
}

[[nodiscard]] inline std::uint64_t read_lane(
	const std::array<std::byte, 16> &bytes, unsigned lane,
	unsigned width)
{
	const unsigned lane_bytes = width / 8U;
	std::uint64_t result = 0;
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		result |= static_cast<std::uint64_t>(std::to_integer<unsigned>(
			bytes[lane * lane_bytes + byte])) << (byte * 8U);
	return result;
}

inline void write_lane(std::array<std::byte, 16> &bytes, unsigned lane,
	unsigned width, std::uint64_t value)
{
	const unsigned lane_bytes = width / 8U;
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		bytes[lane * lane_bytes + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

inline void fill_lanes(arm64::CpuState &cpu, unsigned reg,
	const Form &form, std::uint64_t value)
{
	const unsigned lanes = form.vector_bytes / (form.width / 8U);
	for (unsigned lane = 0; lane < lanes; ++lane)
		write_lane(cpu.vectors[reg].bytes, lane, form.width, value);
}

[[nodiscard]] constexpr bool round_up(std::uint64_t retained,
	std::uint64_t remainder, unsigned shift, unsigned rounding,
	bool negative) noexcept
{
	if (remainder == 0U)
		return false;
	switch (rounding) {
	case 0: {
		const auto halfway = std::uint64_t{1} << (shift - 1U);
		return remainder > halfway ||
			(remainder == halfway && (retained & 1U) != 0U);
	}
	case 1:
		return !negative;
	case 2:
		return negative;
	default:
		return false;
	}
}

/*
 * Independent bit-level transcription of FPUnpack, FPClampScale,
 * FPRound, FPProcessNaN, and FPProcessDenorm for this operation. Scaling by
 * a power of two only changes the binary exponent; integer shifts therefore
 * model the exact real result without using host floating-point arithmetic or
 * any emulator semantic helper.
 */
[[nodiscard]] constexpr ScaleResult oracle_scale(std::uint64_t raw,
	unsigned width, std::int64_t requested_scale,
	std::uint32_t fpcr) noexcept
{
	const auto value = format(width);
	raw &= low_mask(width);
	const bool negative = (raw & sign_mask(value)) != 0U;
	const auto exponent =
		(raw & exponent_mask(value)) >> value.fraction_bits;
	const auto fraction = raw & fraction_mask(value);
	const auto exponent_ones = maximum_exponent(value);

	if (exponent == exponent_ones) {
		if (fraction == 0U)
			return {raw, 0U};
		const auto quiet_bit =
			std::uint64_t{1} << (value.fraction_bits - 1U);
		const bool signaling = (fraction & quiet_bit) == 0U;
		const auto result = (fpcr & fpcr_dn) != 0U ?
			default_nan_bits(value, (fpcr & fpcr_ah) != 0U) :
			raw | quiet_bit;
		return {result, signaling ? fpsr_invalid : 0U};
	}

	if (exponent == 0U && fraction == 0U)
		return {zero_bits(value, negative), 0U};

	const bool denormal = exponent == 0U;
	if (denormal && width == 16U && (fpcr & fpcr_fz16) != 0U)
		return {zero_bits(value, negative), 0U};
	if (denormal && width != 16U) {
		const bool classic_flush =
			(fpcr & fpcr_fz) != 0U && (fpcr & fpcr_ah) == 0U;
		if (classic_flush || (fpcr & fpcr_fiz) != 0U)
			return {zero_bits(value, negative),
				classic_flush ? fpsr_input_denormal : 0U};
	}

	const std::uint64_t significand = denormal ? fraction :
		(std::uint64_t{1} << value.fraction_bits) | fraction;
	const int significand_exponent = denormal ?
		1 - value.bias - static_cast<int>(value.fraction_bits) :
		static_cast<int>(exponent) - value.bias -
			static_cast<int>(value.fraction_bits);
	const std::int64_t minimum_scale =
		-static_cast<std::int64_t>(value.fraction_bits + 1U);
	const std::int64_t maximum_scale =
		static_cast<std::int64_t>(exponent_ones) +
		static_cast<std::int64_t>(value.fraction_bits + 1U);
	const auto low = minimum_scale - static_cast<std::int64_t>(exponent);
	const auto high = maximum_scale - static_cast<std::int64_t>(exponent);
	const auto scale = requested_scale < low ? low :
		requested_scale > high ? high : requested_scale;
	const unsigned highest =
		63U - static_cast<unsigned>(std::countl_zero(significand));
	const auto result_exponent =
		static_cast<std::int64_t>(significand_exponent) + scale +
		static_cast<std::int64_t>(highest);
	const int minimum_normal_exponent = 1 - value.bias;
	std::uint32_t flags =
		denormal && width != 16U && (fpcr & fpcr_ah) != 0U ?
			fpsr_input_denormal : 0U;

	if (result_exponent >= minimum_normal_exponent) {
		const auto biased = result_exponent + value.bias;
		if (biased >= static_cast<std::int64_t>(exponent_ones)) {
			const unsigned rounding = (fpcr >> 22) & 3U;
			const bool to_infinity = rounding == 0U ||
				(rounding == 1U && !negative) ||
				(rounding == 2U && negative);
			return {
				to_infinity ? infinity_bits(value, negative) :
					maximum_finite_bits(value, negative),
				flags | fpsr_overflow | fpsr_inexact,
			};
		}
		const auto normalized =
			significand << (value.fraction_bits - highest);
		return {
			zero_bits(value, negative) |
				(static_cast<std::uint64_t>(biased) <<
					value.fraction_bits) |
				(normalized & fraction_mask(value)),
			flags,
		};
	}

	const bool output_flush = width == 16U ?
		(fpcr & fpcr_fz16) != 0U : (fpcr & fpcr_fz) != 0U;
	if (output_flush && (fpcr & fpcr_ah) == 0U)
		return {zero_bits(value, negative), flags | fpsr_underflow};

	const int subnormal_unit_exponent =
		minimum_normal_exponent - static_cast<int>(value.fraction_bits);
	const auto signed_shift =
		static_cast<std::int64_t>(subnormal_unit_exponent) -
		(static_cast<std::int64_t>(significand_exponent) + scale);
	std::uint64_t retained;
	std::uint64_t remainder;
	unsigned shift{};
	if (signed_shift <= 0) {
		retained = significand << static_cast<unsigned>(-signed_shift);
		remainder = 0U;
	} else {
		shift = static_cast<unsigned>(signed_shift);
		retained = significand >> shift;
		remainder = significand & low_mask(shift);
	}
	const bool inexact = remainder != 0U;
	const auto rounded = retained + static_cast<unsigned>(
		inexact && round_up(retained, remainder, shift,
			(fpcr >> 22) & 3U, negative));

	if (output_flush && (fpcr & fpcr_ah) != 0U)
		return {zero_bits(value, negative),
			flags | fpsr_underflow | fpsr_inexact};

	if (inexact) {
		flags |= fpsr_inexact;
		if ((fpcr & fpcr_ah) == 0U ||
		    rounded < (std::uint64_t{1} << value.fraction_bits))
			flags |= fpsr_underflow;
	}
	return {zero_bits(value, negative) | rounded, flags};
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, const Form &form, Registers registers)
{
	const auto first = initial.vectors[registers.first].bytes;
	const auto second = initial.vectors[registers.second].bytes;
	auto expected = initial;
	std::array<std::byte, 16> result{};
	const unsigned lanes = form.vector_bytes / (form.width / 8U);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto scaled = oracle_scale(
			read_lane(first, lane, form.width), form.width,
			signed_scale(read_lane(second, lane, form.width), form.width),
			initial.fpcr);
		write_lane(result, lane, form.width, scaled.bits);
		expected.fpsr |= scaled.flags;
	}
	expected.vectors[registers.destination].bytes = result;
	return expected;
}

static_assert(signed_scale(0xffffU, 16U) == -1);
static_assert(signed_scale(0x8000'0000U, 32U) ==
	std::numeric_limits<std::int32_t>::min());
static_assert(oracle_scale(0x3fc0'0000U, 32U, 3, 0).bits ==
	0x4140'0000U);
static_assert(oracle_scale(0x0080'0000U, 32U, -1, 0).bits ==
	0x0040'0000U);
static_assert(oracle_scale(0x0080'0000U, 32U, -1, fpcr_fz).flags ==
	fpsr_underflow);
static_assert(oracle_scale(0x7f7f'ffffU, 32U, 1, 0).flags ==
	(fpsr_overflow | fpsr_inexact));
static_assert(oracle_scale(0x7f80'0055U, 32U, 0, 0).bits ==
	0x7fc0'0055U);
static_assert(oracle_scale(0x7f80'0055U, 32U, 0, 0).flags ==
	fpsr_invalid);

} // namespace arm64_floating_scale_test
