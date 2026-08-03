// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_matrix_test_support.hpp"

namespace test = arm64_simd_matrix_test;

namespace {

constexpr test::Registers registers{28U, 29U, 30U};

void exact_encodings_match_the_specification_diagrams()
{
	test::require(test::instruction(test::Mode::signed_signed,
			{0U, 1U, 2U}) == 0x4e82'a420U,
		"signed matrix encoding mismatch");
	test::require(test::instruction(test::Mode::unsigned_unsigned,
			{0U, 1U, 2U}) == 0x6e82'a420U,
		"unsigned matrix encoding mismatch");
	test::require(test::instruction(test::Mode::unsigned_signed,
			{0U, 1U, 2U}) == 0x4e82'ac20U,
		"mixed matrix encoding mismatch");
}

void entire_q_u_size_and_operation_partition()
{
	test::engine_tests::EngineHarness harness;
	for (const bool q : {false, true})
		for (const bool u : {false, true})
			for (unsigned size = 0; size < 4U; ++size)
				for (const bool alternate : {false, true}) {
					const auto initial = test::state(
						(q ? 101U : 0U) + (u ? 37U : 0U) +
						size * 11U + (alternate ? 5U : 0U));
					const auto encoded = test::raw_instruction(
						q, u, size, alternate, registers);
					if (test::valid_encoding(q, u, size, alternate)) {
						const auto mode = test::mode_for(u, alternate);
						harness.check_one(test::inventory_name(mode),
							encoded, initial,
							test::expected_state(initial, mode, registers),
							test::native_requirements());
					} else {
						harness.check_illegal("matrix reserved encoding",
							encoded, initial,
							test::native_requirements());
					}
				}
	harness.print_summary("simd-matrix-allocation");
}

} // namespace

int main()
{
	exact_encodings_match_the_specification_diagrams();
	entire_q_u_size_and_operation_partition();
	return 0;
}
