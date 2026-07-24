// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fabs_fneg_test_support.hpp"

namespace test = arm64_simd_fabs_fneg_test;
namespace base = arm64_simd_test;
namespace fp = arm64_simd_fp_indexed_test;

namespace {

void every_scalar_and_vector_encoding()
{
	base::Harness harness;
	for (const auto operation : {test::Operation::absolute,
		     test::Operation::negate}) {
		for (const auto shape : test::shapes) {
			auto initial = base::state(shape.lane_bytes + shape.scalar + shape.q);
			initial.fpsr = 0x0800'0080U;
			fp::fill_bits(initial.vectors[29].bytes, shape.lane_bytes,
				fp::floating_bits(shape.lane_bytes, -1.5));
			const auto encoded = test::instruction(operation, shape, 28U, 29U);
			test::require_result(encoded, shape, operation, 28U, 29U,
				initial, harness);
		}
	}
}

void canonical_vector_words_match_the_architecture()
{
	constexpr test::Shape half_d{2U, false, false};
	constexpr test::Shape half_q{2U, false, true};
	constexpr test::Shape single_d{4U, false, false};
	constexpr test::Shape single_q{4U, false, true};
	constexpr test::Shape double_q{8U, false, true};
	base::require(test::instruction(
		test::Operation::absolute, half_d, 0U, 0U) == 0x0ef8'f800U);
	base::require(test::instruction(
		test::Operation::absolute, half_q, 0U, 0U) == 0x4ef8'f800U);
	base::require(test::instruction(
		test::Operation::absolute, single_d, 0U, 0U) == 0x0ea0'f800U);
	base::require(test::instruction(
		test::Operation::absolute, single_q, 0U, 0U) == 0x4ea0'f800U);
	base::require(test::instruction(
		test::Operation::absolute, double_q, 0U, 0U) == 0x4ee0'f800U);
	base::require(test::instruction(
		test::Operation::negate, single_d, 0U, 0U) == 0x2ea0'f800U);
	base::require(test::instruction(
		test::Operation::negate, single_q, 0U, 0U) == 0x6ea0'f800U);
	base::require(test::instruction(
		test::Operation::negate, double_q, 0U, 0U) == 0x6ee0'f800U);
}

void reserved_precision_and_arrangement_are_early_illegal()
{
	base::Harness harness;
	for (const auto operation : {test::Operation::absolute,
		     test::Operation::negate}) {
		const auto scalar_reserved =
			(operation == test::Operation::negate ? 0x1e21'4000U :
				0x1e20'c000U) | (2U << 22) | (29U << 5) | 28U;
		harness.require_illegal(scalar_reserved, base::state(31U));
		const test::Shape short_double{8U, false, false};
		harness.require_illegal(
			test::instruction(operation, short_double, 28U, 29U),
			base::state(32U));
		for (const auto shape : {test::Shape{4U, false, false},
		     test::Shape{4U, false, true}, test::Shape{8U, false, true}}) {
			auto unallocated =
				test::instruction(operation, shape, 28U, 29U);
			unallocated &= ~0x0080'0000U;
			harness.require_illegal(unallocated, base::state(33U));
		}
	}
}

} // namespace

int main()
{
	every_scalar_and_vector_encoding();
	canonical_vector_words_match_the_architecture();
	reserved_precision_and_arrangement_are_early_illegal();
	return 0;
}
