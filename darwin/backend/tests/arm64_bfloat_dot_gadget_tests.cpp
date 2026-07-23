// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_dot_test_support.hpp"

namespace test = arm64_bfloat_dot_test;

int main()
{
	test::common::Harness harness;
	unsigned seed = 500;
	for (const bool wide : {false, true})
		for (const auto registers : test::register_cases) {
			auto initial = test::common::state(++seed);
			initial.fpcr = (seed & 1U) != 0U ? 0U : 1U << 13;
			harness.require_differential(test::instruction({false, wide, 0U},
				registers), initial);
		}
	for (const bool wide : {false, true})
		for (unsigned index = 0; index < 4U; ++index)
			for (const auto registers : test::register_cases) {
				auto initial = test::common::state(++seed);
				initial.fpcr = index == 3U ? (1U << 13) | 2U : 0U;
				harness.require_differential(test::instruction(
					{true, wide, index}, registers), initial);
			}
}
