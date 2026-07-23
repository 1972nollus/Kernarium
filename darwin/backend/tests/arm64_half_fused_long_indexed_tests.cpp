// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_fused_long_test_support.hpp"

namespace test = arm64_half_fused_long_test;

int main()
{
	test::common::Harness harness;
	for (const bool subtract : {false, true})
		for (const bool upper : {false, true})
			for (const bool wide : {false, true})
				for (unsigned index = 0; index < 8U; ++index) {
					auto initial = test::common::state(200 + index);
					for (unsigned lane = 0; lane < 4U; ++lane)
						test::set_single(initial, 0, lane, 0U);
					for (unsigned lane = 0; lane < 8U; ++lane) {
						test::set_half(initial, 1, lane,
							test::half(static_cast<float>(lane + 1U)));
						test::set_half(initial, 2, lane,
							test::half(static_cast<float>(lane + 2U)));
					}
					const unsigned lanes = wide ? 4U : 2U;
					const unsigned first = upper ? lanes : 0U;
					std::array<std::uint32_t, 4> expected{};
					for (unsigned lane = 0; lane < lanes; ++lane) {
						float value = static_cast<float>((first + lane + 1U) *
							(index + 2U));
						if (subtract)
							value = -value;
						expected[lane] = std::bit_cast<std::uint32_t>(value);
					}
					const auto operation = test::instruction(
						{true, subtract, upper, wide, index});
					const auto actual = harness.run_interpreter(operation, initial);
					test::require_lanes(actual, 0, expected, lanes);
					harness.require_differential(operation, initial);
				}
}
