// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_floating_compare_test_support.hpp"

namespace test = arm64_simd_floating_compare_test;
namespace base = arm64_simd_test;

namespace {

void every_register_and_zero_shape()
{
	test::engine_tests::EngineHarness harness;
	for (const auto shape : test::shapes) {
		for (const auto operation : test::register_operations)
			for (unsigned destination = 0; destination < 32U;
			     ++destination)
				for (unsigned left = 0; left < 32U; ++left)
					for (unsigned right = 0; right < 32U; ++right) {
						auto initial = base::state(shape.width +
							static_cast<unsigned>(operation) * 7U +
							destination * 11U + left * 13U +
							right * 17U);
						initial.fpsr = 0x0800'0090U;
						const unsigned lanes =
							shape.vector_bytes /
							(shape.width / 8U);
						for (unsigned lane = 0; lane < lanes; ++lane) {
							test::write_lane(
								initial.vectors[left].bytes,
								shape, lane,
								test::positive_one(
									shape.width / 8U));
							test::write_lane(
								initial.vectors[right].bytes,
								shape, lane, 0U);
						}
						test::check_result(harness,
							"simd-floating-compare-register-allocation",
							shape, operation, false, destination,
							left, right, initial);
					}
		for (const auto operation : test::zero_operations)
			for (unsigned destination = 0; destination < 32U;
			     ++destination)
				for (unsigned source = 0; source < 32U; ++source) {
					auto initial = base::state(shape.width + 10U +
						static_cast<unsigned>(operation) * 7U +
						destination * 11U + source * 13U);
					const unsigned lanes =
						shape.vector_bytes / (shape.width / 8U);
					for (unsigned lane = 0; lane < lanes; ++lane)
						test::write_lane(initial.vectors[source].bytes,
							shape, lane,
							test::positive_one(
								shape.width / 8U));
					test::check_result(harness,
						"simd-floating-compare-zero-allocation",
						shape, operation, true, destination,
						source, 0U, initial);
				}
	}
	harness.print_summary("arm64-simd-floating-compare-allocation-valid");
}

void scalar_nep_and_all_aliases()
{
	test::engine_tests::EngineHarness harness;
	for (unsigned shape_index = 0; shape_index < 3U; ++shape_index)
		for (const auto operation : test::register_operations)
			for (const unsigned destination : {28U, 29U, 30U}) {
				const auto shape = test::shapes[shape_index];
				auto initial = base::state(shape.width + destination +
					static_cast<unsigned>(operation));
				initial.fpcr = 4U;
				test::write_lane(initial.vectors[29].bytes, shape, 0U,
					test::positive_one(shape.width / 8U));
				test::write_lane(initial.vectors[30].bytes, shape, 0U,
					test::positive_one(shape.width / 8U));
				test::check_result(harness,
					"simd-floating-compare-scalar-merging",
					shape, operation, false, destination,
					29U, 30U, initial);
			}
	for (unsigned shape_index = 0; shape_index < 3U; ++shape_index)
		for (const auto operation : test::zero_operations)
			for (const unsigned destination : {28U, 29U, 30U}) {
				const auto shape = test::shapes[shape_index];
				auto initial = base::state(shape.width + destination +
					static_cast<unsigned>(operation));
				initial.fpcr = 4U;
				test::write_lane(initial.vectors[29].bytes, shape, 0U,
					test::positive_one(shape.width / 8U));
				test::check_result(harness,
					"simd-floating-compare-scalar-zero-nep",
					shape, operation, true, destination,
					29U, 0U, initial);
			}
	harness.print_summary("arm64-simd-floating-compare-allocation-merging");
}

void reserved_short_double_vectors_are_early_illegal()
{
	test::engine_tests::EngineHarness harness;
	const test::Shape reserved{64U, 8U, false, false};
	for (const auto operation : test::register_operations)
		for (unsigned destination = 0; destination < 32U; ++destination)
			for (unsigned left = 0; left < 32U; ++left)
				for (unsigned right = 0; right < 32U; ++right)
					harness.check_illegal(
						"simd-floating-compare-register-reserved",
						test::register_instruction(operation,
							reserved, destination, left, right),
						base::state(50U + destination + left +
							right));
	for (const auto operation : test::zero_operations)
		for (unsigned destination = 0; destination < 32U; ++destination)
			for (unsigned source = 0; source < 32U; ++source)
				harness.check_illegal(
					"simd-floating-compare-zero-reserved",
					test::zero_instruction(operation, reserved,
						destination, source),
					base::state(51U + destination + source));
	harness.print_summary("arm64-simd-floating-compare-allocation-reserved");
}

} // namespace

int main()
{
	every_register_and_zero_shape();
	scalar_nep_and_all_aliases();
	reserved_short_double_vectors_are_early_illegal();
	return 0;
}
