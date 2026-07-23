// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_floating_compare_test_support.hpp"
#include "arm64_simd_fcvt_precision_test_support.hpp"
#include "cpu/arm64/detail/floating_integer_conversion.hpp"

#include <array>
#include <span>
#include <utility>

namespace arm64_simd_fcvtzi_test {

namespace base = arm64_simd_test;
namespace compare = arm64_simd_floating_compare_test;
namespace move = arm64_simd_fmov_test;
namespace precision = arm64_simd_fcvt_precision_test;

using Shape = compare::Shape;
using Rounding =
	uml::backend::arm64::detail::FloatingIntegerRounding;
inline constexpr auto shapes = compare::shapes;
inline constexpr std::array roundings{
	Rounding::tie_even, Rounding::plus_infinity,
	Rounding::minus_infinity, Rounding::zero, Rounding::tie_away,
};

[[nodiscard]] constexpr std::uint32_t gpr_instruction(Rounding rounding,
	bool unsigned_result,
	unsigned source_width, unsigned integer_width, unsigned destination,
	unsigned source)
{
	const unsigned encoded_rounding = rounding == Rounding::tie_away ? 0U :
		static_cast<unsigned>(rounding);
	const unsigned opcode = rounding == Rounding::tie_away ?
		(unsigned_result ? 5U : 4U) : static_cast<unsigned>(unsigned_result);
	return 0x1e20'0000U |
		(move::type_for_bytes(source_width / 8U) << 22) |
		(encoded_rounding << 19) | (opcode << 16) |
		(integer_width == 64U ? 0x8000'0000U : 0U) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t gpr_instruction(bool unsigned_result,
	unsigned source_width, unsigned integer_width, unsigned destination,
	unsigned source)
{
	return gpr_instruction(Rounding::zero, unsigned_result, source_width,
		integer_width, destination, source);
}

[[nodiscard]] constexpr std::uint32_t simd_instruction(Rounding rounding,
	bool unsigned_result,
	const Shape &shape, unsigned destination, unsigned source)
{
	const bool half = shape.width == 16U;
	constexpr std::array half_bases{
		0x0e79'a800U, 0x0ef9'a800U, 0x0e79'b800U,
		0x0ef9'b800U, 0x0e79'c800U,
	};
	constexpr std::array ordinary_bases{
		0x0e21'a800U, 0x0ea1'a800U, 0x0e21'b800U,
		0x0ea1'b800U, 0x0e21'c800U,
	};
	std::uint32_t encoded =
		(half ? half_bases : ordinary_bases)[static_cast<unsigned>(rounding)];
	if (shape.scalar)
		encoded |= 0x5000'0000U;
	if (unsigned_result)
		encoded |= 0x2000'0000U;
	if (!shape.scalar && shape.q)
		encoded |= 0x4000'0000U;
	if (!half && shape.width == 64U)
		encoded |= 0x0040'0000U;
	return encoded | ((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t simd_instruction(bool unsigned_result,
	const Shape &shape, unsigned destination, unsigned source)
{
	return simd_instruction(Rounding::zero, unsigned_result, shape,
		destination, source);
}

[[nodiscard]] constexpr std::uint32_t fpr_instruction(Rounding rounding,
	bool unsigned_result, unsigned source_width, unsigned integer_width,
	unsigned destination, unsigned source)
{
	unsigned rmode{};
	unsigned opcode{};
	if (rounding == Rounding::tie_even) {
		rmode = 1U;
		opcode = unsigned_result ? 3U : 2U;
	} else if (rounding == Rounding::plus_infinity) {
		rmode = 2U;
		opcode = unsigned_result ? 3U : 2U;
	} else if (rounding == Rounding::minus_infinity) {
		rmode = 2U;
		opcode = unsigned_result ? 5U : 4U;
	} else if (rounding == Rounding::zero) {
		rmode = 2U;
		opcode = unsigned_result ? 7U : 6U;
	} else {
		rmode = 3U;
		opcode = unsigned_result ? 3U : 2U;
	}
	return 0x1e20'0000U |
		(move::type_for_bytes(source_width / 8U) << 22) |
		(integer_width == 64U ? 0x8000'0000U : 0U) |
		(rmode << 19) | (opcode << 16) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t gpr_fixed_instruction(
	bool unsigned_result, unsigned source_width, unsigned integer_width,
	unsigned fractional_bits, unsigned destination, unsigned source)
{
	return 0x1e18'0000U |
		(move::type_for_bytes(source_width / 8U) << 22) |
		(unsigned_result ? 0x0001'0000U : 0U) |
		(integer_width == 64U ? 0x8000'0000U : 0U) |
		((64U - fractional_bits) << 10) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t simd_fixed_instruction(
	bool unsigned_result, const Shape &shape, unsigned fractional_bits,
	unsigned destination, unsigned source)
{
	const unsigned immediate = 2U * shape.width - fractional_bits;
	return (shape.scalar ? 0x5f00'fc00U : 0x0f00'fc00U) |
		(unsigned_result ? 0x2000'0000U : 0U) |
		(!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
		(immediate << 16) | ((source & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint64_t floating_bits(unsigned width,
	bool negative, bool tie)
{
	const auto value = width == 16U ? (tie ? 0x4100U : 0x3e00U) :
		width == 32U ? (tie ? 0x4020'0000U : 0x3fc0'0000U) :
			(tie ? 0x4004'0000'0000'0000ULL :
				0x3ff8'0000'0000'0000ULL);
	return value | (negative ? std::uint64_t{1} << (width - 1U) : 0U);
}

[[nodiscard]] constexpr std::uint64_t expected_rounded(
	Rounding rounding, bool negative, bool tie, unsigned integer_width)
{
	const std::uint64_t magnitude = tie ?
		(rounding == Rounding::tie_away ||
		 (rounding == Rounding::plus_infinity && !negative) ||
		 (rounding == Rounding::minus_infinity && negative) ? 3U : 2U) :
		(rounding == Rounding::tie_even ||
		 rounding == Rounding::tie_away ||
		 (rounding == Rounding::plus_infinity && !negative) ||
		 (rounding == Rounding::minus_infinity && negative) ? 2U : 1U);
	return negative ? (~magnitude + 1U) &
		(integer_width == 64U ? ~std::uint64_t{0} :
			std::uint64_t{0xffff'ffffU}) : magnitude;
}

inline void set_gpr_source(uml::backend::arm64::CpuState &cpu,
	unsigned source, unsigned width, std::uint64_t bits)
{
	move::write_bits(cpu.vectors[source].bytes, 0U, width / 8U, bits);
}

inline void require_gpr(std::uint32_t encoded, unsigned integer_width,
	unsigned destination, std::uint64_t value, std::uint32_t exceptions,
	const uml::backend::arm64::CpuState &initial, base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	if (destination != 31U)
		expected.registers[destination] = integer_width == 64U ? value :
			static_cast<std::uint32_t>(value);
	expected.fpsr |= exceptions;
	base::require_architecture_equal(expected,
		harness.run_interpreter(encoded, initial));
	harness.require_differential(encoded, initial);
}

inline void require_simd(std::uint32_t encoded, const Shape &shape,
	unsigned destination, std::span<const std::uint64_t> values,
	std::uint32_t exceptions, const uml::backend::arm64::CpuState &initial,
	base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	auto result = shape.scalar && (initial.fpcr & 4U) != 0U ?
		initial.vectors[destination].bytes : std::array<std::byte, 16>{};
	for (unsigned lane = 0; lane < values.size(); ++lane)
		move::write_bits(result, lane * shape.width / 8U,
			shape.width / 8U, values[lane]);
	expected.vectors[destination].bytes = result;
	expected.fpsr |= exceptions;
	base::require_architecture_equal(expected,
		harness.run_interpreter(encoded, initial));
	harness.require_differential(encoded, initial);
}

inline void require_fpr(std::uint32_t encoded, unsigned integer_width,
	unsigned destination, std::uint64_t value, std::uint32_t exceptions,
	const uml::backend::arm64::CpuState &initial, base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	auto result = (initial.fpcr & 4U) != 0U ?
		initial.vectors[destination].bytes : std::array<std::byte, 16>{};
	move::write_bits(result, 0U, integer_width / 8U, value);
	expected.vectors[destination].bytes = result;
	expected.fpsr |= exceptions;
	base::require_architecture_equal(expected,
		harness.run_interpreter(encoded, initial));
	harness.require_differential(encoded, initial);
}

} // namespace arm64_simd_fcvtzi_test
