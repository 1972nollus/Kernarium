// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_matrix_test_support.hpp"

namespace test = arm64_bfloat_matrix_test;

int main()
{
	test::common::Harness harness;
	unsigned seed = 300;
	for (const auto registers : test::register_cases) {
		auto standard = test::common::state(++seed);
		harness.require_differential(test::instruction(registers), standard);
		auto extended = test::common::state(++seed);
		extended.fpcr = (1U << 13) | ((seed & 3U) << 22);
		harness.require_differential(test::instruction(registers), extended);
	}
}
