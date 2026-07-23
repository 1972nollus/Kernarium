// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fp_indexed_test_support.hpp"
#include "cpu/arm64/detail/half_dot.hpp"

namespace test = arm64_simd_fp_indexed_test;
namespace base = arm64_simd_test;

namespace {

void every_allocated_shape_and_index()
{
	base::Harness harness;
	for (const auto operation : {test::Operation::fmla, test::Operation::fmls,
	     test::Operation::fmul, test::Operation::fmulx})
		for (const auto shape : test::shapes)
			for (unsigned index = 0; index < shape.indexes; ++index) {
				auto initial = base::state(index + shape.size * 13U +
					static_cast<unsigned>(operation));
				const auto one = test::floating_bits(shape.lane_bytes, 1.0);
				test::fill_bits(initial.vectors[28].bytes,
					shape.lane_bytes, one);
				test::fill_bits(initial.vectors[29].bytes,
					shape.lane_bytes, one);
				test::fill_bits(initial.vectors[shape.size == 0U ? 13U : 19U].bytes,
					shape.lane_bytes, one);
				harness.require_differential(test::instruction(operation, shape,
					28U, 29U, shape.size == 0U ? 13U : 19U, index), initial);
			}
}

void reserved_sizes_and_indices_are_early_illegal()
{
	base::Harness harness;
	for (const auto operation : {test::Operation::fmla, test::Operation::fmls,
	     test::Operation::fmul, test::Operation::fmulx}) {
		for (const bool scalar : {false, true})
			for (const bool q : {false, true}) {
				const test::Shape reserved{1U, 2U, 0U, scalar, q};
				const auto encoded = test::instruction(operation, reserved,
					28U, 29U, 13U, 0U);
				/* FEAT_F16F32DOT allocated the vector size=01 FMUL
				 * slot that older revisions reserved. */
				if (base::arm64::detail::decode_half_dot(encoded).recognized)
					harness.require_differential(encoded, base::state(41U));
				else
					harness.require_illegal(encoded, base::state(41U));
			}
		const test::Shape double_short{3U, 8U, 2U, false, false};
		harness.require_illegal(test::instruction(operation, double_short,
			28U, 29U, 19U, 0U), base::state(42U));
		const test::Shape double_wide{3U, 8U, 2U, false, true};
		harness.require_illegal(test::instruction(operation, double_wide,
			28U, 29U, 19U, 0U) | 0x0020'0000U, base::state(43U));
	}
}

} // namespace

int main()
{
	every_allocated_shape_and_index();
	reserved_sizes_and_indices_are_early_illegal();
	return 0;
}
