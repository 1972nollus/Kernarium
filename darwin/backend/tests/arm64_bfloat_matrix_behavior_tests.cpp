// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_matrix_test_support.hpp"

namespace test = arm64_bfloat_matrix_test;

int main()
{
	test::common::Harness harness;
	auto initial = test::common::state(100);
	for (unsigned lane = 0; lane < 4U; ++lane) {
		test::dot::set_single(initial, 0, lane,
			std::bit_cast<std::uint32_t>(static_cast<float>(lane + 1U)));
		test::dot::set_bfloat(initial, 1, lane,
			test::dot::bfloat(static_cast<float>(lane + 1U)));
		test::dot::set_bfloat(initial, 1, lane + 4U,
			test::dot::bfloat(static_cast<float>(lane + 5U)));
		test::dot::set_bfloat(initial, 2, lane,
			test::dot::bfloat(static_cast<float>(lane + 9U)));
		test::dot::set_bfloat(initial, 2, lane + 4U,
			test::dot::bfloat(static_cast<float>(lane + 13U)));
	}
	const auto actual = harness.run_interpreter(test::instruction(), initial);
	test::dot::require_lanes(actual, 0, {
		std::bit_cast<std::uint32_t>(111.0F),
		std::bit_cast<std::uint32_t>(152.0F),
		std::bit_cast<std::uint32_t>(281.0F),
		std::bit_cast<std::uint32_t>(386.0F)}, 4U);
	harness.require_differential(test::instruction(), initial);

	auto alias = test::common::state(101);
	for (unsigned lane = 0; lane < 8U; ++lane)
		test::dot::set_bfloat(alias, 1, lane, test::dot::bfloat(1.0F));
	harness.require_differential(test::instruction({1, 1, 1}), alias);
}
