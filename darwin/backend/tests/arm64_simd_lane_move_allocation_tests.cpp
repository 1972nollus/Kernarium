// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <bit>

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t lane_instruction(std::uint32_t base,
	bool q, unsigned immediate, unsigned destination = 2, unsigned source = 3)
{
	return base | (q ? 0x4000'0000U : 0U) |
		((immediate & 31U) << 16) | ((source & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr bool lane_valid(unsigned immediate)
{
	return (immediate & 15U) != 0;
}

void duplicate_allocations()
{
	test::Harness harness;
	for (unsigned immediate = 0; immediate < 32; ++immediate) {
		const bool decoded = lane_valid(immediate);
		const unsigned shift = decoded ? std::countr_zero(immediate) : 4U;
		const auto initial = test::state(immediate);

		const auto scalar = lane_instruction(
			0x5e00'0400U, false, immediate);
		if (decoded)
			harness.require_differential(scalar, initial);
		else
			harness.require_illegal(scalar, initial);

		for (const bool q : {false, true}) {
			const bool valid = decoded && (q || shift != 3U);
			for (const auto base : {0x0e00'0400U, 0x0e00'0c00U}) {
				const auto instruction = lane_instruction(
					base, q, immediate);
				if (valid)
					harness.require_differential(instruction, initial);
				else
					harness.require_illegal(instruction, initial);
			}
		}
	}
}

void insert_allocations_and_ignored_bits()
{
	test::Harness harness;
	for (unsigned immediate = 0; immediate < 32; ++immediate) {
		const auto initial = test::state(immediate + 41U);
		const auto general = lane_instruction(
			0x4e00'1c00U, false, immediate);
		if (lane_valid(immediate))
			harness.require_differential(general, initial);
		else
			harness.require_illegal(general, initial);

		for (unsigned imm4 = 0; imm4 < 16; ++imm4) {
			const auto element = lane_instruction(
				0x6e00'0400U | (imm4 << 11), false, immediate);
			if (lane_valid(immediate))
				harness.require_differential(element, initial);
			else
				harness.require_illegal(element, initial);
		}
	}
}

void gpr_move_allocations()
{
	test::Harness harness;
	for (unsigned immediate = 0; immediate < 32; ++immediate) {
		const bool decoded = lane_valid(immediate);
		const unsigned shift = decoded ? std::countr_zero(immediate) : 4U;
		const auto initial = test::state(immediate + 83U);
		for (const bool q : {false, true}) {
			const bool smov_valid = decoded &&
				shift < (q ? 3U : 2U);
			const auto smov = lane_instruction(
				0x0e00'2c00U, q, immediate);
			if (smov_valid)
				harness.require_differential(smov, initial);
			else
				harness.require_illegal(smov, initial);

			const bool umov_valid = decoded &&
				(q ? shift == 3U : shift < 3U);
			const auto umov = lane_instruction(
				0x0e00'3c00U, q, immediate);
			if (umov_valid)
				harness.require_differential(umov, initial);
			else
				harness.require_illegal(umov, initial);
		}
	}
}

} // namespace

int main()
{
	duplicate_allocations();
	insert_allocations_and_ignored_bits();
	gpr_move_allocations();
	return 0;
}
