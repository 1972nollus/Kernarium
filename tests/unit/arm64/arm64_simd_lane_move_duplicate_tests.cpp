// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_lane_move_test_support.hpp"

#include <array>

namespace test = arm64_simd_lane_move_test;

namespace {

constexpr std::array register_cases{
	test::Registers{6, 7},
	test::Registers{7, 7},
	test::Registers{31, 0},
	test::Registers{0, 31},
};

void every_element_size_lane_shape_and_alias(
	test::engine_tests::EngineHarness &harness)
{
	for (unsigned immediate = 0; immediate < 32; ++immediate)
		for (const bool scalar : {false, true})
			for (const bool q : {false, true}) {
				if (scalar && q)
					continue;
				if (!test::valid_duplicate(
				    scalar, q, immediate))
					continue;
				for (const auto registers : register_cases) {
					const auto initial = test::state(
						immediate + q * 37U + scalar * 71U +
						registers.destination);
					harness.check_one("simd-duplicate-element-behavior",
						test::duplicate_element_instruction(
							scalar, q, immediate, registers),
						initial,
						test::expected_duplicate_element(initial,
							scalar, q, immediate, registers),
						test::native_requirements());
				}
			}
}

void every_general_size_shape_ignored_bit_and_zero_register(
	test::engine_tests::EngineHarness &harness)
{
	for (unsigned immediate = 0; immediate < 32; ++immediate)
		for (const bool q : {false, true}) {
			if (!test::valid_duplicate(false, q, immediate))
				continue;
			for (const auto registers : register_cases) {
				auto initial = test::state(
					immediate + q * 43U + registers.source);
				initial.registers[7] = 0xf1e2'd3c4'b5a6'9788ULL;
				harness.check_one("simd-duplicate-general-behavior",
					test::duplicate_general_instruction(
						q, immediate, registers),
					initial,
					test::expected_duplicate_general(
						initial, q, immediate, registers),
					test::native_requirements());
			}
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	/* Scalar DUP is also the MOV (scalar) encoding. */
	every_element_size_lane_shape_and_alias(harness);
	every_general_size_shape_ignored_bit_and_zero_register(harness);
	harness.print_summary("arm64-simd-lane-move-duplicate");
	return 0;
}
