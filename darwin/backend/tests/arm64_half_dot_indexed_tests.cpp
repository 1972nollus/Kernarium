// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_dot_test_support.hpp"

namespace test = arm64_half_dot_test;

int main()
{
	test::common::Harness harness;
	for (unsigned index = 0; index < 4U; ++index) {
		auto initial = test::common::state(200 + index);
		test::matrix::clear_vectors(initial);
		for (unsigned lane = 0; lane < 4U; ++lane) {
			test::matrix::set_single(initial, 0, lane,
				std::bit_cast<std::uint32_t>(1.0F));
			test::matrix::set_half(initial, 1, lane * 2U,
				test::matrix::half(static_cast<float>(lane + 1U)));
			test::matrix::set_half(initial, 1, lane * 2U + 1U,
				test::matrix::half(static_cast<float>(lane + 2U)));
			test::matrix::set_half(initial, 31, lane * 2U,
				test::matrix::half(static_cast<float>(lane * 2U + 3U)));
			test::matrix::set_half(initial, 31, lane * 2U + 1U,
				test::matrix::half(static_cast<float>(lane * 2U + 4U)));
		}
		const auto operation = test::instruction({true, true, index},
			{0, 1, 31});
		const auto actual = harness.run_interpreter(operation, initial);
		const float a = static_cast<float>(index * 2U + 3U);
		const float b = static_cast<float>(index * 2U + 4U);
		for (unsigned lane = 0; lane < 4U; ++lane) {
			const float expected = 1.0F +
				static_cast<float>(lane + 1U) * a +
				static_cast<float>(lane + 2U) * b;
			test::common::require(test::matrix::single_lane(actual, 0, lane) ==
				std::bit_cast<std::uint32_t>(expected));
		}
		harness.require_differential(operation, initial);
	}
}
