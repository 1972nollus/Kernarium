// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_fcvtzi_test_support.hpp"

namespace arm64_simd_cvtfi_test {

namespace base = arm64_simd_test;
namespace fp = arm64_simd_fcvtzi_test;
namespace move = arm64_simd_fmov_test;

using Shape = fp::Shape;
inline constexpr auto shapes = fp::shapes;

[[nodiscard]] constexpr std::uint32_t gpr_instruction(bool unsigned_source,
	unsigned integer_width, unsigned destination_width, unsigned destination,
	unsigned source)
{
	return 0x1e22'0000U |
		(move::type_for_bytes(destination_width / 8U) << 22) |
		(unsigned_source ? 0x0001'0000U : 0U) |
		(integer_width == 64U ? 0x8000'0000U : 0U) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t fpr_instruction(bool unsigned_source,
	unsigned integer_width, unsigned destination_width, unsigned destination,
	unsigned source)
{
	return 0x1e3c'0000U |
		(move::type_for_bytes(destination_width / 8U) << 22) |
		(unsigned_source ? 0x0001'0000U : 0U) |
		(integer_width == 64U ? 0x8000'0000U : 0U) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t simd_instruction(bool unsigned_source,
	const Shape &shape, unsigned destination, unsigned source)
{
	const bool half = shape.width == 16U;
	std::uint32_t encoded = half ? 0x0e79'd800U : 0x0e21'd800U;
	if (shape.scalar)
		encoded |= 0x5000'0000U;
	if (unsigned_source)
		encoded |= 0x2000'0000U;
	if (!shape.scalar && shape.q)
		encoded |= 0x4000'0000U;
	if (!half && shape.width == 64U)
		encoded |= 0x0040'0000U;
	return encoded | ((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t gpr_fixed_instruction(
	bool unsigned_source, unsigned integer_width, unsigned destination_width,
	unsigned fractional_bits, unsigned destination, unsigned source)
{
	return 0x1e02'0000U |
		(move::type_for_bytes(destination_width / 8U) << 22) |
		(unsigned_source ? 0x0001'0000U : 0U) |
		(integer_width == 64U ? 0x8000'0000U : 0U) |
		((64U - fractional_bits) << 10) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t simd_fixed_instruction(
	bool unsigned_source, const Shape &shape, unsigned fractional_bits,
	unsigned destination, unsigned source)
{
	const unsigned immediate = 2U * shape.width - fractional_bits;
	return (shape.scalar ? 0x5f00'e400U : 0x0f00'e400U) |
		(unsigned_source ? 0x2000'0000U : 0U) |
		(!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
		(immediate << 16) | ((source & 31U) << 5) |
		(destination & 31U);
}

inline void set_lane(uml::backend::arm64::CpuState &cpu, unsigned reg,
	unsigned width, unsigned lane, std::uint64_t value)
{
	move::write_bits(cpu.vectors[reg].bytes, lane * width / 8U,
		width / 8U, value);
}

inline void require_vector(std::uint32_t encoded, unsigned destination_width,
	unsigned vector_bytes, bool scalar, unsigned destination,
	std::span<const std::uint64_t> values, std::uint32_t exceptions,
	const uml::backend::arm64::CpuState &initial, base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	auto result = scalar && (initial.fpcr & 4U) != 0U ?
		initial.vectors[destination].bytes : std::array<std::byte, 16>{};
	for (unsigned lane = 0; lane < values.size(); ++lane)
		move::write_bits(result, lane * destination_width / 8U,
			destination_width / 8U, values[lane]);
	expected.vectors[destination].bytes = result;
	expected.fpsr |= exceptions;
	base::require_architecture_equal(expected,
		harness.run_interpreter(encoded, initial));
	harness.require_differential(encoded, initial);
	(void)vector_bytes;
}

} // namespace arm64_simd_cvtfi_test
