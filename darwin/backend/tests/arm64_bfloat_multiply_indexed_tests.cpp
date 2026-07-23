// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_multiply_test_support.hpp"

namespace test = arm64_bfloat_multiply_test;

int main()
{
	test::common::Harness harness;
	for (const auto form : {test::Form::indexed_bottom,
		test::Form::indexed_top}) {
		for (unsigned index = 0; index < 8U; ++index) {
			auto initial = test::common::state(200 + index +
				100U * static_cast<unsigned>(test::top(form)));
			for (unsigned lane = 0; lane < 4U; ++lane) {
				test::set_single(initial, 0, lane, 0U);
				test::set_bfloat(initial, 1, lane * 2U,
					test::bfloat(static_cast<float>(lane + 1U)));
				test::set_bfloat(initial, 1, lane * 2U + 1U,
					test::bfloat(-static_cast<float>(lane + 1U)));
			}
			for (unsigned lane = 0; lane < 8U; ++lane)
				test::set_bfloat(initial, 2, lane,
					test::bfloat(static_cast<float>(lane + 1U)));
			const auto actual = harness.run_interpreter(
				test::instruction(form, index), initial);
			std::array<std::uint32_t, 4> expected{};
			for (unsigned lane = 0; lane < 4U; ++lane) {
				const float sign = test::top(form) ? -1.0F : 1.0F;
				expected[lane] = std::bit_cast<std::uint32_t>(sign *
					static_cast<float>((lane + 1U) * (index + 1U)));
			}
			test::require_lanes(actual, 0, expected);
			harness.require_differential(test::instruction(form, index), initial);
		}
	}
}
