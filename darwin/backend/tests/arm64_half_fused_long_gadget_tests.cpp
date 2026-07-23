// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_fused_long_test_support.hpp"

namespace test = arm64_half_fused_long_test;

int main()
{
	test::common::Harness harness;
	unsigned seed = 500;
	for (const bool subtract : {false, true})
		for (const bool upper : {false, true})
			for (const bool wide : {false, true})
				for (const auto registers : test::vector_register_cases) {
					auto initial = test::common::state(++seed);
					initial.fpcr = (seed & 3U) == 0U ? 2U : 0U;
					harness.require_differential(test::instruction(
						{false, subtract, upper, wide, 0U}, registers), initial);
				}
	for (const bool subtract : {false, true})
		for (const bool upper : {false, true})
			for (const bool wide : {false, true})
				for (unsigned index = 0; index < 8U; ++index)
					for (const auto registers : test::indexed_register_cases) {
						auto initial = test::common::state(++seed);
						initial.fpcr = index == 7U ? 2U : 0U;
						harness.require_differential(test::instruction(
							{true, subtract, upper, wide, index}, registers),
							initial);
					}
}
