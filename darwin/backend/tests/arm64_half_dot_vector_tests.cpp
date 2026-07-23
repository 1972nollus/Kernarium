// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_dot_test_support.hpp"

namespace test = arm64_half_dot_test;

int main()
{
	test::common::Harness harness;
	auto initial = test::common::state(100);
	test::matrix::clear_vectors(initial);
	for (unsigned lane = 0; lane < 4U; ++lane) {
		test::matrix::set_single(initial, 0, lane,
			std::bit_cast<std::uint32_t>(static_cast<float>(lane + 1U)));
		test::matrix::set_half(initial, 1, lane * 2U,
			test::matrix::half(static_cast<float>(lane * 2U + 1U)));
		test::matrix::set_half(initial, 1, lane * 2U + 1U,
			test::matrix::half(static_cast<float>(lane * 2U + 2U)));
		test::matrix::set_half(initial, 2, lane * 2U,
			test::matrix::half(static_cast<float>(lane * 2U + 5U)));
		test::matrix::set_half(initial, 2, lane * 2U + 1U,
			test::matrix::half(static_cast<float>(lane * 2U + 6U)));
	}
	const auto operation = test::instruction({false, true, 0});
	const auto actual = harness.run_interpreter(operation, initial);
	const std::array<float, 4> expected{18, 55, 108, 177};
	for (unsigned lane = 0; lane < 4U; ++lane)
		test::common::require(test::matrix::single_lane(actual, 0, lane) ==
			std::bit_cast<std::uint32_t>(expected[lane]));
	harness.require_differential(operation, initial);

	auto fused = test::common::state(101);
	test::matrix::clear_vectors(fused);
	test::matrix::set_half(fused, 1, 0, 0x3c01U);
	test::matrix::set_half(fused, 1, 1, test::matrix::half(1.0F));
	test::matrix::set_half(fused, 2, 0, 0x3c01U);
	test::matrix::set_half(fused, 2, 1, test::matrix::half(-1.0F));
	const auto fused_result = harness.run_interpreter(
		test::instruction({false, false, 0}), fused);
	test::common::require(test::matrix::single_lane(fused_result, 0, 0) ==
		0x3b00'1000U);
	harness.require_differential(test::instruction({false, false, 0}), fused);
}
