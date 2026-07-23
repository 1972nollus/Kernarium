// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_floating_compare_test_support.hpp"

namespace test = arm64_simd_floating_compare_test;
namespace base = arm64_simd_test;

namespace {

void every_register_and_zero_shape()
{
	base::Harness harness;
	for (const auto shape : test::shapes) {
		for (const auto operation : {test::Operation::equal,
		     test::Operation::greater_equal, test::Operation::greater}) {
			auto initial = base::state(shape.width + static_cast<unsigned>(operation));
			initial.fpsr = 0x0800'0090U;
			const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
			const bool match = operation != test::Operation::equal;
			std::array<bool, 8> matches{};
			for (unsigned lane = 0; lane < lanes; ++lane) {
				test::write_lane(initial.vectors[29].bytes, shape, lane,
					test::scalar::positive_one(shape.width / 8U));
				test::write_lane(initial.vectors[30].bytes, shape, lane, 0U);
				matches[lane] = match;
			}
			test::require_result(test::register_instruction(operation, shape,
				28U, 29U, 30U), shape, operation, false, 28U, 30U,
				std::span{matches}.first(lanes), 0U, initial, harness);
		}
		for (const auto operation : {test::Operation::equal,
		     test::Operation::greater_equal, test::Operation::greater,
		     test::Operation::less_equal, test::Operation::less}) {
			auto initial = base::state(shape.width + 10U +
				static_cast<unsigned>(operation));
			const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
			const bool match = operation == test::Operation::greater_equal ||
				operation == test::Operation::greater;
			std::array<bool, 8> matches{};
			for (unsigned lane = 0; lane < lanes; ++lane) {
				test::write_lane(initial.vectors[29].bytes, shape, lane,
					test::scalar::positive_one(shape.width / 8U));
				matches[lane] = match;
			}
			test::require_result(test::zero_instruction(operation, shape,
				28U, 29U), shape, operation, true, 28U, 0U,
				std::span{matches}.first(lanes), 0U, initial, harness);
		}
	}
}

void scalar_nep_and_all_aliases()
{
	base::Harness harness;
	for (unsigned shape_index = 0; shape_index < 3U; ++shape_index)
		for (const unsigned destination : {28U, 29U, 30U}) {
			const auto shape = test::shapes[shape_index];
			auto initial = base::state(shape.width + destination);
			initial.fpcr = 4U;
			test::write_lane(initial.vectors[29].bytes, shape, 0U,
				test::scalar::positive_one(shape.width / 8U));
			test::write_lane(initial.vectors[30].bytes, shape, 0U,
				test::scalar::positive_one(shape.width / 8U));
			const bool matches[]{true};
			test::require_result(test::register_instruction(
				test::Operation::equal, shape, destination, 29U, 30U),
				shape, test::Operation::equal, false, destination, 30U,
				matches, 0U, initial, harness);
		}
}

void reserved_short_double_vectors_are_early_illegal()
{
	base::Harness harness;
	const test::Shape reserved{64U, 8U, false, false};
	for (const auto operation : {test::Operation::equal,
		     test::Operation::greater_equal, test::Operation::greater})
		harness.require_illegal(test::register_instruction(operation,
			reserved, 28U, 29U, 30U), base::state(50U));
	for (const auto operation : {test::Operation::equal,
		     test::Operation::greater_equal, test::Operation::greater,
		     test::Operation::less_equal, test::Operation::less})
		harness.require_illegal(test::zero_instruction(operation,
			reserved, 28U, 29U), base::state(51U));
}

} // namespace

int main()
{
	every_register_and_zero_shape();
	scalar_nep_and_all_aliases();
	reserved_short_double_vectors_are_early_illegal();
	return 0;
}
