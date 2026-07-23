// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_complex_test_support.hpp"

namespace test = arm64_floating_complex_test;

int main()
{
	test::common::Harness harness;
	unsigned count{};
	test::for_each_form([&](const test::Form &form) {
		harness.require_differential(test::instruction(form, {31, 30, 29}),
			test::common::state(++count));
	});
	test::common::require(count == 62U);

	for (const auto operation : {test::Operation::add,
		test::Operation::multiply_accumulate}) {
		auto size_zero = test::instruction(
			{operation, false, 16U, 8U, 1U, 0U});
		size_zero &= ~0x00c0'0000U;
		harness.require_illegal(size_zero, test::common::state(100));
		auto one_double = test::instruction(
			{operation, false, 64U, 16U, 1U, 0U});
		one_double &= ~0x4000'0000U;
		harness.require_illegal(one_double, test::common::state(101));
	}
	auto indexed = test::instruction({test::Operation::multiply_accumulate,
		true, 16U, 16U, 0U, 0U});
	auto invalid = indexed & ~0x00c0'0000U;
	harness.require_illegal(invalid, test::common::state(102));
	invalid = (indexed & ~0x00c0'0000U) | 0x00c0'0000U;
	harness.require_illegal(invalid, test::common::state(103));
	invalid = test::instruction({test::Operation::multiply_accumulate,
		true, 32U, 16U, 0U, 0U}) & ~0x4000'0000U;
	harness.require_illegal(invalid, test::common::state(104));
	invalid = test::instruction({test::Operation::multiply_accumulate,
		true, 32U, 16U, 0U, 0U}) | 0x0020'0000U;
	harness.require_illegal(invalid, test::common::state(105));
	invalid = test::instruction({test::Operation::multiply_accumulate,
		true, 16U, 8U, 0U, 0U}) | 0x0000'0800U;
	harness.require_illegal(invalid, test::common::state(106));
}
