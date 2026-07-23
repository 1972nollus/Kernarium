// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_matrix_test_support.hpp"

namespace test = arm64_half_matrix_test;

int main()
{
	test::common::Harness harness;
	auto initial = test::common::state(100);
	test::clear_vectors(initial);
	const std::array<float, 8> addends{1, 2, 3, 4, 5, 6, 7, 8};
	const std::array<float, 8> left{1, 2, 3, 4, 2, 1, 4, 3};
	const std::array<float, 8> right{5, 6, 7, 8, 1, 2, 3, 4};
	for (unsigned lane = 0; lane < 8U; ++lane) {
		test::set_half(initial, 0, lane, test::half(addends[lane]));
		test::set_half(initial, 1, lane, test::half(left[lane]));
		test::set_half(initial, 2, lane, test::half(right[lane]));
	}
	const auto operation = test::instruction(false);
	const auto actual = harness.run_interpreter(operation, initial);
	const std::array<float, 8> expected{18, 25, 42, 57, 9, 16, 17, 32};
	for (unsigned lane = 0; lane < 8U; ++lane)
		test::common::require(test::half_lane(actual, 0, lane) ==
			test::half(expected[lane]));
	harness.require_differential(operation, initial);

	auto alias = initial;
	harness.require_differential(test::instruction(false, {1, 1, 1}), alias);
}
