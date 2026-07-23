// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_basic_floating_test_support.hpp"

namespace test = arm64_basic_floating_test;

namespace {

void every_scalar_and_vector_allocation()
{
	test::common::Harness harness;
	unsigned forms{};
	test::for_each_form([&](const test::Form &form) {
		++forms;
		harness.require_differential(test::instruction(form,
			{31, 30, 29}), test::common::state(forms));
	});
	test::common::require(forms == 43U);
}

void reserved_precisions_fail_before_state_changes()
{
	test::common::Harness harness;
	for (const auto operation : test::scalar_operations) {
		auto instruction = test::instruction(
			{operation, true, 32U, 16U}, {0, 1, 2});
		instruction &= ~0x00c0'0000U;
		instruction |= 2U << 22;
		harness.require_illegal(instruction, test::common::state(77));
	}
	for (const auto operation : test::vector_operations) {
		const test::Form form{operation, false, 64U, 16U};
		auto instruction = test::instruction(form, {0, 1, 2});
		instruction &= ~0x4000'0000U;
		harness.require_illegal(instruction, test::common::state(88));
	}
}

} // namespace

int main()
{
	every_scalar_and_vector_allocation();
	reserved_precisions_fail_before_state_changes();
}
