// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_fp_indexed_test_support.hpp"
#include "cpu/arm64/detail/instruction_helpers.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace arm64_simd_fabs_fneg_test {

namespace base = arm64_simd_test;
namespace fp = arm64_simd_fp_indexed_test;

enum class Operation { absolute, negate };

struct Shape final {
	unsigned lane_bytes;
	bool scalar;
	bool q;
};

inline constexpr std::array shapes{
	Shape{2U, true, false}, Shape{4U, true, false}, Shape{8U, true, false},
	Shape{2U, false, false}, Shape{2U, false, true},
	Shape{4U, false, false}, Shape{4U, false, true},
	Shape{8U, false, true}};

[[nodiscard]] constexpr std::uint32_t instruction(Operation operation,
	const Shape &shape, unsigned destination, unsigned source)
{
	const bool negate = operation == Operation::negate;
	std::uint32_t encoded;
	if (shape.scalar) {
		const unsigned type = shape.lane_bytes == 2U ? 3U :
			shape.lane_bytes == 4U ? 0U : 1U;
		encoded = (negate ? 0x1e21'4000U : 0x1e20'c000U) |
			(type << 22);
	} else if (shape.lane_bytes == 2U) {
		encoded = negate ? 0x2ef8'f800U : 0x0ef8'f800U;
		if (shape.q)
			encoded |= 0x4000'0000U;
	} else {
		encoded = negate ? 0x2e20'f800U : 0x0e20'f800U;
		if (shape.q)
			encoded |= 0x4000'0000U;
		if (shape.lane_bytes == 8U)
			encoded |= 0x0040'0000U;
	}
	return encoded | ((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint64_t expected_bits(std::uint64_t value,
	unsigned lane_bytes, Operation operation, bool alternative_handling)
{
	const unsigned bits = lane_bytes * 8U;
	const unsigned fraction_bits = bits == 16U ? 10U : bits == 32U ? 23U : 52U;
	const unsigned exponent_bits = bits == 16U ? 5U : bits == 32U ? 8U : 11U;
	const auto sign = std::uint64_t{1} << (bits - 1U);
	const auto fraction = (std::uint64_t{1} << fraction_bits) - 1U;
	const auto exponent = ((std::uint64_t{1} << exponent_bits) - 1U) <<
		fraction_bits;
	if (alternative_handling && (value & exponent) == exponent &&
	    (value & fraction) != 0U)
		return value;
	return operation == Operation::negate ? value ^ sign : value & ~sign;
}

[[nodiscard]] inline std::uint64_t read_bits(
	const std::array<std::byte, 16> &bytes, unsigned lane_bytes, unsigned lane)
{
	return uml::backend::arm64::detail::decode_little_endian(
		std::span{bytes}.subspan(lane * lane_bytes, lane_bytes));
}

inline void require_result(std::uint32_t encoded, const Shape &shape,
	Operation operation, unsigned destination, unsigned source,
	const uml::backend::arm64::CpuState &initial, base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	const auto source_bytes = initial.vectors[source].bytes;
	std::array<std::byte, 16> result{};
	if (shape.scalar && (initial.fpcr & 4U) != 0U)
		result = initial.vectors[destination].bytes;
	const unsigned vector_bytes = shape.scalar ? shape.lane_bytes :
		shape.q ? 16U : 8U;
	for (unsigned lane = 0; lane < vector_bytes / shape.lane_bytes; ++lane) {
		const auto value = expected_bits(read_bits(source_bytes,
			shape.lane_bytes, lane), shape.lane_bytes, operation,
			(initial.fpcr & 2U) != 0U);
		fp::write_bits(result, shape.lane_bytes, lane, value);
	}
	expected.vectors[destination].bytes = result;
	const auto interpreted = harness.run_interpreter(encoded, initial);
	base::require_architecture_equal(expected, interpreted);
	harness.require_differential(encoded, initial);
}

} // namespace arm64_simd_fabs_fneg_test
