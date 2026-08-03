// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_matrix_test_support.hpp"

namespace test = arm64_half_matrix_test;

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts{};
	auto initial = test::state(100U);
	test::clear_vectors(initial);
	const std::array<float, 8> addends{1, 2, 3, 4, 5, 6, 7, 8};
	const std::array<float, 8> left{1, 2, 3, 4, 2, 1, 4, 3};
	const std::array<float, 8> right{5, 6, 7, 8, 1, 2, 3, 4};
	for (unsigned lane = 0U; lane < 8U; ++lane) {
		test::write_half(initial, 0U, lane,
			test::half(addends[lane]));
		test::write_half(initial, 1U, lane,
			test::half(left[lane]));
		test::write_half(initial, 2U, lane,
			test::half(right[lane]));
	}
	test::check(harness, "half-matrix-non-widening-behavior",
		false, {0U, 1U, 2U}, initial, counts);

	unsigned seed = 200U;
	for (const auto registers : test::register_aliases)
		test::check(harness, "half-matrix-non-widening-alias",
			false, registers, test::state(++seed), counts);

	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-matrix-non-widening");
}
