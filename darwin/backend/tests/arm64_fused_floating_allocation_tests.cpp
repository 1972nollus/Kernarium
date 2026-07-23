// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fused_floating_test_support.hpp"

namespace test = arm64_fused_floating_test;

namespace {

void every_form_is_allocated()
{
	test::common::Harness harness;
	unsigned count{};
	test::for_each_form([&](const test::Form &form) {
		++count;
		harness.require_differential(test::instruction(form,
			{31, 30, 29, 28}), test::common::state(count));
	});
	test::common::require(count == 22U);
}

void reserved_shapes_are_rejected_early()
{
	test::common::Harness harness;
	for (const bool negate : {false, true})
		for (const bool subtract : {false, true}) {
			auto instruction = test::instruction(
				{true, negate, subtract, 32U, 16U});
			instruction |= 2U << 22;
			harness.require_illegal(instruction, test::common::state(50));
		}
	for (const bool subtract : {false, true}) {
		auto instruction = test::instruction(
			{false, false, subtract, 64U, 16U});
		instruction &= ~0x4000'0000U;
		harness.require_illegal(instruction, test::common::state(60));
	}
}

} // namespace

int main()
{
	every_form_is_allocated();
	reserved_shapes_are_rejected_early();
}
