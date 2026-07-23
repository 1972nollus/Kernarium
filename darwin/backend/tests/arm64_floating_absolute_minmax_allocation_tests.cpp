// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_absolute_minmax_test_support.hpp"

namespace test = arm64_floating_absolute_minmax_test;

int main()
{
	test::common::Harness harness;
	unsigned count{};
	test::for_each_form([&](const test::Form &form) {
		harness.require_differential(test::instruction(form, {31, 30, 29}),
			test::common::state(++count));
	});
	test::common::require(count == 10U);
	for (const auto operation : {test::Operation::absolute_maximum,
		test::Operation::absolute_minimum}) {
		auto invalid = test::instruction({operation, 64U, 16U});
		invalid &= ~0x4000'0000U;
		harness.require_illegal(invalid, test::common::state(++count));
	}
}
