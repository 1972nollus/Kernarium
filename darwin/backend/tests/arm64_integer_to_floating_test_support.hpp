// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_integer_to_floating_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

inline constexpr std::uint32_t fpsr_overflow = 1U << 2;
inline constexpr std::uint32_t fpsr_underflow = 1U << 3;
inline constexpr std::uint32_t fpsr_inexact = 1U << 4;
inline constexpr std::uint32_t fpsr_saturation = 1U << 27;

inline constexpr std::uint32_t fpcr_fiz = 1U << 0;
inline constexpr std::uint32_t fpcr_ah = 1U << 1;
inline constexpr std::uint32_t fpcr_nep = 1U << 2;
inline constexpr std::uint32_t fpcr_fz16 = 1U << 19;
inline constexpr std::uint32_t fpcr_fz = 1U << 24;
inline constexpr std::uint32_t fpcr_dn = 1U << 25;
inline constexpr std::uint32_t fpcr_ahp = 1U << 26;

enum class Encoding {
	scalar_simd,
	scalar_fixed,
	scalar_integer,
	simd_fixed,
	simd_integer,
};

struct Form final {
	Encoding encoding;
	bool unsigned_source;
	bool scalar;
	unsigned integer_width;
	unsigned destination_width;
	unsigned vector_bytes;
	unsigned fractional_bits;
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

struct Conversion final {
	std::uint64_t bits;
	std::uint32_t flags;
};

struct Shifted final {
	std::uint64_t value;
	bool inexact;
};

inline constexpr std::array register_edges{
	Registers{0U, 1U}, Registers{1U, 0U}, Registers{1U, 1U},
	Registers{30U, 31U}, Registers{31U, 30U}, Registers{31U, 31U}};

struct SimdShape final {
	bool scalar;
	unsigned width;
	unsigned bytes;
};

inline constexpr std::array simd_shapes{
	SimdShape{true, 16U, 2U}, SimdShape{true, 32U, 4U},
	SimdShape{true, 64U, 8U}, SimdShape{false, 16U, 8U},
	SimdShape{false, 16U, 16U}, SimdShape{false, 32U, 8U},
	SimdShape{false, 32U, 16U}, SimdShape{false, 64U, 16U}};

[[nodiscard]] constexpr bool source_is_gpr(const Form &form) noexcept
{
	return form.encoding == Encoding::scalar_fixed ||
		form.encoding == Encoding::scalar_integer;
}

[[nodiscard]] constexpr bool is_fixed(const Form &form) noexcept
{
	return form.encoding == Encoding::scalar_fixed ||
		form.encoding == Encoding::simd_fixed;
}

[[nodiscard]] constexpr bool is_cross_width(const Form &form) noexcept
{
	return form.encoding == Encoding::scalar_simd;
}

template<typename Function>
void for_each_nonfixed_form(Function &&function)
{
	for (const bool unsigned_source : {false, true}) {
		for (const auto widths : std::array{
			     std::array{32U, 16U}, std::array{32U, 64U},
			     std::array{64U, 16U}, std::array{64U, 32U}})
			function(Form{Encoding::scalar_simd, unsigned_source, true,
				widths[0], widths[1], widths[1] / 8U, 0U});
		for (const unsigned integer_width : {32U, 64U})
			for (const unsigned destination_width : {16U, 32U, 64U})
				function(Form{Encoding::scalar_integer,
					unsigned_source, true, integer_width,
					destination_width, destination_width / 8U, 0U});
		for (const auto shape : simd_shapes)
			function(Form{Encoding::simd_integer, unsigned_source,
				shape.scalar, shape.width, shape.width, shape.bytes, 0U});
	}
}

template<typename Function>
void for_each_fixed_form(Function &&function, bool every_fraction)
{
	for (const bool unsigned_source : {false, true}) {
		for (const unsigned integer_width : {32U, 64U})
			for (const unsigned destination_width : {16U, 32U, 64U}) {
				const unsigned first = every_fraction ? 1U :
					integer_width / 2U;
				const unsigned last = every_fraction ?
					integer_width : first;
				for (unsigned fractional_bits = first;
				     fractional_bits <= last; ++fractional_bits)
					function(Form{Encoding::scalar_fixed,
						unsigned_source, true, integer_width,
						destination_width,
						destination_width / 8U,
						fractional_bits});
			}
		for (const auto shape : simd_shapes) {
			const unsigned first = every_fraction ? 1U :
				shape.width / 2U;
			const unsigned last = every_fraction ? shape.width : first;
			for (unsigned fractional_bits = first;
			     fractional_bits <= last; ++fractional_bits)
				function(Form{Encoding::simd_fixed, unsigned_source,
					shape.scalar, shape.width, shape.width,
					shape.bytes, fractional_bits});
		}
	}
}

template<typename Function>
void for_each_form(Function &&function, bool every_fraction = true)
{
	for_each_nonfixed_form(function);
	for_each_fixed_form(function, every_fraction);
}

[[nodiscard]] constexpr unsigned scalar_type(unsigned width) noexcept
{
	return width == 16U ? 3U : width == 32U ? 0U : 1U;
}

[[nodiscard]] constexpr std::uint32_t instruction(
	const Form &form, Registers registers = {0U, 1U}) noexcept
{
	const auto fields = ((registers.source & 31U) << 5) |
		(registers.destination & 31U);
	switch (form.encoding) {
	case Encoding::scalar_simd:
		return 0x1e20'0000U |
			(form.integer_width == 64U ? 0x8000'0000U : 0U) |
			(scalar_type(form.destination_width) << 22) |
			(3U << 19) | ((form.unsigned_source ? 5U : 4U) << 16) |
			fields;
	case Encoding::scalar_fixed:
		return 0x1e00'0000U |
			(form.integer_width == 64U ? 0x8000'0000U : 0U) |
			(scalar_type(form.destination_width) << 22) |
			(form.unsigned_source ? 3U : 2U) << 16 |
			((64U - form.fractional_bits) << 10) | fields;
	case Encoding::scalar_integer:
		return 0x1e20'0000U |
			(form.integer_width == 64U ? 0x8000'0000U : 0U) |
			(scalar_type(form.destination_width) << 22) |
			(form.unsigned_source ? 3U : 2U) << 16 | fields;
	case Encoding::simd_fixed: {
		const auto immediate =
			2U * form.integer_width - form.fractional_bits;
		return (form.scalar ? 0x5f00'e400U : 0x0f00'e400U) |
			(form.unsigned_source ? 0x2000'0000U : 0U) |
			(!form.scalar && form.vector_bytes == 16U ?
				0x4000'0000U : 0U) |
			(immediate << 16) | fields;
	}
	case Encoding::simd_integer:
		if (form.destination_width == 16U)
			return (form.scalar ? 0x5e79'd800U : 0x0e79'd800U) |
				(form.unsigned_source ? 0x2000'0000U : 0U) |
				(!form.scalar && form.vector_bytes == 16U ?
					0x4000'0000U : 0U) |
				fields;
		return (form.scalar ? 0x5e21'd800U : 0x0e21'd800U) |
			(form.unsigned_source ? 0x2000'0000U : 0U) |
			(form.destination_width == 64U ? 0x0040'0000U : 0U) |
			(!form.scalar && form.vector_bytes == 16U ?
				0x4000'0000U : 0U) |
			fields;
	}
	return 0U;
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(const Form &form)
{
	using uml::backend::ArmFeature;
	if (is_cross_width(form))
		return engine_tests::NativeRequirements::features(
			ArmFeature::fprcvt);
	auto requirements = engine_tests::NativeRequirements::features(
		source_is_gpr(form) ? ArmFeature::floating_point :
			ArmFeature::advanced_simd);
	if (form.destination_width == 16U)
		requirements = requirements | ArmFeature::fp16;
	return requirements;
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
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
	cpu.fpcr = 0U;
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
	cpu.pstate_sm = false;
	cpu.pstate_za = false;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xbU;
	cpu.exclusive_address = 0x1234'5000ULL;
	cpu.exclusive_key = 0x2468U;
	cpu.exclusive_value = 0x1122'3344'5566'7788ULL;
	cpu.exclusive_value2 = 0x8877'6655'4433'2211ULL;
	cpu.exclusive_version = 0x1020'3040'5060'7080ULL;
	cpu.exclusive_size = 8U;
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

[[nodiscard]] constexpr std::uint64_t sign_mask(unsigned width) noexcept
{
	return std::uint64_t{1} << (width - 1U);
}

[[nodiscard]] constexpr std::uint64_t infinity(
	const Format &destination, bool negative) noexcept
{
	return (negative ? sign_mask(destination.width) : 0U) |
		(low_mask(destination.exponent_bits) <<
			destination.fraction_bits);
}

[[nodiscard]] constexpr std::uint64_t maximum_normal(
	const Format &destination, bool negative) noexcept
{
	return (negative ? sign_mask(destination.width) : 0U) |
		((low_mask(destination.exponent_bits) - 1U) <<
			destination.fraction_bits) |
		low_mask(destination.fraction_bits);
}

[[nodiscard]] constexpr Shifted round_shift(
	std::uint64_t magnitude, int shift, unsigned rounding,
	bool negative) noexcept
{
	if (shift <= 0)
		return {magnitude << static_cast<unsigned>(-shift), false};
	const auto wide = static_cast<unsigned __int128>(magnitude);
	const auto amount = static_cast<unsigned>(shift);
	const auto truncated = amount < 128U ? wide >> amount : 0U;
	const auto remainder = amount < 128U ?
		wide - (truncated << amount) : wide;
	if (remainder == 0U)
		return {static_cast<std::uint64_t>(truncated), false};
	bool increment{};
	if (rounding == 0U && amount < 128U) {
		const auto halfway =
			static_cast<unsigned __int128>(1) << (amount - 1U);
		increment = remainder > halfway ||
			(remainder == halfway && (truncated & 1U) != 0U);
	} else if (rounding == 1U) {
		increment = !negative;
	} else if (rounding == 2U) {
		increment = negative;
	}
	return {static_cast<std::uint64_t>(
		truncated + static_cast<unsigned>(increment)), true};
}

/*
 * Independent FixedToFP oracle. It operates only on exact integers and
 * powers of two; it neither calls the emulator conversion helper nor uses
 * host floating-point conversion instructions.
 */
[[nodiscard]] constexpr Conversion convert(
	std::uint64_t input, unsigned integer_width,
	unsigned destination_width, unsigned fractional_bits,
	bool unsigned_source, std::uint32_t fpcr) noexcept
{
	const auto integer_mask = low_mask(integer_width);
	input &= integer_mask;
	const bool negative = !unsigned_source &&
		(input & sign_mask(integer_width)) != 0U;
	const std::uint64_t magnitude = negative ?
		(~input + 1U) & integer_mask : input;
	if (magnitude == 0U)
		return {0U, 0U};

	const auto destination = format(destination_width);
	const auto sign = negative ? sign_mask(destination_width) : 0U;
	const unsigned rounding = (fpcr >> 22) & 3U;
	const int highest =
		static_cast<int>(std::bit_width(magnitude)) - 1;
	const int exponent =
		highest - static_cast<int>(fractional_bits);
	const int minimum_exponent = 1 - destination.bias;
	std::uint32_t flags{};

	if (exponent >= minimum_exponent) {
		auto significand = round_shift(magnitude,
			highest - static_cast<int>(destination.fraction_bits),
			rounding, negative);
		int biased_exponent = exponent + destination.bias;
		if (significand.value ==
		    (std::uint64_t{1} << (destination.fraction_bits + 1U))) {
			significand.value >>= 1U;
			++biased_exponent;
		}
		if (biased_exponent >=
		    static_cast<int>(low_mask(destination.exponent_bits))) {
			const bool to_infinity = rounding == 0U ||
				(rounding == 1U && !negative) ||
				(rounding == 2U && negative);
			return {to_infinity ? infinity(destination, negative) :
				maximum_normal(destination, negative),
				fpsr_overflow | fpsr_inexact};
		}
		if (significand.inexact)
			flags |= fpsr_inexact;
		return {sign |
			(static_cast<std::uint64_t>(biased_exponent) <<
				destination.fraction_bits) |
			(significand.value & low_mask(destination.fraction_bits)),
			flags};
	}

	const bool alternative = (fpcr & fpcr_ah) != 0U;
	const bool flush = destination_width == 16U ?
		(fpcr & fpcr_fz16) != 0U : (fpcr & fpcr_fz) != 0U;
	if (!alternative && flush)
		return {sign, fpsr_underflow};

	const int unit_exponent =
		minimum_exponent - static_cast<int>(destination.fraction_bits);
	const auto significand = round_shift(magnitude,
		unit_exponent + static_cast<int>(fractional_bits),
		rounding, negative);
	const bool became_normal = significand.value >=
		(std::uint64_t{1} << destination.fraction_bits);
	if (alternative && !became_normal && flush)
		return {sign, fpsr_underflow | fpsr_inexact};
	if (significand.inexact) {
		flags |= fpsr_inexact;
		if (!alternative || !became_normal)
			flags |= fpsr_underflow;
	}
	return {sign | (became_normal ?
		(std::uint64_t{1} << destination.fraction_bits) :
		significand.value), flags};
}

[[nodiscard]] inline std::uint64_t read_lane(
	const std::array<std::byte, 16> &bytes, unsigned lane,
	unsigned width)
{
	const unsigned lane_bytes = width / 8U;
	std::uint64_t result{};
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

[[nodiscard]] constexpr unsigned lanes(const Form &form) noexcept
{
	return form.scalar ? 1U :
		form.vector_bytes / (form.integer_width / 8U);
}

inline void fill_source(arm64::CpuState &cpu, const Form &form,
	Registers registers, std::uint64_t primary,
	bool vary_lanes = true)
{
	if (source_is_gpr(form)) {
		if (registers.source != 31U)
			cpu.registers[registers.source] = primary;
		return;
	}
	const auto mask = low_mask(form.integer_width);
	for (unsigned lane = 0; lane < lanes(form); ++lane) {
		auto value = primary & mask;
		if (vary_lanes) {
			switch (lane & 3U) {
			case 1U:
				value = 0U;
				break;
			case 2U:
				value = mask;
				break;
			case 3U:
				value ^= sign_mask(form.integer_width);
				break;
			default:
				break;
			}
		}
		write_lane(cpu.vectors[registers.source].bytes, lane,
			form.integer_width, value);
	}
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, const Form &form,
	Registers registers)
{
	const auto source = source_is_gpr(form) ?
		std::array<std::byte, 16>{} :
		initial.vectors[registers.source].bytes;
	const auto gpr = source_is_gpr(form) && registers.source != 31U ?
		initial.registers[registers.source] : 0U;
	auto result = form.scalar && (initial.fpcr & fpcr_nep) != 0U ?
		initial.vectors[registers.destination].bytes :
		std::array<std::byte, 16>{};
	auto expected = initial;
	for (unsigned lane = 0; lane < lanes(form); ++lane) {
		const auto input = source_is_gpr(form) ? gpr :
			read_lane(source, lane, form.integer_width);
		const auto converted = convert(input, form.integer_width,
			form.destination_width, form.fractional_bits,
			form.unsigned_source, initial.fpcr);
		write_lane(result, lane, form.destination_width, converted.bits);
		expected.fpsr |= converted.flags;
	}
	expected.vectors[registers.destination].bytes = result;
	return expected;
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, const Form &form, Registers registers,
	const arm64::CpuState &initial)
{
	harness.check_one(name, instruction(form, registers), initial,
		expected_state(initial, form, registers),
		native_requirements(form));
}

static_assert(instruction(
	Form{Encoding::scalar_integer, false, true, 64U, 32U, 4U, 0U},
	{0U, 0U}) ==
	0x9e22'0000U);
static_assert(instruction(
	Form{Encoding::simd_integer, true, false, 32U, 32U, 16U, 0U},
	{0U, 0U}) ==
	0x6e21'd800U);
static_assert(convert(1U, 32U, 16U, 0U, false, 0U).bits == 0x3c00U);
static_assert(convert(0xffff'ffffU, 32U, 16U, 0U, false, 0U).bits ==
	0xbc00U);
static_assert(convert(1U, 32U, 16U, 24U, false, 0U).bits == 1U);
static_assert(convert(1U, 32U, 16U, 24U, false, fpcr_fz16).bits == 0U);
static_assert(convert(1U, 32U, 16U, 24U, false, fpcr_fz16).flags ==
	fpsr_underflow);
static_assert(convert(0xffff'ffffU, 32U, 32U, 0U, true, 0U).bits ==
	0x4f80'0000U);
static_assert(convert(0xffff'ffffU, 32U, 32U, 0U, true, 0U).flags ==
	fpsr_inexact);
static_assert(convert(0xffff'ffff'ffff'ffffULL, 64U, 16U, 0U,
	true, fpcr_ahp).flags == (fpsr_overflow | fpsr_inexact));

} // namespace arm64_integer_to_floating_test
