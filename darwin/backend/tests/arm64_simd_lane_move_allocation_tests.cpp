// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_lane_move_test_support.hpp"

namespace test = arm64_simd_lane_move_test;

namespace {

constexpr test::Registers registers{2, 3};

void duplicate_allocations(test::engine_tests::EngineHarness &harness)
{
	for (unsigned immediate = 0; immediate < 32; ++immediate) {
		const auto initial = test::state(immediate);
		const auto scalar = test::duplicate_element_instruction(
			true, false, immediate, registers);
		if (test::valid_duplicate(true, false, immediate))
			harness.check_one("simd-duplicate-scalar-allocation",
				scalar, initial,
				test::expected_duplicate_element(initial, true, false,
					immediate, registers),
				test::native_requirements());
		else
			harness.check_illegal(
				"simd-duplicate-scalar-reserved", scalar, initial);

		for (const bool q : {false, true}) {
			const auto element = test::duplicate_element_instruction(
				false, q, immediate, registers);
			const auto general = test::duplicate_general_instruction(
				q, immediate, registers);
			if (test::valid_duplicate(false, q, immediate)) {
				harness.check_one("simd-duplicate-element-allocation",
					element, initial,
					test::expected_duplicate_element(initial, false, q,
						immediate, registers),
					test::native_requirements());
				harness.check_one("simd-duplicate-general-allocation",
					general, initial,
					test::expected_duplicate_general(initial, q,
						immediate, registers),
					test::native_requirements());
			} else {
				harness.check_illegal(
					"simd-duplicate-element-reserved", element, initial);
				harness.check_illegal(
					"simd-duplicate-general-reserved", general, initial);
			}
		}
	}
}

void insert_allocations_and_ignored_bits(
	test::engine_tests::EngineHarness &harness)
{
	for (unsigned immediate = 0; immediate < 32; ++immediate) {
		const auto initial = test::state(immediate + 41U);
		const auto general = test::insert_general_instruction(
			immediate, registers);
		if (test::valid_insert(immediate))
			harness.check_one("simd-insert-general-allocation",
				general, initial,
				test::expected_insert_general(
					initial, immediate, registers),
				test::native_requirements());
		else
			harness.check_illegal(
				"simd-insert-general-reserved", general, initial);

		for (unsigned source_immediate = 0; source_immediate < 16;
		     ++source_immediate) {
			const auto element = test::insert_element_instruction(
				immediate, source_immediate, registers);
			if (test::valid_insert(immediate))
				harness.check_one("simd-insert-element-allocation",
					element, initial,
					test::expected_insert_element(initial, immediate,
						source_immediate, registers),
					test::native_requirements());
			else
				harness.check_illegal(
					"simd-insert-element-reserved", element, initial);
		}
	}
}

void gpr_move_allocations(test::engine_tests::EngineHarness &harness)
{
	for (unsigned immediate = 0; immediate < 32; ++immediate) {
		const auto initial = test::state(immediate + 83U);
		for (const bool signed_move : {false, true})
			for (const bool wide : {false, true}) {
				const auto instruction = test::gpr_move_instruction(
					signed_move, wide, immediate, registers);
				if (test::valid_gpr_move(
				    signed_move, wide, immediate))
					harness.check_one("simd-lane-to-gpr-allocation",
						instruction, initial,
						test::expected_gpr_move(initial, signed_move,
							wide, immediate, registers),
						test::native_requirements());
				else
					harness.check_illegal(
						"simd-lane-to-gpr-reserved",
						instruction, initial);
			}
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	duplicate_allocations(harness);
	insert_allocations_and_ignored_bits(harness);
	gpr_move_allocations(harness);
	harness.print_summary("arm64-simd-lane-move-allocation");
	return 0;
}
