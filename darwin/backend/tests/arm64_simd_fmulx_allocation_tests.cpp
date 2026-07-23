// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fmulx_test_support.hpp"

namespace test = arm64_simd_fmulx_test;
namespace fp = arm64_simd_fp_indexed_test;
namespace base = arm64_simd_test;

namespace {

void every_scalar_and_vector_precision()
{
	base::Harness harness;
	for (const auto shape : fp::shapes) {
		auto initial = base::state(shape.size + shape.scalar + shape.q);
		initial.fpsr = 0x80U;
		const auto one = fp::floating_bits(shape.lane_bytes, 1.0);
		fp::fill_bits(initial.vectors[29].bytes, shape.lane_bytes, one);
		fp::fill_bits(initial.vectors[30].bytes, shape.lane_bytes, one);
		harness.require_differential(test::instruction(shape, 28U, 29U, 30U),
			initial);
	}
}

void reserved_short_double_vector_is_early_illegal()
{
	base::Harness harness;
	const fp::Shape reserved{3U, 8U, 0U, false, false};
	harness.require_illegal(test::instruction(reserved, 28U, 29U, 30U),
		base::state(71U));
}

} // namespace

int main()
{
	every_scalar_and_vector_precision();
	reserved_short_double_vector_is_early_illegal();
	return 0;
}
