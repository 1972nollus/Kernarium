// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_round_test_support.hpp"

namespace test = arm64_floating_round_test;

namespace {

void every_scalar_vector_operation_and_shape()
{
	test::common::Harness harness;
	unsigned count{};
	test::for_each_form([&](const test::Form &form) {
		++count;
		harness.require_differential(test::instruction(form, {31, 30}),
			test::common::state(count));
	});
	test::common::require(count == 76U);
}

void every_reserved_precision_is_rejected()
{
	test::common::Harness harness;
	for (const auto operation : test::standard_operations) {
		auto instruction = test::instruction(
			{operation, true, 32U, 16U});
		instruction &= ~0x00c0'0000U;
		instruction |= 2U << 22;
		harness.require_illegal(instruction, test::common::state(90));
	}
	for (const auto operation : test::sized_operations)
		for (const unsigned type : {2U, 3U}) {
			auto instruction = test::instruction(
				{operation, true, 32U, 16U});
			instruction &= ~0x00c0'0000U;
			instruction |= type << 22;
			harness.require_illegal(instruction, test::common::state(91));
		}
	for (const auto operation : test::standard_operations) {
		auto instruction = test::instruction(
			{operation, false, 64U, 16U});
		instruction &= ~0x4000'0000U;
		harness.require_illegal(instruction, test::common::state(92));
	}
	for (const auto operation : test::sized_operations) {
		auto instruction = test::instruction(
			{operation, false, 64U, 16U});
		instruction &= ~0x4000'0000U;
		harness.require_illegal(instruction, test::common::state(93));
	}
}

} // namespace

int main()
{
	every_scalar_vector_operation_and_shape();
	every_reserved_precision_is_rejected();
}
