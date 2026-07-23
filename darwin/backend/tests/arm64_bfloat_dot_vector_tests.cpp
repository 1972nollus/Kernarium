// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_dot_test_support.hpp"

namespace test = arm64_bfloat_dot_test;

int main()
{
	test::common::Harness harness;
	auto initial = test::common::state(100);
	const std::array<float, 8> left{1.0F, 2.0F, 3.0F, 4.0F,
		5.0F, 6.0F, 7.0F, 8.0F};
	const std::array<float, 8> right{5.0F, 6.0F, 7.0F, 8.0F,
		9.0F, 10.0F, 11.0F, 12.0F};
	for (unsigned lane = 0; lane < 8U; ++lane) {
		test::set_bfloat(initial, 1, lane, test::bfloat(left[lane]));
		test::set_bfloat(initial, 2, lane, test::bfloat(right[lane]));
	}
	for (unsigned lane = 0; lane < 4U; ++lane)
		test::set_single(initial, 0, lane,
			std::bit_cast<std::uint32_t>(10.0F * (lane + 1U)));
	const std::array expected{
		std::bit_cast<std::uint32_t>(27.0F),
		std::bit_cast<std::uint32_t>(73.0F),
		std::bit_cast<std::uint32_t>(135.0F),
		std::bit_cast<std::uint32_t>(213.0F)};

	for (const bool wide : {false, true}) {
		const auto operation = test::instruction({false, wide, 0U});
		const auto actual = harness.run_interpreter(operation, initial);
		test::require_lanes(actual, 0, expected, wide ? 4U : 2U);
		harness.require_differential(operation, initial);
	}

	/* Read-before-write behavior for every overlapping register. */
	auto alias = test::common::state(101);
	for (unsigned lane = 0; lane < 8U; ++lane)
		test::set_bfloat(alias, 1, lane, test::bfloat(1.0F));
	harness.require_differential(test::instruction({false, true, 0U},
		{1, 1, 1}), alias);
}
