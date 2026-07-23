// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_fused_long_test_support.hpp"

namespace test = arm64_half_fused_long_test;

int main()
{
	test::common::Harness harness;
	auto initial = test::common::state(100);
	for (unsigned lane = 0; lane < 4U; ++lane)
		test::set_single(initial, 0, lane,
			std::bit_cast<std::uint32_t>(10.0F * (lane + 1U)));
	for (unsigned lane = 0; lane < 8U; ++lane) {
		test::set_half(initial, 1, lane,
			test::half(static_cast<float>(lane + 1U)));
		test::set_half(initial, 2, lane,
			test::half(static_cast<float>(lane + 2U)));
	}

	for (const bool subtract : {false, true}) {
		for (const bool upper : {false, true}) {
			for (const bool wide : {false, true}) {
				const unsigned lanes = wide ? 4U : 2U;
				const unsigned first = upper ? lanes : 0U;
				std::array<std::uint32_t, 4> expected{};
				for (unsigned lane = 0; lane < lanes; ++lane) {
					const float product = static_cast<float>((first + lane + 1U) *
						(first + lane + 2U));
					const float value = 10.0F * (lane + 1U) +
						(subtract ? -product : product);
					expected[lane] = std::bit_cast<std::uint32_t>(value);
				}
				const auto operation = test::instruction(
					{false, subtract, upper, wide, 0U});
				const auto actual = harness.run_interpreter(operation, initial);
				test::require_lanes(actual, 0, expected, lanes);
				harness.require_differential(operation, initial);
			}
		}
	}

	auto alias = test::common::state(101);
	for (unsigned lane = 0; lane < 8U; ++lane)
		test::set_half(alias, 1, lane, test::half(1.0F));
	harness.require_differential(test::instruction(
		{false, false, false, true, 0U}, {1, 1, 1}), alias);
}
