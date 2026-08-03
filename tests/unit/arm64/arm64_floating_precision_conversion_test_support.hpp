// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_floating_precision_conversion_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

inline constexpr std::uint32_t fpsr_invalid = 1U << 0;
inline constexpr std::uint32_t fpsr_overflow = 1U << 2;
inline constexpr std::uint32_t fpsr_underflow = 1U << 3;
inline constexpr std::uint32_t fpsr_inexact = 1U << 4;
inline constexpr std::uint32_t fpsr_input_denormal = 1U << 7;

inline constexpr std::uint32_t fpcr_fiz = 1U << 0;
inline constexpr std::uint32_t fpcr_ah = 1U << 1;
inline constexpr std::uint32_t fpcr_nep = 1U << 2;
inline constexpr std::uint32_t fpcr_fz16 = 1U << 19;
inline constexpr std::uint32_t fpcr_fz = 1U << 24;
inline constexpr std::uint32_t fpcr_dn = 1U << 25;
inline constexpr std::uint32_t fpcr_ahp = 1U << 26;

enum class Operation : std::uint8_t {
	widen,
	narrow,
	round_odd,
};

struct Form final {
	Operation operation;
	unsigned source_width;
	unsigned destination_width;
	bool upper;
	bool scalar;
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

enum class FloatingClass : std::uint8_t {
	zero,
	denormal,
	finite,
	infinity,
	quiet_nan,
	signaling_nan,
};

struct Unpacked final {
	std::uint64_t original;
	std::uint64_t significand;
	int exponent;
	FloatingClass type;
	bool negative;
};

struct Conversion final {
	std::uint64_t bits;
	std::uint32_t flags;
};

inline constexpr std::array ordinary_forms{
	Form{Operation::widen, 16U, 32U, false, false},
	Form{Operation::widen, 16U, 32U, true, false},
	Form{Operation::widen, 32U, 64U, false, false},
	Form{Operation::widen, 32U, 64U, true, false},
	Form{Operation::narrow, 32U, 16U, false, false},
	Form{Operation::narrow, 32U, 16U, true, false},
	Form{Operation::narrow, 64U, 32U, false, false},
	Form{Operation::narrow, 64U, 32U, true, false},
};

inline constexpr std::array round_odd_forms{
	Form{Operation::round_odd, 64U, 32U, false, true},
	Form{Operation::round_odd, 64U, 32U, false, false},
	Form{Operation::round_odd, 64U, 32U, true, false},
};

inline constexpr std::array register_edges{
	Registers{0U, 1U}, Registers{1U, 0U}, Registers{1U, 1U},
	Registers{30U, 31U}, Registers{31U, 30U}, Registers{31U, 31U},
};

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

[[nodiscard]] constexpr unsigned lanes(const Form &form) noexcept
{
	if (form.scalar)
		return 1U;
	return (form.operation == Operation::widen ? 64U : 128U) /
		form.source_width;
}

[[nodiscard]] constexpr unsigned source_lane_base(const Form &form) noexcept
{
	return form.operation == Operation::widen && form.upper ?
		64U / form.source_width : 0U;
}

[[nodiscard]] constexpr unsigned destination_lane_base(
	const Form &form) noexcept
{
	return form.operation != Operation::widen && form.upper ?
		64U / form.destination_width : 0U;
}

[[nodiscard]] constexpr std::uint32_t instruction(
	const Form &form, Registers registers = {0U, 1U}) noexcept
{
	const auto fields = ((registers.source & 31U) << 5) |
		(registers.destination & 31U);
	if (form.operation == Operation::round_odd)
		return (form.scalar ? 0x7e61'6800U : 0x2e61'6800U) |
			(!form.scalar && form.upper ? 0x4000'0000U : 0U) | fields;
	auto encoded = form.operation == Operation::widen ?
		0x0e21'7800U : 0x0e21'6800U;
	if ((form.operation == Operation::widen &&
	     form.source_width == 32U) ||
	    (form.operation == Operation::narrow &&
	     form.source_width == 64U))
		encoded |= 0x0040'0000U;
	if (form.upper)
		encoded |= 0x4000'0000U;
	return encoded | fields;
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(const Form &form, std::uint32_t fpcr)
{
	using ::uml::cpu::ArmFeature;
	auto result = engine_tests::NativeRequirements::features(
		ArmFeature::advanced_simd);
	if ((fpcr & (fpcr_fiz | fpcr_ah)) != 0U ||
	    (form.scalar && (fpcr & fpcr_nep) != 0U))
		result = result | ArmFeature::afp;
	/* The vector precision-conversion allocations require AdvSIMD, not
	 * FEAT_FP16, even when half precision is an operand. */
	return result;
}

[[nodiscard]] constexpr Unpacked unpack_cv(std::uint64_t bits,
	unsigned width, std::uint32_t fpcr, std::uint32_t &flags) noexcept
{
	const auto source = format(width);
	bits &= low_mask(width);
	const bool negative = (bits & sign_mask(width)) != 0U;
	const auto exponent =
		(bits >> source.fraction_bits) & low_mask(source.exponent_bits);
	const auto fraction = bits & low_mask(source.fraction_bits);
	if (exponent == 0U) {
		if (fraction == 0U)
			return {bits, 0U, 0, FloatingClass::zero, negative};
		/* FPUnpackCV clears FZ16. For S/D, FIZ flushes silently and
		 * standard FZ flushes with IDC unless AH overrides it. */
		if (width != 16U) {
			const bool fiz = (fpcr & fpcr_fiz) != 0U;
			const bool fz = (fpcr & fpcr_fz) != 0U &&
				(fpcr & fpcr_ah) == 0U;
			if (fiz || fz) {
				if (fz)
					flags |= fpsr_input_denormal;
				return {negative ? sign_mask(width) : 0U, 0U, 0,
					FloatingClass::zero, negative};
			}
		}
		return {bits, fraction,
			1 - source.bias - static_cast<int>(source.fraction_bits),
			FloatingClass::denormal, negative};
	}
	const auto exponent_ones = low_mask(source.exponent_bits);
	if (exponent == exponent_ones &&
	    !(width == 16U && (fpcr & fpcr_ahp) != 0U)) {
		if (fraction == 0U)
			return {bits, 0U, 0, FloatingClass::infinity, negative};
		const auto quiet = std::uint64_t{1} <<
			(source.fraction_bits - 1U);
		return {bits, 0U, 0,
			(fraction & quiet) != 0U ?
				FloatingClass::quiet_nan :
				FloatingClass::signaling_nan,
			negative};
	}
	return {bits,
		(std::uint64_t{1} << source.fraction_bits) | fraction,
		static_cast<int>(exponent) - source.bias -
			static_cast<int>(source.fraction_bits),
		FloatingClass::finite, negative};
}

struct Rounded final {
	std::uint64_t value;
	bool inexact;
};

[[nodiscard]] constexpr Rounded round_significand(std::uint64_t value,
	int shift, unsigned mode, bool negative) noexcept
{
	if (shift <= 0)
		return {value << static_cast<unsigned>(-shift), false};
	std::uint64_t truncated{};
	std::uint64_t remainder{};
	if (shift < 64) {
		truncated = value >> static_cast<unsigned>(shift);
		remainder = value & low_mask(static_cast<unsigned>(shift));
	} else {
		remainder = value;
	}
	if (remainder == 0U)
		return {truncated, false};
	if (mode == 4U)
		return {truncated | 1U, true};
	bool increment{};
	if (mode == 0U && shift < 64) {
		const auto halfway =
			std::uint64_t{1} << (static_cast<unsigned>(shift) - 1U);
		increment = remainder > halfway ||
			(remainder == halfway && (truncated & 1U) != 0U);
	} else if (mode == 1U) {
		increment = !negative;
	} else if (mode == 2U) {
		increment = negative;
	}
	return {truncated + static_cast<unsigned>(increment), true};
}

[[nodiscard]] constexpr std::uint64_t infinity_bits(
	unsigned width, bool negative) noexcept
{
	const auto destination = format(width);
	return (negative ? sign_mask(width) : 0U) |
		(low_mask(destination.exponent_bits) <<
			destination.fraction_bits);
}

[[nodiscard]] constexpr std::uint64_t maximum_normal_bits(
	unsigned width, bool negative) noexcept
{
	const auto destination = format(width);
	return (negative ? sign_mask(width) : 0U) |
		((low_mask(destination.exponent_bits) - 1U) <<
			destination.fraction_bits) |
		low_mask(destination.fraction_bits);
}

[[nodiscard]] constexpr std::uint64_t converted_nan(
	const Unpacked &source, unsigned source_width,
	unsigned destination_width, std::uint32_t fpcr) noexcept
{
	const auto source_format = format(source_width);
	const auto destination = format(destination_width);
	if ((fpcr & fpcr_dn) != 0U)
		return ((fpcr & fpcr_ah) != 0U ?
			sign_mask(destination_width) : 0U) |
			(low_mask(destination.exponent_bits) <<
				destination.fraction_bits) |
			(std::uint64_t{1} << (destination.fraction_bits - 1U));
	const unsigned source_payload_bits = source_format.fraction_bits - 1U;
	const unsigned destination_payload_bits =
		destination.fraction_bits - 1U;
	const auto source_payload =
		source.original & low_mask(source_payload_bits);
	const auto payload = source_payload_bits > destination_payload_bits ?
		source_payload >>
			(source_payload_bits - destination_payload_bits) :
		source_payload <<
			(destination_payload_bits - source_payload_bits);
	return (source.negative ? sign_mask(destination_width) : 0U) |
		(low_mask(destination.exponent_bits) <<
			destination.fraction_bits) |
		(std::uint64_t{1} << (destination.fraction_bits - 1U)) |
		payload;
}

[[nodiscard]] constexpr Conversion convert(std::uint64_t input,
	unsigned source_width, unsigned destination_width,
	std::uint32_t fpcr, bool round_odd = false) noexcept
{
	std::uint32_t flags{};
	const auto source = unpack_cv(input, source_width, fpcr, flags);
	const auto destination = format(destination_width);
	const auto sign = source.negative ? sign_mask(destination_width) : 0U;
	const bool alternative_half =
		destination_width == 16U && (fpcr & fpcr_ahp) != 0U;
	if (source.type == FloatingClass::quiet_nan ||
	    source.type == FloatingClass::signaling_nan) {
		if (alternative_half)
			return {sign, flags | fpsr_invalid};
		if (source.type == FloatingClass::signaling_nan)
			flags |= fpsr_invalid;
		return {converted_nan(source, source_width, destination_width,
			fpcr), flags};
	}
	if (source.type == FloatingClass::infinity) {
		if (alternative_half)
			return {sign | low_mask(destination_width - 1U),
				flags | fpsr_invalid};
		return {infinity_bits(destination_width, source.negative), flags};
	}
	if (source.type == FloatingClass::zero)
		return {sign, flags};

	const unsigned rounding =
		round_odd ? 4U : (fpcr >> 22) & 3U;
	const int highest =
		static_cast<int>(std::bit_width(source.significand)) - 1;
	const int value_exponent = highest + source.exponent;
	const int minimum_exponent = 1 - destination.bias;
	const auto exponent_ones = low_mask(destination.exponent_bits);
	std::uint64_t result{};
	if (value_exponent >= minimum_exponent) {
		auto rounded = round_significand(source.significand,
			highest - static_cast<int>(destination.fraction_bits),
			rounding, source.negative);
		int biased_exponent = value_exponent + destination.bias;
		if (rounded.value ==
		    (std::uint64_t{1} << (destination.fraction_bits + 1U))) {
			rounded.value >>= 1U;
			++biased_exponent;
		}
		const auto limit =
			alternative_half ? exponent_ones + 1U : exponent_ones;
		if (biased_exponent >= static_cast<int>(limit)) {
			if (alternative_half) {
				result = sign | low_mask(destination_width - 1U);
				flags |= fpsr_invalid;
			} else {
				const bool to_infinity = rounding == 0U ||
					(rounding == 1U && !source.negative) ||
					(rounding == 2U && source.negative);
				result = to_infinity ?
					infinity_bits(destination_width, source.negative) :
					maximum_normal_bits(destination_width,
						source.negative);
				flags |= fpsr_overflow | fpsr_inexact;
			}
		} else {
			result = sign |
				(static_cast<std::uint64_t>(biased_exponent) <<
					destination.fraction_bits) |
				(rounded.value & low_mask(destination.fraction_bits));
			if (rounded.inexact)
				flags |= fpsr_inexact;
		}
	} else if ((fpcr & fpcr_ah) == 0U &&
	    destination_width != 16U && (fpcr & fpcr_fz) != 0U) {
		result = sign;
		flags |= fpsr_underflow;
	} else {
		const int unit_exponent =
			minimum_exponent - static_cast<int>(destination.fraction_bits);
		const auto rounded = round_significand(source.significand,
			unit_exponent - source.exponent, rounding, source.negative);
		const bool became_normal = rounded.value >=
			(std::uint64_t{1} << destination.fraction_bits);
		if ((fpcr & fpcr_ah) != 0U && !became_normal &&
		    destination_width != 16U && (fpcr & fpcr_fz) != 0U) {
			result = sign;
			flags |= fpsr_underflow | fpsr_inexact;
		} else {
			result = sign | (became_normal ?
				(std::uint64_t{1} << destination.fraction_bits) :
				rounded.value);
			if (rounded.inexact) {
				flags |= fpsr_inexact;
				if ((fpcr & fpcr_ah) == 0U || !became_normal)
					flags |= fpsr_underflow;
			}
		}
	}
	if (source_width != 16U &&
	    source.type == FloatingClass::denormal &&
	    (fpcr & fpcr_ah) != 0U)
		flags |= fpsr_input_denormal;
	return {result, flags};
}

[[nodiscard]] inline std::uint64_t read_lane(
	const std::array<std::byte, 16> &bytes, unsigned lane,
	unsigned width) noexcept
{
	std::uint64_t result{};
	for (unsigned byte = 0U; byte < width / 8U; ++byte)
		result |= static_cast<std::uint64_t>(std::to_integer<unsigned>(
			bytes[lane * width / 8U + byte])) << (byte * 8U);
	return result;
}

inline void write_lane(std::array<std::byte, 16> &bytes, unsigned lane,
	unsigned width, std::uint64_t value) noexcept
{
	for (unsigned byte = 0U; byte < width / 8U; ++byte)
		bytes[lane * width / 8U + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

inline void fill_source(arm64::CpuState &cpu, const Form &form,
	Registers registers, std::span<const std::uint64_t> values)
{
	const auto first = source_lane_base(form);
	for (unsigned lane = 0U; lane < lanes(form); ++lane)
		write_lane(cpu.vectors[registers.source].bytes, first + lane,
			form.source_width, values[lane % values.size()]);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0U; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0U;
		     byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] =
				static_cast<std::byte>(seed * 61U + reg * 31U +
					byte * 19U + 7U);
	for (unsigned reg = 0U; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(reg + seed) *
				0x0101'0101'0101'0101ULL);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpsr = (seed & 1U) != 0U ? 1U << 27 : 0U;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
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
	const auto source = initial.vectors[registers.source].bytes;
	auto expected = initial;
	auto result = (form.operation != Operation::widen && form.upper) ||
		(form.scalar && (initial.fpcr & fpcr_nep) != 0U) ?
		initial.vectors[registers.destination].bytes :
		std::array<std::byte, 16>{};
	std::uint32_t flags{};
	const unsigned input_base = source_lane_base(form);
	const unsigned output_base = destination_lane_base(form);
	for (unsigned lane = 0U; lane < lanes(form); ++lane) {
		const auto converted = convert(read_lane(source,
			input_base + lane, form.source_width), form.source_width,
			form.destination_width, initial.fpcr,
			form.operation == Operation::round_odd);
		write_lane(result, output_base + lane, form.destination_width,
			converted.bits);
		flags |= converted.flags;
	}
	expected.vectors[registers.destination].bytes = result;
	expected.fpsr |= flags;
	return expected;
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, const Form &form, Registers registers,
	const arm64::CpuState &initial)
{
	harness.check_one(name, instruction(form, registers), initial,
		expected_state(initial, form, registers),
		native_requirements(form, initial.fpcr));
}

[[nodiscard]] constexpr std::array<std::uint64_t, 20>
representative_inputs(unsigned width) noexcept
{
	if (width == 16U)
		return {0x0000U, 0x8000U, 0x0001U, 0x8001U,
			0x03ffU, 0x83ffU, 0x0400U, 0x8400U,
			0x3c00U, 0xbc00U, 0x3c01U, 0xbc01U,
			0x7bffU, 0xfbffU, 0x7c00U, 0xfc00U,
			0x7e55U, 0xfe55U, 0x7d55U, 0xfd55U};
	if (width == 32U)
		return {0x0000'0000U, 0x8000'0000U,
			0x0000'0001U, 0x8000'0001U,
			0x007f'ffffU, 0x807f'ffffU,
			0x0080'0000U, 0x8080'0000U,
			0x3f80'0000U, 0xbf80'0000U,
			0x3f80'0001U, 0xbf80'0001U,
			0x7f7f'ffffU, 0xff7f'ffffU,
			0x7f80'0000U, 0xff80'0000U,
			0x7fc1'2345U, 0xffc1'2345U,
			0x7f81'2345U, 0xff81'2345U};
	return {0x0000'0000'0000'0000ULL,
		0x8000'0000'0000'0000ULL,
		0x0000'0000'0000'0001ULL,
		0x8000'0000'0000'0001ULL,
		0x000f'ffff'ffff'ffffULL,
		0x800f'ffff'ffff'ffffULL,
		0x0010'0000'0000'0000ULL,
		0x8010'0000'0000'0000ULL,
		0x3ff0'0000'0000'0000ULL,
		0xbff0'0000'0000'0000ULL,
		0x3ff0'0000'0000'0001ULL,
		0xbff0'0000'0000'0001ULL,
		0x7fef'ffff'ffff'ffffULL,
		0xffef'ffff'ffff'ffffULL,
		0x7ff0'0000'0000'0000ULL,
		0xfff0'0000'0000'0000ULL,
		0x7ff8'1234'5678'9abcULL,
		0xfff8'1234'5678'9abcULL,
		0x7ff0'1234'5678'9abcULL,
		0xfff0'1234'5678'9abcULL};
}

} // namespace arm64_floating_precision_conversion_test
