// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_matrix_test_support.hpp"

namespace test = arm64_half_matrix_test;

int main()
{
	test::common::Harness harness;
	auto initial = test::common::state(200);
	test::clear_vectors(initial);
	for (unsigned lane = 0; lane < 4U; ++lane)
		test::set_single(initial, 0, lane,
			std::bit_cast<std::uint32_t>(static_cast<float>(lane + 1U)));
	for (unsigned lane = 0; lane < 8U; ++lane) {
		test::set_half(initial, 1, lane, test::half(static_cast<float>(lane + 1U)));
		test::set_half(initial, 2, lane, test::half(static_cast<float>(lane + 9U)));
	}
	const auto operation = test::instruction(true);
	const auto actual = harness.run_interpreter(operation, initial);
	const std::array<float, 4> expected{111, 152, 281, 386};
	for (unsigned lane = 0; lane < 4U; ++lane)
		test::common::require(test::single_lane(actual, 0, lane) ==
			std::bit_cast<std::uint32_t>(expected[lane]));
	harness.require_differential(operation, initial);

	harness.require_differential(test::instruction(true, {1, 1, 1}), initial);
}
