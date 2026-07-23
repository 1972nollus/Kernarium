// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_absolute_minmax_test_support.hpp"

namespace test = arm64_floating_absolute_minmax_test;

int main()
{
	test::common::Harness harness;
	unsigned seed = 60;
	test::for_each_form([&](const test::Form &form) {
		for (const auto registers : test::register_cases)
			harness.require_differential(test::instruction(form, registers),
				test::common::state(++seed));
	});
}
