// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_matrix_test_support.hpp"

namespace test = arm64_half_matrix_test;

int main()
{
	test::common::Harness harness;
	unsigned seed = 400;
	for (const bool widening : {false, true}) {
		for (const auto registers : test::register_cases) {
			auto ordinary = test::common::state(++seed);
			ordinary.fpcr = (seed & 3U) << 22;
			harness.require_differential(
				test::instruction(widening, registers), ordinary);
			auto controlled = test::common::state(++seed);
			controlled.fpcr = 3U | (1U << 19) | (1U << 24) |
				((seed & 3U) << 22);
			harness.require_differential(
				test::instruction(widening, registers), controlled);
		}
	}
}
