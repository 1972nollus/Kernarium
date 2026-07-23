// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_test_support.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <span>

namespace arm64_simd_fp_indexed_test {

namespace base = arm64_simd_test;

enum class Operation : unsigned { fmla = 1, fmls = 5, fmul = 9, fmulx = 25 };

struct Shape final {
	unsigned size;
	unsigned lane_bytes;
	unsigned indexes;
	bool scalar;
	bool q;
};

inline constexpr std::array shapes{
	Shape{0U, 2U, 8U, true, true}, Shape{0U, 2U, 8U, false, false},
	Shape{0U, 2U, 8U, false, true}, Shape{2U, 4U, 4U, true, true},
	Shape{2U, 4U, 4U, false, false}, Shape{2U, 4U, 4U, false, true},
	Shape{3U, 8U, 2U, true, true}, Shape{3U, 8U, 2U, false, true}};

[[nodiscard]] constexpr bool accumulating(Operation operation)
{
	return operation == Operation::fmla || operation == Operation::fmls;
}

[[nodiscard]] constexpr std::uint32_t instruction(Operation operation,
	const Shape &shape, unsigned destination, unsigned left,
	unsigned element_register, unsigned index)
{
	const bool extended = operation == Operation::fmulx;
	const unsigned opcode = static_cast<unsigned>(operation) & 15U;
	std::uint32_t encoded = (shape.scalar ? 0x5f00'0000U : 0x0f00'0000U) |
		(!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
		(extended ? 0x2000'0000U : 0U) |
		((shape.size & 3U) << 22) | ((opcode & 15U) << 12) |
		((element_register & 15U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
	if (shape.size == 0U) {
		encoded |= (index & 1U) << 20;
		encoded |= (index & 2U) << 20;
		encoded |= (index & 4U) << 9;
	} else {
		encoded |= ((element_register >> 4) & 1U) << 20;
		if (shape.size == 2U) {
			encoded |= (index & 1U) << 21;
			encoded |= (index & 2U) << 10;
		} else {
			encoded |= (index & 1U) << 11;
		}
	}
	return encoded;
}

inline void write_bits(std::array<std::byte, 16> &bytes, unsigned lane_bytes,
	unsigned lane, std::uint64_t bits)
{
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		bytes[lane * lane_bytes + byte] =
			static_cast<std::byte>(bits >> (byte * 8U));
}

inline void fill_bits(std::array<std::byte, 16> &bytes, unsigned lane_bytes,
	std::uint64_t bits)
{
	for (unsigned lane = 0; lane < 16U / lane_bytes; ++lane)
		write_bits(bytes, lane_bytes, lane, bits);
}

[[nodiscard]] inline std::uint64_t floating_bits(unsigned lane_bytes,
	double value)
{
	if (lane_bytes == 2U)
		return std::bit_cast<std::uint16_t>(static_cast<_Float16>(value));
	if (lane_bytes == 4U)
		return std::bit_cast<std::uint32_t>(static_cast<float>(value));
	return std::bit_cast<std::uint64_t>(value);
}

inline void require_uniform_result(std::uint32_t encoded, const Shape &shape,
	Operation operation, unsigned destination, unsigned left,
	const uml::backend::arm64::CpuState &initial, std::uint64_t result_bits,
	std::uint32_t expected_fpsr, base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	std::array<std::byte, 16> result{};
	if (shape.scalar && (initial.fpcr & 4U) != 0)
		result = accumulating(operation) ?
			initial.vectors[destination].bytes : initial.vectors[left].bytes;
	const unsigned bytes = shape.scalar ? shape.lane_bytes : shape.q ? 16U : 8U;
	for (unsigned lane = 0; lane < bytes / shape.lane_bytes; ++lane)
		write_bits(result, shape.lane_bytes, lane, result_bits);
	expected.vectors[destination].bytes = result;
	expected.fpsr = expected_fpsr;
	const auto interpreted = harness.run_interpreter(encoded, initial);
	base::require_architecture_equal(expected, interpreted);
	harness.require_differential(encoded, initial);
}

} // namespace arm64_simd_fp_indexed_test
