// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_indexed_test_support.hpp"

namespace arm64_simd_dot_test {

namespace indexed = arm64_simd_indexed_test;
namespace base = arm64_simd_test;

enum class Mode { signed_signed, unsigned_unsigned, signed_unsigned,
	unsigned_signed };

[[nodiscard]] constexpr bool left_signed(Mode mode)
{
	return mode == Mode::signed_signed || mode == Mode::signed_unsigned;
}

[[nodiscard]] constexpr bool right_signed(Mode mode)
{
	return mode == Mode::signed_signed || mode == Mode::unsigned_signed;
}

[[nodiscard]] constexpr std::uint32_t vector_instruction(Mode mode, bool q,
	unsigned size, unsigned destination, unsigned left, unsigned right)
{
	const bool unsigned_dot = mode == Mode::unsigned_unsigned;
	const bool mixed = mode == Mode::unsigned_signed;
	return (mixed ? 0x0e00'9c00U : 0x0e00'9400U) |
		(q ? 0x4000'0000U : 0U) |
		(unsigned_dot ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((right & 31U) << 16) |
		((left & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t indexed_instruction(Mode mode, bool q,
	unsigned destination, unsigned left, unsigned element_register,
	unsigned index)
{
	const bool ordinary = mode == Mode::signed_signed ||
		mode == Mode::unsigned_unsigned;
	const unsigned size = mode == Mode::signed_unsigned ? 0U : 2U;
	auto encoded = indexed::instruction(false, q,
		mode == Mode::unsigned_unsigned, size, ordinary ? 14U : 15U,
		destination, left, element_register, index);
	if (mode == Mode::signed_unsigned) {
		encoded |= ((element_register >> 4) & 1U) << 20;
		encoded |= (index & 1U) << 21;
		encoded |= (index & 2U) << 10;
	}
	return encoded;
}

inline void require_oracle(std::uint32_t encoded, Mode mode, bool q,
	bool by_element, unsigned destination, unsigned left, unsigned right,
	unsigned index, const uml::backend::arm64::CpuState &initial,
	base::Harness &harness)
{
	const unsigned vector_bytes = q ? 16U : 8U;
	const auto old_destination = initial.vectors[destination].bytes;
	const auto old_left = initial.vectors[left].bytes;
	const auto old_right = initial.vectors[right].bytes;
	std::array<std::byte, 16> expected{};
	for (unsigned group = 0; group < vector_bytes / 4U; ++group) {
		std::int64_t sum = 0;
		for (unsigned byte = 0; byte < 4U; ++byte) {
			const auto left_raw = std::to_integer<std::uint8_t>(
				old_left[group * 4U + byte]);
			const unsigned right_offset =
				(by_element ? index : group) * 4U + byte;
			const auto right_raw = std::to_integer<std::uint8_t>(
				old_right[right_offset]);
			const std::int64_t left_value = left_signed(mode) ?
				static_cast<std::int8_t>(left_raw) : left_raw;
			const std::int64_t right_value = right_signed(mode) ?
				static_cast<std::int8_t>(right_raw) : right_raw;
			sum += left_value * right_value;
		}
		const auto accumulator = indexed::read_lane(
			old_destination, group * 4U, 4U);
		indexed::write_lane(expected, group * 4U, 4U,
			static_cast<std::uint32_t>(accumulator + sum));
	}
	auto expected_state = initial;
	expected_state.pc = base::pc + 4U;
	expected_state.vectors[destination].bytes = expected;
	const auto interpreted = harness.run_interpreter(encoded, initial);
	base::require_architecture_equal(expected_state, interpreted);
	harness.require_differential(encoded, initial);
}

} // namespace arm64_simd_dot_test
