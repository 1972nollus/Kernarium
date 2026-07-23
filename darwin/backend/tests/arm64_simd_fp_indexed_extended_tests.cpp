// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fp_indexed_test_support.hpp"

namespace test = arm64_simd_fp_indexed_test;
namespace base = arm64_simd_test;

namespace {

[[nodiscard]] std::uint64_t sign_mask(unsigned lane_bytes)
{
	return std::uint64_t{1} << (lane_bytes * 8U - 1U);
}

[[nodiscard]] std::uint64_t infinity_bits(unsigned lane_bytes)
{
	if (lane_bytes == 2U)
		return 0x7c00U;
	if (lane_bytes == 4U)
		return 0x7f80'0000U;
	return 0x7ff0'0000'0000'0000ULL;
}

void ordinary_fmulx_for_every_shape_and_index()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (unsigned index = 0; index < shape.indexes; ++index) {
			const unsigned element_register = shape.size == 0U ? 13U : 19U;
			auto initial = base::state(0U);
			initial.fpsr = 0x80U;
			test::fill_bits(initial.vectors[29].bytes, shape.lane_bytes,
				test::floating_bits(shape.lane_bytes, 2.0));
			test::fill_bits(initial.vectors[element_register].bytes,
				shape.lane_bytes, test::floating_bits(shape.lane_bytes, -4.0));
			test::write_bits(initial.vectors[element_register].bytes,
				shape.lane_bytes, index,
				test::floating_bits(shape.lane_bytes, 3.0));
			const auto encoded = test::instruction(test::Operation::fmulx,
				shape, 28U, 29U, element_register, index);
			test::require_uniform_result(encoded, shape,
				test::Operation::fmulx, 28U, 29U, initial,
				test::floating_bits(shape.lane_bytes, 6.0), 0x80U, harness);
		}
}

void zero_times_infinity_is_signed_two_without_invalid()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const bool negative : {false, true}) {
			const unsigned element_register = shape.size == 0U ? 13U : 19U;
			const unsigned index = shape.indexes - 1U;
			auto initial = base::state(0U);
			initial.fpsr = 0x80U;
			const auto zero = negative ? sign_mask(shape.lane_bytes) : 0U;
			test::fill_bits(initial.vectors[29].bytes, shape.lane_bytes, zero);
			test::fill_bits(initial.vectors[element_register].bytes,
				shape.lane_bytes, test::floating_bits(shape.lane_bytes, 1.0));
			test::write_bits(initial.vectors[element_register].bytes,
				shape.lane_bytes, index, infinity_bits(shape.lane_bytes));
			const auto encoded = test::instruction(test::Operation::fmulx,
				shape, 28U, 29U, element_register, index);
			const auto two = test::floating_bits(shape.lane_bytes, 2.0) |
				(negative ? sign_mask(shape.lane_bytes) : 0U);
			test::require_uniform_result(encoded, shape,
				test::Operation::fmulx, 28U, 29U, initial, two, 0x80U,
				harness);
		}
}

void scalar_nep_merges_from_the_architectural_source()
{
	base::Harness harness;
	for (const auto operation : {test::Operation::fmla, test::Operation::fmls,
	     test::Operation::fmul, test::Operation::fmulx})
		for (const auto shape : {test::shapes[0], test::shapes[3],
		     test::shapes[6]}) {
			const unsigned element_register = shape.size == 0U ? 13U : 19U;
			auto initial = base::state(18U);
			initial.fpcr = 4U;
			initial.fpsr = 0;
			test::write_bits(initial.vectors[28].bytes, shape.lane_bytes, 0U,
				test::floating_bits(shape.lane_bytes, 1.0));
			test::write_bits(initial.vectors[29].bytes, shape.lane_bytes, 0U,
				test::floating_bits(shape.lane_bytes, 2.0));
			test::fill_bits(initial.vectors[element_register].bytes,
				shape.lane_bytes, test::floating_bits(shape.lane_bytes, 3.0));
			const double value = operation == test::Operation::fmla ? 7.0 :
				operation == test::Operation::fmls ? -5.0 : 6.0;
			const auto encoded = test::instruction(operation, shape,
				28U, 29U, element_register, shape.indexes - 1U);
			test::require_uniform_result(encoded, shape, operation, 28U, 29U,
				initial, test::floating_bits(shape.lane_bytes, value), 0U,
				harness);
		}
}

void exception_flags_are_cumulative()
{
	base::Harness harness;
	const auto shape = test::shapes[5];
	auto invalid = base::state(0U);
	invalid.fpsr = 0x80U;
	test::fill_bits(invalid.vectors[29].bytes, 4U, 0U);
	test::fill_bits(invalid.vectors[19].bytes, 4U, 0x7f80'0000U);
	const auto invalid_instruction = test::instruction(test::Operation::fmul,
		shape, 28U, 29U, 19U, 0U);
	const auto invalid_result = harness.run_interpreter(invalid_instruction,
		invalid);
	base::require((invalid_result.fpsr & 0x81U) == 0x81U);
	harness.require_differential(invalid_instruction, invalid);

	auto overflow = base::state(0U);
	overflow.fpsr = 0x80U;
	test::fill_bits(overflow.vectors[29].bytes, 4U, 0x7f7f'ffffU);
	test::fill_bits(overflow.vectors[19].bytes, 4U, 0x4000'0000U);
	const auto overflow_instruction = test::instruction(test::Operation::fmul,
		shape, 28U, 29U, 19U, 0U);
	const auto overflow_result = harness.run_interpreter(overflow_instruction,
		overflow);
	base::require((overflow_result.fpsr & 0x94U) == 0x94U);
	harness.require_differential(overflow_instruction, overflow);
}

} // namespace

int main()
{
	ordinary_fmulx_for_every_shape_and_index();
	zero_times_infinity_is_signed_two_without_invalid();
	scalar_nep_merges_from_the_architectural_source();
	exception_flags_are_cumulative();
	return 0;
}
