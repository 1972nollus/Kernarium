// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_lane_move_test_support.hpp"

#include <array>

namespace test = arm64_simd_lane_move_test;

namespace {

constexpr std::array register_cases{
	test::Registers{5, 12},
	test::Registers{5, 31},
	test::Registers{31, 15},
	test::Registers{14, 14},
};

void every_general_lane_and_zero_register(
	test::engine_tests::EngineHarness &harness)
{
	for (unsigned immediate = 0; immediate < 32; ++immediate) {
		if (!test::valid_insert(immediate))
			continue;
		for (const auto registers : register_cases) {
			auto initial = test::state(
				immediate + registers.destination + registers.source);
			initial.registers[12] = 0x8172'6354'4536'2718ULL;
			harness.check_one("FPSIMD-0207 INS general behavior",
				test::insert_general_instruction(
					immediate, registers),
				initial,
				test::expected_insert_general(
					initial, immediate, registers),
				test::native_requirements());
		}
	}
}

void every_element_lane_ignored_bit_and_alias(
	test::engine_tests::EngineHarness &harness)
{
	for (unsigned immediate = 0; immediate < 32; ++immediate) {
		if (!test::valid_insert(immediate))
			continue;
		for (unsigned source_immediate = 0; source_immediate < 16;
		     ++source_immediate)
			for (const auto registers : register_cases) {
				const auto initial = test::state(
					immediate * 17U + source_immediate * 3U +
					registers.destination);
				harness.check_one("FPSIMD-0206 INS element behavior",
					test::insert_element_instruction(immediate,
						source_immediate, registers),
					initial,
					test::expected_insert_element(initial, immediate,
						source_immediate, registers),
					test::native_requirements());
			}
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	/* These are also MOV (from general) and MOV (element). */
	every_general_lane_and_zero_register(harness);
	every_element_lane_ignored_bit_and_alias(harness);
	harness.print_summary("arm64-simd-lane-move-insert");
	return 0;
}
