// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <bit>

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t instruction(bool scalar, bool q,
	bool alternate, unsigned opcode, unsigned encoded)
{
	return (scalar ? 0x5f00'0400U : 0x0f00'0400U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(alternate ? 0x2000'0000U : 0U) | ((encoded & 127U) << 16) |
		((opcode & 31U) << 11) | (3U << 5) | 2U;
}

[[nodiscard]] constexpr bool valid_left_shape(
	bool scalar, bool q, unsigned encoded)
{
	if (encoded < 8U)
		return false;
	const unsigned lane_bits = std::bit_floor(encoded);
	return lane_bits <= 64U && (scalar || q || lane_bits != 64U);
}

void left_shift_allocations()
{
	test::Harness harness;
	for (unsigned encoded = 8; encoded < 128; ++encoded)
		for (const bool scalar : {false, true})
			for (const bool q : {false, true}) {
				if (scalar && q)
					continue;
				for (const bool unsigned_source : {false, true}) {
					const auto encoded_instruction = instruction(scalar, q,
						unsigned_source, 14, encoded);
					if (valid_left_shape(scalar, q, encoded))
						harness.require_differential(encoded_instruction,
							test::state(encoded + unsigned_source));
					else
						harness.require_illegal(encoded_instruction,
							test::state(encoded + unsigned_source));
				}
				for (const bool alternate : {false, true}) {
					const auto encoded_instruction = instruction(scalar, q,
						alternate, 12, encoded);
					if (alternate && valid_left_shape(scalar, q, encoded))
						harness.require_differential(encoded_instruction,
							test::state(encoded + 3U));
					else
						harness.require_illegal(encoded_instruction,
							test::state(encoded + 3U));
				}
			}
}

void narrow_allocations()
{
	test::Harness harness;
	for (unsigned encoded = 8; encoded < 128; ++encoded)
		for (const bool scalar : {false, true})
			for (const bool q : {false, true}) {
				if (scalar && q)
					continue;
				for (unsigned opcode = 16; opcode <= 19; ++opcode)
					for (const bool alternate : {false, true}) {
						const auto encoded_instruction = instruction(scalar, q,
							alternate, opcode, encoded);
						const bool saturating_form = opcode >= 18U || alternate;
						if (!saturating_form && !scalar)
							continue; // SHRN/RSHRN, tested separately.
						const bool valid = saturating_form && encoded < 64U;
						if (valid)
							harness.require_differential(encoded_instruction,
								test::state(encoded + opcode + alternate));
						else
							harness.require_illegal(encoded_instruction,
								test::state(encoded + opcode + alternate));
					}
			}
}

} // namespace

int main()
{
	left_shift_allocations();
	narrow_allocations();
	return 0;
}
