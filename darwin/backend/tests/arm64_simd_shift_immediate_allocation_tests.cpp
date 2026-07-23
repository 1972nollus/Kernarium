// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <bit>

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t shift_instruction(bool scalar, bool q,
	bool alternate, unsigned opcode, unsigned encoded)
{
	return (scalar ? 0x5f00'0400U : 0x0f00'0400U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(alternate ? 0x2000'0000U : 0U) | ((encoded & 127U) << 16) |
		((opcode & 31U) << 11) | (3U << 5) | 2U;
}

[[nodiscard]] constexpr bool valid_shape(
	bool scalar, bool q, unsigned encoded)
{
	if (encoded < 8U)
		return false;
	const unsigned lane_bits = std::bit_floor(encoded);
	return lane_bits <= 64U && (!scalar || lane_bits == 64U) &&
		(scalar || q || lane_bits != 64U);
}

void right_shift_allocations()
{
	test::Harness harness;
	for (unsigned encoded = 8; encoded < 128; ++encoded)
		for (const bool scalar : {false, true})
			for (const bool q : {false, true}) {
				if (scalar && q)
					continue;
				const bool shape_valid = valid_shape(scalar, q, encoded);
				for (const unsigned opcode : {0U, 2U, 4U, 6U, 8U})
					for (const bool alternate : {false, true}) {
						const auto instruction = shift_instruction(scalar, q,
							alternate, opcode, encoded);
						const bool valid = shape_valid &&
							(opcode != 8U || alternate);
						if (valid)
							harness.require_differential(instruction,
								test::state(encoded + opcode));
						else
							harness.require_illegal(instruction,
								test::state(encoded + opcode));
					}
			}
}

void left_shift_allocations()
{
	test::Harness harness;
	for (unsigned encoded = 8; encoded < 128; ++encoded)
		for (const bool scalar : {false, true})
			for (const bool q : {false, true}) {
				if (scalar && q)
					continue;
				for (const bool insert : {false, true}) {
					const auto instruction = shift_instruction(
						scalar, q, insert, 10, encoded);
					if (valid_shape(scalar, q, encoded))
						harness.require_differential(instruction,
							test::state(encoded + insert));
					else
						harness.require_illegal(instruction,
							test::state(encoded + insert));
				}
			}
}

void narrow_allocations()
{
	test::Harness harness;
	for (unsigned encoded = 8; encoded < 128; ++encoded)
		for (const bool high : {false, true})
			for (const bool rounding : {false, true}) {
				const auto instruction = shift_instruction(false, high, false,
					rounding ? 17U : 16U, encoded);
				const bool valid = encoded >= 8U && encoded < 64U;
				if (valid)
					harness.require_differential(instruction,
						test::state(encoded + high + rounding));
				else
					harness.require_illegal(instruction,
						test::state(encoded + high + rounding));
			}
}

} // namespace

int main()
{
	right_shift_allocations();
	left_shift_allocations();
	narrow_allocations();
	return 0;
}
