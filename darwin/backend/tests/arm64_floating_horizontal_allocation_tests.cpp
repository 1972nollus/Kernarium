// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_horizontal_test_support.hpp"

namespace test = arm64_floating_horizontal_test;

namespace {

void every_operation_form_and_shape()
{
	test::common::Harness harness;
	unsigned count{};
	test::for_each_form([&](const test::Form &form) {
		const auto encoded = test::instruction(form, {31, 30, 29});
		harness.require_differential(encoded,
			test::common::state(++count));
	});
	test::common::require(count == 92U);
}

void reserved_shapes_are_rejected_before_side_effects()
{
	test::common::Harness harness;
	for (const auto width : {32U, 64U}) {
		auto scalar_fabd = test::instruction(
			{test::Operation::absolute_difference,
			 test::Kind::scalar_binary, width, 16U});
		scalar_fabd &= ~0x0080'0000U;
		harness.require_illegal(scalar_fabd, test::common::state(199));
	}
	for (const auto operation : test::minmax_operations) {
		auto scalar = test::instruction(
			{operation, test::Kind::scalar_binary, 32U, 16U});
		scalar &= ~0x00c0'0000U;
		scalar |= 2U << 22;
		harness.require_illegal(scalar, test::common::state(200));
	}
	for (const auto operation : {test::Operation::absolute_difference,
		test::Operation::add, test::Operation::maximum,
		test::Operation::maximum_number, test::Operation::minimum,
		test::Operation::minimum_number}) {
		const auto kind = operation == test::Operation::add ||
			(operation != test::Operation::absolute_difference &&
			 static_cast<unsigned>(operation) >= 2U) ?
			test::Kind::vector_pairwise : test::Kind::vector_binary;
		auto vector = test::instruction({operation, kind, 64U, 16U});
		vector &= ~0x4000'0000U;
		harness.require_illegal(vector, test::common::state(201));
	}
	for (const auto operation : test::minmax_operations) {
		auto vector = test::instruction(
			{operation, test::Kind::vector_binary, 64U, 16U});
		vector &= ~0x4000'0000U;
		harness.require_illegal(vector, test::common::state(205));
	}
	for (const auto operation : {test::Operation::add,
		test::Operation::maximum, test::Operation::maximum_number,
		test::Operation::minimum, test::Operation::minimum_number}) {
		auto scalar = test::instruction(
			{operation, test::Kind::scalar_pairwise, 16U, 16U});
		scalar |= 0x0040'0000U;
		harness.require_illegal(scalar, test::common::state(202));
	}
	for (const auto operation : test::minmax_operations) {
		auto reduction = test::instruction(
			{operation, test::Kind::reduction, 32U, 16U});
		reduction &= ~0x4000'0000U;
		harness.require_illegal(reduction, test::common::state(203));
		reduction |= 0x0040'0000U;
		harness.require_illegal(reduction, test::common::state(204));
	}
}

} // namespace

int main()
{
	every_operation_form_and_shape();
	reserved_shapes_are_rejected_before_side_effects();
}
