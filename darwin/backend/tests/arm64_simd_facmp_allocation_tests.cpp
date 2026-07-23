// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_facmp_test_support.hpp"

namespace test = arm64_simd_facmp_test;
namespace base = arm64_simd_test;

namespace {

void every_shape_operation_and_alias()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const auto operation : {test::Operation::greater_equal,
		     test::Operation::greater})
			for (const unsigned destination : {28U, 29U, 30U}) {
				auto initial = base::state(shape.width + destination);
				const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
				std::array<bool, 8> matches{};
				for (unsigned lane = 0; lane < lanes; ++lane) {
					test::compare::write_lane(initial.vectors[29].bytes, shape,
						lane, test::scalar::negative_two(shape.width / 8U));
					test::compare::write_lane(initial.vectors[30].bytes, shape,
						lane, test::scalar::positive_one(shape.width / 8U));
					matches[lane] = true;
				}
				test::require_result(test::instruction(operation, shape,
					destination, 29U, 30U), shape, operation, destination, 30U,
					std::span{matches}.first(lanes), 0U, initial, harness);
			}
}

void scalar_nep_merges_from_second_source()
{
	base::Harness harness;
	for (unsigned shape_index = 0; shape_index < 3U; ++shape_index) {
		const auto shape = test::shapes[shape_index];
		auto initial = base::state(shape.width + 70U);
		initial.fpcr = 4U;
		test::compare::write_lane(initial.vectors[29].bytes, shape, 0U,
			test::scalar::negative_two(shape.width / 8U));
		test::compare::write_lane(initial.vectors[30].bytes, shape, 0U,
			test::scalar::positive_one(shape.width / 8U));
		const bool matches[]{true};
		test::require_result(test::instruction(test::Operation::greater, shape,
			28U, 29U, 30U), shape, test::Operation::greater, 28U, 30U,
			matches, 0U, initial, harness);
	}
}

void reserved_short_double_vectors_are_early_illegal()
{
	base::Harness harness;
	const test::Shape reserved{64U, 8U, false, false};
	for (const auto operation : {test::Operation::greater_equal,
	     test::Operation::greater})
		harness.require_illegal(test::instruction(operation, reserved,
			28U, 29U, 30U), base::state(71U));
}

} // namespace

int main()
{
	every_shape_operation_and_alias();
	scalar_nep_merges_from_second_source();
	reserved_short_double_vectors_are_early_illegal();
	return 0;
}
