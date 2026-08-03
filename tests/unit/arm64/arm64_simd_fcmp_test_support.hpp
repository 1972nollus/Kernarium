// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_simd_fcsel_test_support.hpp"
#include "cpu/arm64/detail/instruction_helpers.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_simd_fcmp_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

inline constexpr std::uint32_t fpsr_invalid = 1U << 0;
inline constexpr std::uint32_t fpsr_input_denormal = 1U << 7;

struct Format final {
	unsigned bytes;
	unsigned type;
	std::uint64_t sign;
	std::uint64_t fraction;
	std::uint64_t exponent;
	std::uint64_t quiet;
	std::uint64_t positive_one;
	std::uint64_t positive_two;
	std::uint64_t positive_infinity;
	std::uint64_t maximum_finite;
	std::uint64_t minimum_normal;
	std::uint64_t minimum_subnormal;
	std::uint64_t quiet_nan;
	std::uint64_t signaling_nan;
};

inline constexpr std::array formats{
	Format{
		.bytes = 2U,
		.type = 3U,
		.sign = 0x8000U,
		.fraction = 0x03ffU,
		.exponent = 0x7c00U,
		.quiet = 0x0200U,
		.positive_one = 0x3c00U,
		.positive_two = 0x4000U,
		.positive_infinity = 0x7c00U,
		.maximum_finite = 0x7bffU,
		.minimum_normal = 0x0400U,
		.minimum_subnormal = 0x0001U,
		.quiet_nan = 0x7e51U,
		.signaling_nan = 0x7d51U,
	},
	Format{
		.bytes = 4U,
		.type = 0U,
		.sign = 0x8000'0000U,
		.fraction = 0x007f'ffffU,
		.exponent = 0x7f80'0000U,
		.quiet = 0x0040'0000U,
		.positive_one = 0x3f80'0000U,
		.positive_two = 0x4000'0000U,
		.positive_infinity = 0x7f80'0000U,
		.maximum_finite = 0x7f7f'ffffU,
		.minimum_normal = 0x0080'0000U,
		.minimum_subnormal = 0x0000'0001U,
		.quiet_nan = 0x7fc1'2345U,
		.signaling_nan = 0x7f81'2345U,
	},
	Format{
		.bytes = 8U,
		.type = 1U,
		.sign = 0x8000'0000'0000'0000ULL,
		.fraction = 0x000f'ffff'ffff'ffffULL,
		.exponent = 0x7ff0'0000'0000'0000ULL,
		.quiet = 0x0008'0000'0000'0000ULL,
		.positive_one = 0x3ff0'0000'0000'0000ULL,
		.positive_two = 0x4000'0000'0000'0000ULL,
		.positive_infinity = 0x7ff0'0000'0000'0000ULL,
		.maximum_finite = 0x7fef'ffff'ffff'ffffULL,
		.minimum_normal = 0x0010'0000'0000'0000ULL,
		.minimum_subnormal = 0x0000'0000'0000'0001ULL,
		.quiet_nan = 0x7ff8'1234'5678'9abcULL,
		.signaling_nan = 0x7ff0'1234'5678'9abcULL,
	},
};

[[nodiscard]] constexpr const Format &format_for_type(unsigned type)
{
	return type == 3U ? formats[0] : type == 0U ? formats[1] : formats[2];
}

[[nodiscard]] constexpr std::uint32_t direct_raw_instruction(unsigned type,
	unsigned right, unsigned left, unsigned low)
{
	return 0x1e20'2000U | ((type & 3U) << 22) |
		((right & 31U) << 16) | ((left & 31U) << 5) | (low & 31U);
}

[[nodiscard]] constexpr std::uint32_t direct_instruction(
	const Format &format, bool signaling, bool zero, unsigned left,
	unsigned right = 0U)
{
	return direct_raw_instruction(format.type, zero ? 0U : right, left,
		(signaling ? 16U : 0U) | (zero ? 8U : 0U));
}

[[nodiscard]] constexpr bool direct_valid(
	unsigned type, unsigned right, unsigned low)
{
	return type != 2U && (low & 7U) == 0U &&
		((low & 8U) == 0U || right == 0U);
}

[[nodiscard]] constexpr std::uint32_t conditional_raw_instruction(
	unsigned type, unsigned right, unsigned condition, unsigned left,
	unsigned low)
{
	return 0x1e20'0400U | ((type & 3U) << 22) |
		((right & 31U) << 16) | ((condition & 15U) << 12) |
		((left & 31U) << 5) | (low & 31U);
}

[[nodiscard]] constexpr std::uint32_t conditional_instruction(
	const Format &format, bool signaling, unsigned condition,
	unsigned fallback, unsigned left, unsigned right)
{
	return conditional_raw_instruction(format.type, right, condition, left,
		(signaling ? 16U : 0U) | (fallback & 15U));
}

[[nodiscard]] constexpr bool conditional_valid(unsigned type)
{
	return type != 2U;
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	auto cpu = arm64_simd_fcsel_test::state(seed);
	cpu.fpcr = 0U;
	cpu.fpsr = seed & 1U;
	return cpu;
}

inline void put(arm64::CpuState &cpu, unsigned reg, const Format &format,
	std::uint64_t bits)
{
	arm64::detail::encode_little_endian(bits,
		std::span{cpu.vectors[reg].bytes}.first(format.bytes));
}

[[nodiscard]] inline std::uint64_t get(const arm64::CpuState &cpu,
	unsigned reg, const Format &format)
{
	return arm64::detail::decode_little_endian(
		std::span{cpu.vectors[reg].bytes}.first(format.bytes));
}

/*
 * Test-only transcription of the architectural ConditionHolds() operation.
 * It intentionally does not call the emulator's condition helper.
 */
[[nodiscard]] constexpr bool condition_holds(
	std::uint32_t nzcv, unsigned condition)
{
	const bool negative = (nzcv & 0x8000'0000U) != 0U;
	const bool zero = (nzcv & 0x4000'0000U) != 0U;
	const bool carry = (nzcv & 0x2000'0000U) != 0U;
	const bool overflow = (nzcv & 0x1000'0000U) != 0U;
	bool result;
	switch ((condition >> 1U) & 7U) {
	case 0U: result = zero; break;
	case 1U: result = carry; break;
	case 2U: result = negative; break;
	case 3U: result = overflow; break;
	case 4U: result = carry && !zero; break;
	case 5U: result = negative == overflow; break;
	case 6U: result = !zero && negative == overflow; break;
	default: result = true; break;
	}
	if ((condition & 1U) != 0U && (condition & 14U) != 14U)
		result = !result;
	return result;
}

enum class ValueClass : std::uint8_t {
	zero,
	subnormal,
	finite,
	infinity,
	quiet_nan,
	signaling_nan,
};

struct Unpacked final {
	std::uint64_t bits;
	ValueClass value_class;
};

struct Comparison final {
	std::uint32_t nzcv;
	std::uint32_t exceptions;
};

/*
 * Test-only specification oracle for FPUnpack followed by FPCompare. This is
 * deliberately kept separate from detail/floating_compare.hpp.
 */
[[nodiscard]] constexpr Unpacked unpack(
	std::uint64_t bits, const Format &format, std::uint32_t fpcr,
	std::uint32_t &exceptions)
{
	const auto value_mask = format.sign | format.exponent | format.fraction;
	bits &= value_mask;
	const auto exponent = bits & format.exponent;
	const auto fraction = bits & format.fraction;
	if (exponent == 0U) {
		if (fraction == 0U)
			return {bits, ValueClass::zero};
		if (format.bytes == 2U) {
			if ((fpcr & (1U << 19)) != 0U)
				return {bits & format.sign, ValueClass::zero};
			return {bits, ValueClass::subnormal};
		}
		const bool alternative_handling = (fpcr & (1U << 1)) != 0U;
		const bool flush_input = (fpcr & 1U) != 0U;
		const bool standard_flush =
			(fpcr & (1U << 24)) != 0U && !alternative_handling;
		if (standard_flush || flush_input) {
			if (standard_flush)
				exceptions |= fpsr_input_denormal;
			return {bits & format.sign, ValueClass::zero};
		}
		return {bits, ValueClass::subnormal};
	}
	if (exponent != format.exponent)
		return {bits, ValueClass::finite};
	if (fraction == 0U)
		return {bits, ValueClass::infinity};
	return {
		bits,
		(fraction & format.quiet) != 0U ?
			ValueClass::quiet_nan : ValueClass::signaling_nan,
	};
}

[[nodiscard]] constexpr Comparison compare(
	std::uint64_t left_bits, std::uint64_t right_bits, const Format &format,
	bool signal_all_nans, std::uint32_t fpcr)
{
	std::uint32_t exceptions = 0U;
	const auto left = unpack(left_bits, format, fpcr, exceptions);
	const auto right = unpack(right_bits, format, fpcr, exceptions);
	const auto is_nan = [](ValueClass value_class) constexpr {
		return value_class == ValueClass::quiet_nan ||
			value_class == ValueClass::signaling_nan;
	};
	if (is_nan(left.value_class) || is_nan(right.value_class)) {
		if (signal_all_nans ||
		    left.value_class == ValueClass::signaling_nan ||
		    right.value_class == ValueClass::signaling_nan)
			exceptions |= fpsr_invalid;
		return {0x3000'0000U, exceptions};
	}

	const bool alternative_handling =
		format.bytes != 2U && (fpcr & (1U << 1)) != 0U;
	const bool flush_input = (fpcr & 1U) != 0U;
	if (alternative_handling && !flush_input &&
	    (left.value_class == ValueClass::subnormal ||
	     right.value_class == ValueClass::subnormal))
		exceptions |= fpsr_input_denormal;

	const auto magnitude_mask = format.sign - 1U;
	const auto left_magnitude = left.bits & magnitude_mask;
	const auto right_magnitude = right.bits & magnitude_mask;
	if ((left_magnitude | right_magnitude) == 0U ||
	    left.bits == right.bits)
		return {0x6000'0000U, exceptions};
	const bool left_negative = (left.bits & format.sign) != 0U;
	const bool right_negative = (right.bits & format.sign) != 0U;
	const bool less = left_negative != right_negative ? left_negative :
		left_negative ? left_magnitude > right_magnitude :
			left_magnitude < right_magnitude;
	return {less ? 0x8000'0000U : 0x2000'0000U, exceptions};
}

[[nodiscard]] inline arm64::CpuState expected_direct(
	const arm64::CpuState &initial, const Format &format, bool signaling,
	bool zero, unsigned left, unsigned right)
{
	auto expected = initial;
	const auto result = compare(get(initial, left, format),
		zero ? 0U : get(initial, right, format), format, signaling,
		initial.fpcr);
	expected.nzcv = result.nzcv;
	expected.fpsr |= result.exceptions;
	return expected;
}

[[nodiscard]] inline arm64::CpuState expected_conditional(
	const arm64::CpuState &initial, const Format &format, bool signaling,
	unsigned condition, unsigned fallback, unsigned left, unsigned right)
{
	auto expected = initial;
	if (!condition_holds(initial.nzcv, condition)) {
		expected.nzcv = (fallback & 15U) << 28;
		return expected;
	}
	const auto result = compare(get(initial, left, format),
		get(initial, right, format), format, signaling, initial.fpcr);
	expected.nzcv = result.nzcv;
	expected.fpsr |= result.exceptions;
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements(
	const Format &format, std::uint32_t fpcr = 0U)
{
	using ::uml::cpu::ArmFeature;
	auto requirements = engine_tests::NativeRequirements::features(
		ArmFeature::floating_point);
	if (format.bytes == 2U)
		requirements = requirements | ArmFeature::fp16;
	if (format.bytes != 2U && (fpcr & 3U) != 0U)
		requirements = requirements | ArmFeature::afp;
	return requirements;
}

inline void check_direct(engine_tests::EngineHarness &harness,
	std::string_view name, const Format &format, bool signaling, bool zero,
	unsigned left, unsigned right, const arm64::CpuState &initial)
{
	harness.check_one(name,
		direct_instruction(format, signaling, zero, left, right), initial,
		expected_direct(initial, format, signaling, zero, left, right),
		native_requirements(format, initial.fpcr));
}

inline void check_conditional(engine_tests::EngineHarness &harness,
	std::string_view name, const Format &format, bool signaling,
	unsigned condition, unsigned fallback, unsigned left, unsigned right,
	const arm64::CpuState &initial)
{
	harness.check_one(name,
		conditional_instruction(format, signaling, condition, fallback,
			left, right),
		initial,
		expected_conditional(initial, format, signaling, condition,
			fallback, left, right),
		native_requirements(format, initial.fpcr));
}

} // namespace arm64_simd_fcmp_test
