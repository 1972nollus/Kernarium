// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_conversion_test_support.hpp"

namespace test = arm64_bfloat_conversion_test;

int main()
{
	test::common::Harness harness;
	unsigned seed = 70;
	for (const auto form : {test::Form::scalar, test::Form::lower,
		test::Form::upper})
		for (const auto registers : test::register_cases)
			harness.require_differential(test::instruction(form, registers),
				test::common::state(++seed));
}
