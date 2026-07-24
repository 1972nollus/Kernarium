// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_horizontal_test_support.hpp"

namespace test = arm64_floating_horizontal_test;

int main()
{
	test::common::Harness harness;
	unsigned seed = 900;
	test::for_each_form([&](const test::Form &form) {
		for (const auto registers : test::register_cases)
			harness.require_differential(test::instruction(form, registers),
				test::common::state(++seed));
	});

	const test::Form reported{test::Operation::absolute_difference,
		test::Kind::scalar_binary, 64U, 16U};
	const auto instruction = test::instruction(reported, {30, 15, 30});
	test::common::require(instruction == 0x7efe'd5feU);
	harness.require_differential(instruction, test::common::state(++seed));
}
