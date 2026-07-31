// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "cpu/arm64/detail/floating_round.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_floating_round_test {

namespace arm64 = uml::backend::arm64;
namespace detail = uml::backend::arm64::detail;
namespace engine_tests = uml::backend::tests;

using Operation = detail::FloatingRoundOperation;

inline constexpr std::uint32_t fpsr_invalid = 1U << 0;
inline constexpr std::uint32_t fpsr_inexact = 1U << 4;
inline constexpr std::uint32_t fpsr_input_denormal = 1U << 7;
inline constexpr std::uint32_t fpsr_saturation = 1U << 27;

inline constexpr std::uint32_t fpcr_fiz = 1U << 0;
inline constexpr std::uint32_t fpcr_ah = 1U << 1;
inline constexpr std::uint32_t fpcr_nep = 1U << 2;
inline constexpr std::uint32_t fpcr_fz16 = 1U << 19;
inline constexpr std::uint32_t fpcr_fz = 1U << 24;
inline constexpr std::uint32_t fpcr_dn = 1U << 25;

struct Form final {
	Operation operation;
	bool scalar;
	unsigned width;
	unsigned vector_bytes;
};

struct Registers final {
	unsigned destination;
	unsigned source;
};

struct Format final {
	unsigned width;
	unsigned exponent_bits;
	unsigned fraction_bits;
	int bias;
};

struct RoundResult final {
	std::uint64_t bits;
	std::uint32_t flags;
};

enum class Rounding {
	ties_even,
	plus_infinity,
	minus_infinity,
	zero,
	ties_away,
};

inline constexpr std::array standard_operations{
	Operation::nearest, Operation::plus, Operation::minus, Operation::zero,
	Operation::ties_away, Operation::exact_current, Operation::current};
inline constexpr std::array sized_operations{
	Operation::int32_exact, Operation::int32_zero,
	Operation::int64_exact, Operation::int64_zero};
inline constexpr std::array widths{16U, 32U, 64U};
inline constexpr std::array vector_shapes{
	std::array{16U, 8U}, std::array{16U, 16U},
	std::array{32U, 8U}, std::array{32U, 16U},
	std::array{64U, 16U}};
inline constexpr std::array sized_vector_shapes{
	std::array{32U, 8U}, std::array{32U, 16U},
	std::array{64U, 16U}};
inline constexpr std::array register_cases{
	Registers{0, 1}, Registers{1, 1}, Registers{1, 0},
	Registers{31, 30}, Registers{30, 31}, Registers{31, 31}};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto operation : standard_operations) {
		for (const auto width : widths)
			function(Form{operation, true, width, 16U});
		for (const auto shape : vector_shapes)
			function(Form{operation, false, shape[0], shape[1]});
	}
	for (const auto operation : sized_operations) {
		for (const auto width : {32U, 64U})
			function(Form{operation, true, width, 16U});
		for (const auto shape : sized_vector_shapes)
			function(Form{operation, false, shape[0], shape[1]});
	}
}

[[nodiscard]] constexpr bool is_sized(Operation operation) noexcept
{
	return operation == Operation::int32_exact ||
		operation == Operation::int32_zero ||
		operation == Operation::int64_exact ||
		operation == Operation::int64_zero;
}

[[nodiscard]] constexpr unsigned operation_index(Operation operation) noexcept
{
	switch (operation) {
	case Operation::nearest: return 0;
	case Operation::plus: return 1;
	case Operation::minus: return 2;
	case Operation::zero: return 3;
	case Operation::ties_away: return 4;
	case Operation::exact_current: return 5;
	case Operation::current: return 6;
	case Operation::int32_exact: return 0;
	case Operation::int32_zero: return 1;
	case Operation::int64_exact: return 2;
	case Operation::int64_zero: return 3;
	}
	return 0;
}

[[nodiscard]] constexpr std::string_view operation_name(
	Operation operation) noexcept
{
	switch (operation) {
	case Operation::nearest: return "round-nearest-even";
	case Operation::plus: return "round-plus-infinity";
	case Operation::minus: return "round-minus-infinity";
	case Operation::zero: return "round-zero";
	case Operation::ties_away: return "round-nearest-away";
	case Operation::exact_current: return "round-current-exact";
	case Operation::current: return "round-current";
	case Operation::int32_exact: return "round-32-current";
	case Operation::int32_zero: return "round-32-zero";
	case Operation::int64_exact: return "round-64-current";
	case Operation::int64_zero: return "round-64-zero";
	}
	return "round";
}

[[nodiscard]] constexpr unsigned scalar_type(unsigned width) noexcept
{
	return width == 16U ? 3U : width == 32U ? 0U : 1U;
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1}) noexcept
{
	constexpr std::array scalar_standard{
		0x1e24'4000U, 0x1e24'c000U, 0x1e25'4000U,
		0x1e25'c000U, 0x1e26'4000U, 0x1e27'4000U,
		0x1e27'c000U};
	constexpr std::array scalar_sized{
		0x1e28'c000U, 0x1e28'4000U,
		0x1e29'c000U, 0x1e29'4000U};
	constexpr std::array vector_half{
		0x0e79'8800U, 0x0ef9'8800U, 0x0e79'9800U,
		0x0ef9'9800U, 0x2e79'8800U, 0x2e79'9800U,
		0x2ef9'9800U};
	constexpr std::array vector_ordinary{
		0x0e21'8800U, 0x0ea1'8800U, 0x0e21'9800U,
		0x0ea1'9800U, 0x2e21'8800U, 0x2e21'9800U,
		0x2ea1'9800U};
	constexpr std::array vector_sized{
		0x2e21'e800U, 0x0e21'e800U,
		0x2e21'f800U, 0x0e21'f800U};
	std::uint32_t value;
	if (form.scalar) {
		value = is_sized(form.operation) ?
			scalar_sized[operation_index(form.operation)] :
			scalar_standard[operation_index(form.operation)];
		value |= scalar_type(form.width) << 22;
	} else {
		const bool q = form.vector_bytes == 16U;
		if (is_sized(form.operation))
			value = vector_sized[operation_index(form.operation)];
		else if (form.width == 16U)
			value = vector_half[operation_index(form.operation)];
		else
			value = vector_ordinary[operation_index(form.operation)];
		value |= q ? 0x4000'0000U : 0U;
		value |= form.width == 64U ? 0x0040'0000U : 0U;
	}
	return value | ((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(const Form &form)
{
	using uml::backend::ArmFeature;
	auto requirements = engine_tests::NativeRequirements::features(
		form.scalar ? ArmFeature::floating_point :
			ArmFeature::advanced_simd);
	if (form.width == 16U)
		requirements = requirements | ArmFeature::fp16;
	if (is_sized(form.operation))
		requirements = requirements | ArmFeature::frintts;
	return requirements;
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
	return bits == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] constexpr std::uint64_t sign_mask(const Format &value) noexcept
{
	return std::uint64_t{1} << (value.width - 1U);
}

[[nodiscard]] constexpr std::uint64_t fraction_mask(
	const Format &value) noexcept
{
	return low_mask(value.fraction_bits);
}

[[nodiscard]] constexpr std::uint64_t exponent_mask(
	const Format &value) noexcept
{
	return low_mask(value.exponent_bits) << value.fraction_bits;
}

[[nodiscard]] constexpr std::uint64_t zero_bits(
	const Format &value, bool negative = false) noexcept
{
	return negative ? sign_mask(value) : 0U;
}

[[nodiscard]] constexpr std::uint64_t infinity_bits(
	const Format &value, bool negative = false) noexcept
{
	return (negative ? sign_mask(value) : 0U) | exponent_mask(value);
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
	const Format &value) noexcept
{
	return exponent_mask(value) |
		(std::uint64_t{1} << (value.fraction_bits - 1U));
}

[[nodiscard]] constexpr std::uint64_t power_of_two_bits(
	const Format &value, int exponent, bool negative = false) noexcept
{
	return (negative ? sign_mask(value) : 0U) |
		(static_cast<std::uint64_t>(exponent + value.bias) <<
			value.fraction_bits);
}

/*
 * Encodes significand * 2^binary_exponent. Test values are deliberately
 * exact dyadic rationals, so no host floating-point operation participates
 * in either input generation or the expected-result oracle.
 */
[[nodiscard]] constexpr std::uint64_t dyadic_bits(const Format &value,
	std::uint64_t significand, int binary_exponent,
	bool negative = false) noexcept
{
	if (significand == 0)
		return zero_bits(value, negative);
	const unsigned most_significant =
		63U - static_cast<unsigned>(std::countl_zero(significand));
	const int exponent =
		static_cast<int>(most_significant) + binary_exponent;
	std::uint64_t normalized;
	if (most_significant <= value.fraction_bits)
		normalized = significand <<
			(value.fraction_bits - most_significant);
	else
		normalized = significand >>
			(most_significant - value.fraction_bits);
	return (negative ? sign_mask(value) : 0U) |
		(static_cast<std::uint64_t>(exponent + value.bias) <<
			value.fraction_bits) |
		(normalized & fraction_mask(value));
}

[[nodiscard]] constexpr std::uint64_t minimum_subnormal_bits(
	const Format &value, bool negative = false) noexcept
{
	return (negative ? sign_mask(value) : 0U) | 1U;
}

[[nodiscard]] constexpr std::uint64_t next_below_power_of_two(
	const Format &value, int exponent) noexcept
{
	return power_of_two_bits(value, exponent) - 1U;
}

[[nodiscard]] constexpr std::uint64_t next_above_negative_power_of_two(
	const Format &value, int exponent) noexcept
{
	return sign_mask(value) | (power_of_two_bits(value, exponent) + 1U);
}

[[nodiscard]] inline std::uint64_t read_lane(
	const std::array<std::byte, 16> &bytes, unsigned lane,
	unsigned width)
{
	const unsigned lane_bytes = width / 8U;
	std::uint64_t result = 0;
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		result |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(
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

inline void fill_active_lanes(arm64::CpuState &cpu, unsigned reg,
	const Form &form, std::uint64_t value)
{
	const unsigned lanes = form.scalar ? 1U :
		form.vector_bytes / (form.width / 8U);
	for (unsigned lane = 0; lane < lanes; ++lane)
		write_lane(cpu.vectors[reg].bytes, lane, form.width, value);
}

[[nodiscard]] constexpr Rounding rounding_mode(
	Operation operation, std::uint32_t fpcr) noexcept
{
	switch (operation) {
	case Operation::nearest: return Rounding::ties_even;
	case Operation::plus: return Rounding::plus_infinity;
	case Operation::minus: return Rounding::minus_infinity;
	case Operation::zero:
	case Operation::int32_zero:
	case Operation::int64_zero:
		return Rounding::zero;
	case Operation::ties_away: return Rounding::ties_away;
	case Operation::exact_current:
	case Operation::current:
	case Operation::int32_exact:
	case Operation::int64_exact:
		switch ((fpcr >> 22) & 3U) {
		case 0U: return Rounding::ties_even;
		case 1U: return Rounding::plus_infinity;
		case 2U: return Rounding::minus_infinity;
		default: return Rounding::zero;
		}
	}
	return Rounding::ties_even;
}

[[nodiscard]] constexpr bool standard_exact(Operation operation) noexcept
{
	return operation == Operation::exact_current;
}

[[nodiscard]] constexpr unsigned integer_size(Operation operation) noexcept
{
	return operation == Operation::int32_exact ||
		operation == Operation::int32_zero ? 32U : 64U;
}

[[nodiscard]] constexpr std::uint64_t integer_to_float_bits(
	const Format &value, unsigned __int128 magnitude, bool negative) noexcept
{
	if (magnitude == 0)
		return zero_bits(value, negative);
	unsigned most_significant = 0;
	for (auto remaining = magnitude; remaining > 1U; remaining >>= 1U)
		++most_significant;
	const auto leading = static_cast<std::uint64_t>(
		magnitude - (static_cast<unsigned __int128>(1) <<
			most_significant));
	const auto fraction = leading <<
		(value.fraction_bits - most_significant);
	return (negative ? sign_mask(value) : 0U) |
		(static_cast<std::uint64_t>(
			static_cast<int>(most_significant) + value.bias) <<
			value.fraction_bits) |
		(fraction & fraction_mask(value));
}

[[nodiscard]] constexpr std::uint64_t invalid_sized_result(
	const Format &value, unsigned intsize) noexcept
{
	return power_of_two_bits(value, static_cast<int>(intsize - 1U), true);
}

/*
 * Independent transcription of FPRoundInt and FPRoundIntN. It classifies and
 * rounds the encoded bits directly, never invoking the emulator's decoder,
 * helper, native instruction, or the host floating-point library.
 */
[[nodiscard]] constexpr RoundResult round_bits(std::uint64_t raw,
	const Format &value, Operation operation, std::uint32_t fpcr) noexcept
{
	raw &= low_mask(value.width);
	const bool negative = (raw & sign_mask(value)) != 0U;
	const auto exponent_field =
		(raw & exponent_mask(value)) >> value.fraction_bits;
	const auto fraction = raw & fraction_mask(value);
	const auto maximum_exponent = low_mask(value.exponent_bits);
	const bool sized = is_sized(operation);
	const unsigned intsize = sized ? integer_size(operation) : 0U;

	if (exponent_field == maximum_exponent) {
		if (sized)
			return {invalid_sized_result(value, intsize), fpsr_invalid};
		if (fraction == 0)
			return {raw, 0};
		const bool signaling =
			(fraction &
			 (std::uint64_t{1} << (value.fraction_bits - 1U))) == 0U;
		auto result = raw |
			(std::uint64_t{1} << (value.fraction_bits - 1U));
		if ((fpcr & fpcr_dn) != 0U)
			result = default_nan_bits(value);
		return {result, signaling ? fpsr_invalid : 0U};
	}

	if (exponent_field == 0U) {
		if (fraction == 0U)
			return {zero_bits(value, negative), 0};
		const bool flush = value.width == 16U ?
			(fpcr & fpcr_fz16) != 0U :
			(fpcr & fpcr_fz) != 0U;
		if (flush)
			return {zero_bits(value, negative),
				value.width == 16U ? 0U : fpsr_input_denormal};
	}

	const int exponent = exponent_field == 0U ?
		1 - value.bias :
		static_cast<int>(exponent_field) - value.bias;
	const unsigned __int128 significand = exponent_field == 0U ?
		fraction :
		(static_cast<unsigned __int128>(1) << value.fraction_bits) |
			fraction;

	/* Values with this exponent already have no fractional component. */
	if (exponent >= static_cast<int>(value.fraction_bits)) {
		if (!sized)
			return {raw, 0};
		const int limit_exponent = static_cast<int>(intsize - 1U);
		const bool valid = exponent < limit_exponent ||
			(exponent == limit_exponent && negative && fraction == 0U);
		return valid ? RoundResult{raw, 0} :
			RoundResult{invalid_sized_result(value, intsize),
				fpsr_invalid};
	}

	const unsigned shift = static_cast<unsigned>(
		static_cast<int>(value.fraction_bits) - exponent);
	const unsigned __int128 whole = shift >= 128U ? 0U :
		significand >> shift;
	const unsigned __int128 remainder = shift >= 128U ?
		significand :
		significand &
			((static_cast<unsigned __int128>(1) << shift) - 1U);
	const bool inexact = remainder != 0U;
	bool increment = false;
	switch (rounding_mode(operation, fpcr)) {
	case Rounding::ties_even:
		if (shift < 128U) {
			const auto half =
				static_cast<unsigned __int128>(1) << (shift - 1U);
			increment = remainder > half ||
				(remainder == half && (whole & 1U) != 0U);
		}
		break;
	case Rounding::plus_infinity:
		increment = !negative && inexact;
		break;
	case Rounding::minus_infinity:
		increment = negative && inexact;
		break;
	case Rounding::zero:
		break;
	case Rounding::ties_away:
		if (shift < 128U)
			increment = remainder >=
				(static_cast<unsigned __int128>(1) << (shift - 1U));
		break;
	}
	const unsigned __int128 magnitude = whole + (increment ? 1U : 0U);

	if (sized) {
		const auto positive_limit =
			(static_cast<unsigned __int128>(1) << (intsize - 1U)) - 1U;
		const auto negative_limit =
			static_cast<unsigned __int128>(1) << (intsize - 1U);
		if ((!negative && magnitude > positive_limit) ||
		    (negative && magnitude > negative_limit))
			return {invalid_sized_result(value, intsize), fpsr_invalid};
	}

	const auto result = integer_to_float_bits(value, magnitude, negative);
	const bool raise_inexact = inexact &&
		(sized || standard_exact(operation));
	return {result, raise_inexact ? fpsr_inexact : 0U};
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, const Form &form, Registers registers)
{
	const auto source = initial.vectors[registers.source].bytes;
	std::array<std::byte, 16> result{};
	if (form.scalar && (initial.fpcr & fpcr_nep) != 0U)
		result = initial.vectors[registers.destination].bytes;
	auto expected = initial;
	const unsigned lanes = form.scalar ? 1U :
		form.vector_bytes / (form.width / 8U);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto rounded = round_bits(
			read_lane(source, lane, form.width), format(form.width),
			form.operation, initial.fpcr);
		write_lane(result, lane, form.width, rounded.bits);
		expected.fpsr |= rounded.flags;
	}
	expected.vectors[registers.destination].bytes = result;
	return expected;
}

static_assert(dyadic_bits(format(16), 5U, -1) == 0x4100U);
static_assert(dyadic_bits(format(32), 5U, -1) == 0x4020'0000U);
static_assert(dyadic_bits(format(64), 5U, -1) ==
	0x4004'0000'0000'0000ULL);
static_assert(round_bits(0x4020'0000U, format(32), Operation::nearest, 0).bits ==
	0x4000'0000U);
static_assert(round_bits(0x4020'0000U, format(32), Operation::ties_away, 0).bits ==
	0x4040'0000U);
static_assert(round_bits(0xc020'0000U, format(32), Operation::plus, 0).bits ==
	0xc000'0000U);
static_assert(round_bits(0xc020'0000U, format(32), Operation::minus, 0).bits ==
	0xc040'0000U);
static_assert(round_bits(0x7f80'0055U, format(32), Operation::nearest, 0).flags ==
	fpsr_invalid);
static_assert(round_bits(0x7fc0'0055U, format(32), Operation::nearest, fpcr_dn)
	.bits == 0x7fc0'0000U);
static_assert(round_bits(0x4f00'0000U, format(32), Operation::int32_zero, 0)
	.flags == fpsr_invalid);
static_assert(round_bits(0xcf00'0000U, format(32), Operation::int32_zero, 0)
	.bits == 0xcf00'0000U);

} // namespace arm64_floating_round_test
