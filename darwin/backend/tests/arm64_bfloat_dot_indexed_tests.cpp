// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_dot_test_support.hpp"

namespace test = arm64_bfloat_dot_test;

int main()
{
	test::common::Harness harness;
	for (const bool wide : {false, true}) {
		for (unsigned index = 0; index < 4U; ++index) {
			auto initial = test::common::state(200 + index + 10U * wide);
			for (unsigned lane = 0; lane < 4U; ++lane) {
				test::set_single(initial, 0, lane, 0U);
				test::set_bfloat(initial, 1, lane * 2U,
					test::bfloat(static_cast<float>(lane + 1U)));
				test::set_bfloat(initial, 1, lane * 2U + 1U,
					test::bfloat(static_cast<float>(lane + 2U)));
			}
			for (unsigned pair = 0; pair < 4U; ++pair) {
				test::set_bfloat(initial, 2, pair * 2U,
					test::bfloat(static_cast<float>(pair + 1U)));
				test::set_bfloat(initial, 2, pair * 2U + 1U,
					test::bfloat(static_cast<float>(pair + 3U)));
			}
			std::array<std::uint32_t, 4> expected{};
			for (unsigned lane = 0; lane < 4U; ++lane) {
				const float value = static_cast<float>((lane + 1U) *
					(index + 1U) + (lane + 2U) * (index + 3U));
				expected[lane] = std::bit_cast<std::uint32_t>(value);
			}
			const auto operation = test::instruction({true, wide, index});
			const auto actual = harness.run_interpreter(operation, initial);
			test::require_lanes(actual, 0, expected, wide ? 4U : 2U);
			harness.require_differential(operation, initial);
		}
	}
}
