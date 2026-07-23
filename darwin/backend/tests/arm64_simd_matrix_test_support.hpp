// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_indexed_test_support.hpp"

#include <array>

namespace arm64_simd_matrix_test {

namespace base = arm64_simd_test;
namespace indexed = arm64_simd_indexed_test;

enum class Mode { signed_signed, unsigned_unsigned, unsigned_signed };

[[nodiscard]] constexpr std::uint32_t instruction(Mode mode,
	unsigned destination, unsigned left, unsigned right)
{
	const std::uint32_t operation = mode == Mode::unsigned_signed ?
		0x4e80'ac00U : 0x4e80'a400U;
	return operation |
		(mode == Mode::unsigned_unsigned ? 0x2000'0000U : 0U) |
		((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

inline void require_oracle(std::uint32_t encoded, Mode mode,
	unsigned destination, unsigned left_register, unsigned right_register,
	const uml::backend::arm64::CpuState &initial, base::Harness &harness)
{
	const bool left_unsigned = mode != Mode::signed_signed;
	const bool right_unsigned = mode == Mode::unsigned_unsigned;
	const auto left = initial.vectors[left_register].bytes;
	const auto right = initial.vectors[right_register].bytes;
	const auto accumulator = initial.vectors[destination].bytes;
	std::array<std::byte, 16> result{};
	for (unsigned row = 0; row < 2U; ++row)
		for (unsigned column = 0; column < 2U; ++column) {
			std::int64_t sum = 0;
			for (unsigned k = 0; k < 8U; ++k) {
				const auto left_raw = std::to_integer<std::uint8_t>(
					left[row * 8U + k]);
				const auto right_raw = std::to_integer<std::uint8_t>(
					right[column * 8U + k]);
				const std::int64_t left_value = left_unsigned ? left_raw :
					static_cast<std::int8_t>(left_raw);
				const std::int64_t right_value = right_unsigned ? right_raw :
					static_cast<std::int8_t>(right_raw);
				sum += left_value * right_value;
			}
			const unsigned lane = row * 2U + column;
			const auto old = indexed::read_lane(accumulator, lane * 4U, 4U);
			indexed::write_lane(result, lane * 4U, 4U,
				static_cast<std::uint32_t>(old + sum));
		}
	auto expected = initial;
	expected.pc = base::pc + 4U;
	expected.vectors[destination].bytes = result;
	const auto interpreted = harness.run_interpreter(encoded, initial);
	base::require_architecture_equal(expected, interpreted);
	harness.require_differential(encoded, initial);
}

} // namespace arm64_simd_matrix_test
