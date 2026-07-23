// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_reciprocal_test_support.hpp"

namespace test = arm64_floating_reciprocal_test;

int main()
{
	test::common::Harness harness;
	unsigned count{};
	test::for_each_form([&](const test::Form &form) {
		harness.require_differential(test::instruction(form, {31, 30, 29}),
			test::common::state(++count));
	});
	test::common::require(count == 35U);
	for (const auto operation : {test::Operation::reciprocal_estimate,
		test::Operation::reciprocal_step,
		test::Operation::reciprocal_sqrt_estimate,
		test::Operation::reciprocal_sqrt_step}) {
		auto encoded = test::instruction({operation, false, 64U, 16U});
		encoded &= ~0x4000'0000U;
		harness.require_illegal(encoded, test::common::state(100));
	}
}
