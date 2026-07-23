// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fp_indexed_test_support.hpp"

namespace test = arm64_simd_fp_indexed_test;
namespace base = arm64_simd_test;

namespace {

[[nodiscard]] double expected_value(test::Operation operation)
{
	if (operation == test::Operation::fmla)
		return 7.0;
	if (operation == test::Operation::fmls)
		return -5.0;
	return 6.0;
}

void exact_results_for_every_shape_and_index()
{
	base::Harness harness;
	for (const auto operation : {test::Operation::fmla, test::Operation::fmls,
	     test::Operation::fmul})
		for (const auto shape : test::shapes)
			for (unsigned index = 0; index < shape.indexes; ++index) {
				const unsigned element_register = shape.size == 0U ? 13U : 19U;
				auto initial = base::state(0U);
				initial.fpsr = 0x80U;
				test::fill_bits(initial.vectors[28].bytes, shape.lane_bytes,
					test::floating_bits(shape.lane_bytes, 1.0));
				test::fill_bits(initial.vectors[29].bytes, shape.lane_bytes,
					test::floating_bits(shape.lane_bytes, 2.0));
				test::fill_bits(initial.vectors[element_register].bytes,
					shape.lane_bytes,
					test::floating_bits(shape.lane_bytes, -4.0));
				test::write_bits(initial.vectors[element_register].bytes,
					shape.lane_bytes, index,
					test::floating_bits(shape.lane_bytes, 3.0));
				const auto encoded = test::instruction(operation, shape, 28U,
					29U, element_register, index);
				test::require_uniform_result(encoded, shape, operation, 28U,
					29U, initial, test::floating_bits(shape.lane_bytes,
						expected_value(operation)), 0x80U, harness);
			}
}

void source_destination_and_element_aliases_use_original_values()
{
	base::Harness harness;
	for (const auto operation : {test::Operation::fmla, test::Operation::fmls,
	     test::Operation::fmul})
		for (const auto shape : {test::shapes[5], test::shapes[6]}) {
			auto initial = base::state(0U);
			initial.fpsr = 0x80U;
			test::fill_bits(initial.vectors[3].bytes, shape.lane_bytes,
				test::floating_bits(shape.lane_bytes, 2.0));
			const double output = operation == test::Operation::fmla ? 6.0 :
				operation == test::Operation::fmls ? -2.0 : 4.0;
			const auto encoded = test::instruction(operation, shape,
				3U, 3U, 3U, shape.indexes - 1U);
			test::require_uniform_result(encoded, shape, operation, 3U, 3U,
				initial, test::floating_bits(shape.lane_bytes, output),
				0x80U, harness);
		}
}

void multiply_add_is_fused()
{
	base::Harness harness;
	for (const auto operation : {test::Operation::fmla, test::Operation::fmls})
		for (const auto shape : {test::shapes[3], test::shapes[5]}) {
			auto initial = base::state(0U);
			initial.fpsr = 0;
			test::fill_bits(initial.vectors[29].bytes, 4U, 0x3f80'0001U);
			test::fill_bits(initial.vectors[19].bytes, 4U, 0x3f80'0001U);
			const auto accumulator = operation == test::Operation::fmla ?
				0xbf80'0002U : 0x3f80'0002U;
			test::fill_bits(initial.vectors[28].bytes, 4U, accumulator);
			const auto expected = operation == test::Operation::fmla ?
				0x2880'0000U : 0xa880'0000U;
			const auto encoded = test::instruction(operation, shape,
				28U, 29U, 19U, 3U);
			test::require_uniform_result(encoded, shape, operation, 28U, 29U,
				initial, expected, 0U, harness);
		}
}

} // namespace

int main()
{
	exact_results_for_every_shape_and_index();
	source_destination_and_element_aliases_use_original_values();
	multiply_add_is_fused();
	return 0;
}
