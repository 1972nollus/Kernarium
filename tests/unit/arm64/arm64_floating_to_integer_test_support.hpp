// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_floating_to_integer_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

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
inline constexpr std::uint32_t fpcr_ahp = 1U << 26;

enum class Destination : std::uint8_t {
	gpr,
	simd_same_width,
	simd_cross_width,
};

enum class Rounding : std::uint8_t {
	tie_even,
	plus_infinity,
	minus_infinity,
	zero,
	tie_away,
};

struct Form final {
	Destination destination;
	Rounding rounding;
	bool unsigned_result;
	bool scalar;
	bool fixed;
	unsigned source_width;
	unsigned integer_width;
	unsigned vector_bytes;
	unsigned fractional_bits;
};

struct Registers final {
	unsigned destination;
	unsigned source;
};

struct Shape final {
	bool scalar;
	unsigned width;
	unsigned bytes;
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
	unsigned __int128 significand;
	int exponent;
	FloatingClass type;
	bool negative;
};

struct Conversion final {
	std::uint64_t value;
	std::uint32_t flags;
};

struct JavaScriptConversion final {
	std::uint32_t value;
	std::uint32_t flags;
	bool exact;
};

inline constexpr std::array register_edges{
	Registers{0U, 1U}, Registers{1U, 0U}, Registers{1U, 1U},
	Registers{30U, 31U}, Registers{31U, 30U}, Registers{31U, 31U}};

inline constexpr std::array shapes{
	Shape{true, 16U, 2U}, Shape{true, 32U, 4U},
	Shape{true, 64U, 8U}, Shape{false, 16U, 8U},
	Shape{false, 16U, 16U}, Shape{false, 32U, 8U},
	Shape{false, 32U, 16U}, Shape{false, 64U, 16U}};

[[nodiscard]] constexpr std::uint64_t low_mask(unsigned bits) noexcept
{
	return bits == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] constexpr std::uint64_t sign_mask(unsigned width) noexcept
{
	return std::uint64_t{1} << (width - 1U);
}

[[nodiscard]] constexpr Format format(unsigned width) noexcept
{
	return width == 16U ? Format{16U, 5U, 10U, 15} :
		width == 32U ? Format{32U, 8U, 23U, 127} :
			Format{64U, 11U, 52U, 1023};
}

[[nodiscard]] constexpr bool is_cross_width(const Form &form) noexcept
{
	return form.destination == Destination::simd_cross_width;
}

[[nodiscard]] constexpr unsigned lanes(const Form &form) noexcept
{
	return form.scalar ? 1U :
		form.vector_bytes / (form.source_width / 8U);
}

template<typename Function>
void for_each_nonfixed_form(Function &&function)
{
	for (unsigned rounding = 0U; rounding < 5U; ++rounding) {
		for (const bool unsigned_result : {false, true}) {
			for (const unsigned source_width : {16U, 32U, 64U})
				for (const unsigned integer_width : {32U, 64U})
					function(Form{Destination::gpr,
						static_cast<Rounding>(rounding), unsigned_result,
						true, false, source_width, integer_width,
						source_width / 8U, 0U});
			for (const auto widths : std::array{
				     std::array{16U, 32U}, std::array{16U, 64U},
				     std::array{32U, 64U}, std::array{64U, 32U}})
				function(Form{Destination::simd_cross_width,
					static_cast<Rounding>(rounding), unsigned_result,
					true, false, widths[0], widths[1],
					widths[1] / 8U, 0U});
			for (const auto shape : shapes)
				function(Form{Destination::simd_same_width,
					static_cast<Rounding>(rounding), unsigned_result,
					shape.scalar, false, shape.width, shape.width,
					shape.bytes, 0U});
		}
	}
}

template<typename Function>
void for_each_fixed_form(Function &&function, bool every_fraction = true)
{
	for (const bool unsigned_result : {false, true}) {
		for (const unsigned source_width : {16U, 32U, 64U})
			for (const unsigned integer_width : {32U, 64U}) {
				const unsigned first = every_fraction ? 1U :
					integer_width / 2U;
				const unsigned last = every_fraction ?
					integer_width : first;
				for (unsigned fractional_bits = first;
				     fractional_bits <= last; ++fractional_bits)
					function(Form{Destination::gpr, Rounding::zero,
						unsigned_result, true, true, source_width,
						integer_width, source_width / 8U,
						fractional_bits});
			}
		for (const auto shape : shapes) {
			const unsigned first = every_fraction ? 1U : shape.width / 2U;
			const unsigned last = every_fraction ? shape.width : first;
			for (unsigned fractional_bits = first;
			     fractional_bits <= last; ++fractional_bits)
				function(Form{Destination::simd_same_width,
					Rounding::zero, unsigned_result, shape.scalar, true,
					shape.width, shape.width, shape.bytes,
					fractional_bits});
		}
	}
}

template<typename Function>
void for_each_form(Function &&function)
{
	for_each_nonfixed_form(function);
	for_each_fixed_form(function);
}

[[nodiscard]] constexpr unsigned type_field(unsigned width) noexcept
{
	return width == 16U ? 3U : width == 32U ? 0U : 1U;
}

[[nodiscard]] constexpr std::uint32_t instruction(
	const Form &form, Registers registers = {0U, 1U}) noexcept
{
	const auto fields = ((registers.source & 31U) << 5) |
		(registers.destination & 31U);
	if (form.destination == Destination::gpr) {
		if (form.fixed)
			return 0x1e00'0000U |
				(form.integer_width == 64U ? 0x8000'0000U : 0U) |
				(type_field(form.source_width) << 22) | (3U << 19) |
				(static_cast<unsigned>(form.unsigned_result) << 16) |
				((64U - form.fractional_bits) << 10) | fields;
		unsigned rmode{};
		unsigned opcode = static_cast<unsigned>(form.unsigned_result);
		switch (form.rounding) {
		case Rounding::tie_even: rmode = 0U; break;
		case Rounding::plus_infinity: rmode = 1U; break;
		case Rounding::minus_infinity: rmode = 2U; break;
		case Rounding::zero: rmode = 3U; break;
		case Rounding::tie_away:
			rmode = 0U;
			opcode += 4U;
			break;
		}
		return 0x1e20'0000U |
			(form.integer_width == 64U ? 0x8000'0000U : 0U) |
			(type_field(form.source_width) << 22) |
			(rmode << 19) | (opcode << 16) | fields;
	}

	if (form.destination == Destination::simd_cross_width) {
		unsigned rmode{};
		unsigned opcode{};
		switch (form.rounding) {
		case Rounding::tie_even: rmode = 1U; opcode = 2U; break;
		case Rounding::plus_infinity: rmode = 2U; opcode = 2U; break;
		case Rounding::minus_infinity: rmode = 2U; opcode = 4U; break;
		case Rounding::zero: rmode = 2U; opcode = 6U; break;
		case Rounding::tie_away: rmode = 3U; opcode = 2U; break;
		}
		opcode += static_cast<unsigned>(form.unsigned_result);
		return 0x1e20'0000U |
			(form.integer_width == 64U ? 0x8000'0000U : 0U) |
			(type_field(form.source_width) << 22) |
			(rmode << 19) | (opcode << 16) | fields;
	}

	if (form.fixed) {
		const unsigned immediate =
			2U * form.source_width - form.fractional_bits;
		return (form.scalar ? 0x5f00'fc00U : 0x0f00'fc00U) |
			(form.unsigned_result ? 0x2000'0000U : 0U) |
			(!form.scalar && form.vector_bytes == 16U ?
				0x4000'0000U : 0U) |
			(immediate << 16) | fields;
	}

	constexpr std::array half_bases{
		0x0e79'a800U, 0x0ef9'a800U, 0x0e79'b800U,
		0x0ef9'b800U, 0x0e79'c800U};
	constexpr std::array ordinary_bases{
		0x0e21'a800U, 0x0ea1'a800U, 0x0e21'b800U,
		0x0ea1'b800U, 0x0e21'c800U};
	const auto index = static_cast<unsigned>(form.rounding);
	return (form.source_width == 16U ?
			half_bases[index] : ordinary_bases[index]) |
		(form.scalar ? 0x5000'0000U : 0U) |
		(form.unsigned_result ? 0x2000'0000U : 0U) |
		(form.source_width == 64U ? 0x0040'0000U : 0U) |
		(!form.scalar && form.vector_bytes == 16U ?
			0x4000'0000U : 0U) |
		fields;
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(const Form &form, std::uint32_t fpcr = 0U)
{
	using ::uml::cpu::ArmFeature;
	auto requirements = is_cross_width(form) ?
		engine_tests::NativeRequirements::features(ArmFeature::fprcvt) :
		engine_tests::NativeRequirements::features(
			form.destination == Destination::gpr ?
				ArmFeature::floating_point : ArmFeature::advanced_simd);
	if (!is_cross_width(form) && form.source_width == 16U)
		requirements = requirements | ArmFeature::fp16;
	if ((fpcr & (fpcr_fiz | fpcr_ah)) != 0U ||
	    (form.scalar && (fpcr & fpcr_nep) != 0U))
		requirements = requirements | ArmFeature::afp;
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

[[nodiscard]] constexpr Unpacked unpack(std::uint64_t input,
	unsigned width, std::uint32_t fpcr, std::uint32_t &flags) noexcept
{
	const auto source = format(width);
	input &= low_mask(width);
	const bool negative = (input & sign_mask(width)) != 0U;
	const auto fraction = input & low_mask(source.fraction_bits);
	const auto exponent = (input >> source.fraction_bits) &
		low_mask(source.exponent_bits);
	if (exponent == 0U) {
		if (fraction == 0U)
			return {0U, 0, FloatingClass::zero, negative};
		if (width == 16U && (fpcr & fpcr_fz16) != 0U)
			return {0U, 0, FloatingClass::zero, negative};
		if (width != 16U) {
			const bool flush =
				(fpcr & fpcr_fz) != 0U && (fpcr & fpcr_ah) == 0U;
			if (flush || (fpcr & fpcr_fiz) != 0U) {
				if (flush)
					flags |= fpsr_input_denormal;
				return {0U, 0, FloatingClass::zero, negative};
			}
		}
		return {fraction,
			1 - source.bias - static_cast<int>(source.fraction_bits),
			FloatingClass::denormal, negative};
	}
	if (exponent == low_mask(source.exponent_bits)) {
		if (fraction == 0U)
			return {0U, 0, FloatingClass::infinity, negative};
		const auto quiet = std::uint64_t{1} <<
			(source.fraction_bits - 1U);
		return {0U, 0, (fraction & quiet) != 0U ?
			FloatingClass::quiet_nan : FloatingClass::signaling_nan,
			negative};
	}
	return {(static_cast<unsigned __int128>(1) << source.fraction_bits) |
		fraction,
		static_cast<int>(exponent) - source.bias -
			static_cast<int>(source.fraction_bits),
		FloatingClass::finite, negative};
}

[[nodiscard]] constexpr Conversion convert(std::uint64_t input,
	unsigned source_width, unsigned integer_width, unsigned fractional_bits,
	bool unsigned_result, Rounding rounding, std::uint32_t fpcr) noexcept
{
	std::uint32_t flags{};
	const auto source = unpack(input, source_width,
		fpcr & ~fpcr_ahp, flags);
	const auto maximum = unsigned_result ?
		static_cast<unsigned __int128>(low_mask(integer_width)) :
		(static_cast<unsigned __int128>(1) << (integer_width - 1U)) - 1U;
	const auto minimum_magnitude = unsigned_result ? 0U :
		static_cast<unsigned __int128>(1) << (integer_width - 1U);
	const auto saturated = [&]() constexpr -> Conversion {
		flags |= fpsr_invalid;
		if (unsigned_result)
			return {source.negative ? 0U : low_mask(integer_width), flags};
		return {source.negative ?
			static_cast<std::uint64_t>(minimum_magnitude) :
			static_cast<std::uint64_t>(maximum), flags};
	};
	if (source.type == FloatingClass::quiet_nan ||
	    source.type == FloatingClass::signaling_nan)
		return {0U, flags | fpsr_invalid};
	if (source.type == FloatingClass::infinity)
		return saturated();
	if (source.type == FloatingClass::zero)
		return {0U, flags};

	const int exponent = source.exponent +
		static_cast<int>(fractional_bits);
	unsigned __int128 magnitude{};
	unsigned __int128 remainder{};
	unsigned shift{};
	bool arithmetic_overflow{};
	if (exponent >= 0) {
		shift = static_cast<unsigned>(exponent);
		if (shift >= 128U ||
		    std::bit_width(source.significand) + shift > 128U)
			arithmetic_overflow = true;
		else
			magnitude = source.significand << shift;
	} else {
		shift = static_cast<unsigned>(-exponent);
		if (shift < 128U) {
			magnitude = source.significand >> shift;
			remainder = source.significand -
				(magnitude << shift);
		} else {
			remainder = source.significand;
		}
		if (remainder != 0U) {
			bool increment{};
			if (rounding == Rounding::plus_infinity)
				increment = !source.negative;
			else if (rounding == Rounding::minus_infinity)
				increment = source.negative;
			else if ((rounding == Rounding::tie_even ||
			          rounding == Rounding::tie_away) &&
			         shift < 128U) {
				const auto halfway =
					static_cast<unsigned __int128>(1) << (shift - 1U);
				increment = remainder > halfway ||
					(remainder == halfway &&
					 (rounding == Rounding::tie_away ||
					  (magnitude & 1U) != 0U));
			}
			magnitude += static_cast<unsigned>(increment);
		}
	}
	if (arithmetic_overflow ||
	    (source.negative ? magnitude > minimum_magnitude :
			magnitude > maximum) ||
	    (unsigned_result && source.negative && magnitude != 0U))
		return saturated();
	if (remainder != 0U)
		flags |= fpsr_inexact;
	const auto narrowed = static_cast<std::uint64_t>(magnitude) &
		low_mask(integer_width);
	return {source.negative && magnitude != 0U ?
		(~narrowed + 1U) & low_mask(integer_width) : narrowed, flags};
}

[[nodiscard]] inline std::uint64_t read_lane(
	const std::array<std::byte, 16> &bytes, unsigned lane, unsigned width)
{
	const unsigned lane_bytes = width / 8U;
	std::uint64_t value{};
	for (unsigned byte = 0U; byte < lane_bytes; ++byte)
		value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(
			bytes[lane * lane_bytes + byte])) << (byte * 8U);
	return value;
}

inline void write_lane(std::array<std::byte, 16> &bytes,
	unsigned lane, unsigned width, std::uint64_t value)
{
	const unsigned lane_bytes = width / 8U;
	for (unsigned byte = 0U; byte < lane_bytes; ++byte)
		bytes[lane * lane_bytes + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

inline void fill_source(arm64::CpuState &cpu, const Form &form,
	Registers registers, std::span<const std::uint64_t> values)
{
	for (unsigned lane = 0U; lane < lanes(form); ++lane)
		write_lane(cpu.vectors[registers.source].bytes, lane,
			form.source_width, values[lane % values.size()]);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, const Form &form, Registers registers)
{
	const auto source = initial.vectors[registers.source].bytes;
	auto expected = initial;
	std::uint32_t flags{};
	if (form.destination == Destination::gpr) {
		const auto converted = convert(read_lane(source, 0U,
			form.source_width), form.source_width, form.integer_width,
			form.fractional_bits, form.unsigned_result, form.rounding,
			initial.fpcr);
		if (registers.destination != 31U)
			expected.registers[registers.destination] =
				form.integer_width == 64U ? converted.value :
					static_cast<std::uint32_t>(converted.value);
		flags = converted.flags;
	} else {
		auto result = form.scalar && (initial.fpcr & fpcr_nep) != 0U ?
			initial.vectors[registers.destination].bytes :
			std::array<std::byte, 16>{};
		for (unsigned lane = 0U; lane < lanes(form); ++lane) {
			const auto converted = convert(read_lane(source, lane,
				form.source_width), form.source_width, form.integer_width,
				form.fractional_bits, form.unsigned_result, form.rounding,
				initial.fpcr);
			write_lane(result, lane, form.integer_width, converted.value);
			flags |= converted.flags;
		}
		expected.vectors[registers.destination].bytes = result;
	}
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

[[nodiscard]] constexpr std::uint64_t power_of_two(
	unsigned width, int exponent, bool negative = false) noexcept
{
	const auto source = format(width);
	const auto sign = negative ? sign_mask(width) : 0U;
	const int biased = exponent + source.bias;
	if (biased <= 0)
		return sign;
	if (biased >= static_cast<int>(low_mask(source.exponent_bits)))
		return sign |
			(low_mask(source.exponent_bits) << source.fraction_bits);
	return sign | (static_cast<std::uint64_t>(biased) <<
		source.fraction_bits);
}

[[nodiscard]] constexpr std::array<std::uint64_t, 20>
representative_inputs(unsigned width) noexcept
{
	if (width == 16U)
		return {0x0000U, 0x8000U, 0x0001U, 0x8001U, 0x03ffU,
			0x83ffU, 0x0400U, 0x8400U, 0x3400U, 0xb400U,
			0x3800U, 0xb800U, 0x3801U, 0xb801U, 0x3e00U,
			0xbe00U, 0x7c00U, 0xfc00U, 0x7e55U, 0x7d55U};
	if (width == 32U)
		return {0x0000'0000U, 0x8000'0000U, 0x0000'0001U,
			0x8000'0001U, 0x007f'ffffU, 0x807f'ffffU,
			0x0080'0000U, 0x8080'0000U, 0x3e80'0000U,
			0xbe80'0000U, 0x3f00'0000U, 0xbf00'0000U,
			0x3f00'0001U, 0xbf00'0001U, 0x3fc0'0000U,
			0xbfc0'0000U, 0x7f80'0000U, 0xff80'0000U,
			0x7fc1'2345U, 0x7f81'2345U};
	return {0x0000'0000'0000'0000ULL, 0x8000'0000'0000'0000ULL,
		0x0000'0000'0000'0001ULL, 0x8000'0000'0000'0001ULL,
		0x000f'ffff'ffff'ffffULL, 0x800f'ffff'ffff'ffffULL,
		0x0010'0000'0000'0000ULL, 0x8010'0000'0000'0000ULL,
		0x3fd0'0000'0000'0000ULL, 0xbfd0'0000'0000'0000ULL,
		0x3fe0'0000'0000'0000ULL, 0xbfe0'0000'0000'0000ULL,
		0x3fe0'0000'0000'0001ULL, 0xbfe0'0000'0000'0001ULL,
		0x3ff8'0000'0000'0000ULL, 0xbff8'0000'0000'0000ULL,
		0x7ff0'0000'0000'0000ULL, 0xfff0'0000'0000'0000ULL,
		0x7ff8'1234'5678'9abcULL, 0x7ff0'1234'5678'9abcULL};
}

[[nodiscard]] constexpr JavaScriptConversion convert_javascript(
	std::uint64_t input, std::uint32_t fpcr) noexcept
{
	std::uint32_t flags{};
	const auto source = unpack(input, 64U, fpcr, flags);
	if (source.type == FloatingClass::quiet_nan ||
	    source.type == FloatingClass::signaling_nan ||
	    source.type == FloatingClass::infinity)
		return {0U, flags | fpsr_invalid, false};
	if (source.type == FloatingClass::zero)
		return {0U, flags,
			!source.negative && (input & low_mask(52U)) == 0U};

	unsigned __int128 magnitude{};
	unsigned __int128 remainder{};
	const int exponent = source.exponent;
	if (exponent >= 0) {
		const unsigned shift = static_cast<unsigned>(exponent);
		magnitude = shift < 128U ? source.significand << shift : 0U;
	} else {
		const unsigned shift = static_cast<unsigned>(-exponent);
		if (shift < 128U) {
			magnitude = source.significand >> shift;
			remainder = source.significand - (magnitude << shift);
		} else {
			remainder = source.significand;
		}
	}
	const auto limit = static_cast<unsigned __int128>(
		source.negative ? 0x8000'0000ULL : 0x7fff'ffffULL);
	const bool outside = exponent >= 128 ||
		magnitude > limit;
	const unsigned modulo_shift = exponent >= 0 ?
		static_cast<unsigned>(exponent) : 0U;
	const std::uint32_t modulo = exponent >= 0 ?
		(modulo_shift >= 32U ? 0U :
			static_cast<std::uint32_t>(source.significand << modulo_shift)) :
		static_cast<std::uint32_t>(magnitude);
	const auto value = source.negative ? ~modulo + 1U : modulo;
	if (outside)
		flags |= fpsr_invalid;
	else if (remainder != 0U)
		flags |= fpsr_inexact;
	return {value, flags, !outside && remainder == 0U};
}

[[nodiscard]] inline arm64::CpuState expected_javascript(
	const arm64::CpuState &initial, Registers registers)
{
	auto expected = initial;
	const auto source = initial.vectors[registers.source].bytes;
	const auto converted = convert_javascript(
		read_lane(source, 0U, 64U), initial.fpcr);
	if (registers.destination != 31U)
		expected.registers[registers.destination] = converted.value;
	expected.fpsr |= converted.flags;
	expected.nzcv = converted.exact ? 0x4000'0000U : 0U;
	return expected;
}

inline void check_javascript(engine_tests::EngineHarness &harness,
	std::string_view name, Registers registers,
	const arm64::CpuState &initial)
{
	auto native = engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::jscvt);
	if ((initial.fpcr & (fpcr_fiz | fpcr_ah)) != 0U)
		native = native | ::uml::cpu::ArmFeature::afp;
	harness.check_one(name,
		0x1e7e'0000U | ((registers.source & 31U) << 5) |
			(registers.destination & 31U),
		initial, expected_javascript(initial, registers), native);
}

static_assert(instruction(Form{Destination::gpr, Rounding::tie_away,
	false, true, false, 64U, 32U, 8U, 0U}, {0U, 0U}) ==
	0x1e64'0000U);
static_assert(instruction(Form{Destination::simd_cross_width,
	Rounding::zero, true, true, false, 64U, 32U, 4U, 0U},
	{0U, 0U}) == 0x1e77'0000U);
static_assert(instruction(Form{Destination::simd_same_width,
	Rounding::tie_even, false, false, false, 32U, 32U, 16U, 0U},
	{0U, 0U}) == 0x4e21'a800U);
static_assert(instruction(Form{Destination::gpr, Rounding::zero,
	true, true, true, 64U, 64U, 8U, 64U}, {0U, 0U}) ==
	0x9e59'0000U);
static_assert(convert(0x3fc0'0000U, 32U, 32U, 0U, false,
	Rounding::tie_even, 0U).value == 2U);
static_assert(convert(0x3fc0'0000U, 32U, 32U, 0U, false,
	Rounding::tie_away, 0U).value == 2U);
static_assert(convert(0xbfc0'0000U, 32U, 32U, 0U, true,
	Rounding::zero, 0U).flags == fpsr_invalid);
static_assert(convert_javascript(0x3ff0'0000'0000'0000ULL, 0U).exact);
static_assert(convert_javascript(0x8000'0000'0000'0000ULL, 0U).exact ==
	false);

} // namespace arm64_floating_to_integer_test
