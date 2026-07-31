// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fmulx_test_support.hpp"

namespace test = arm64_simd_fmulx_test;
namespace fp = arm64_simd_fp_indexed_test;
namespace base = arm64_simd_test;

namespace {

void every_scalar_and_vector_precision()
{
	test::engine_tests::EngineHarness harness;
	for (const auto shape : fp::shapes) {
		const auto one = fp::floating_bits(shape.lane_bytes, 1.0);
		for (unsigned destination = 0; destination < 32U; ++destination)
			for (unsigned left = 0; left < 32U; ++left)
				for (unsigned right = 0; right < 32U; ++right) {
					auto initial = base::state(shape.size + shape.scalar +
						shape.q + destination * 3U + left * 5U +
						right * 7U);
					initial.fpsr = 0x80U;
					fp::fill_bits(initial.vectors[left].bytes,
						shape.lane_bytes, one);
					fp::fill_bits(initial.vectors[right].bytes,
						shape.lane_bytes, one);
					test::check_uniform(harness,
						"fmulx-valid-allocation", shape,
						destination, left, right, initial, one,
						0x80U);
				}
	}
	harness.print_summary("arm64-simd-fmulx-allocation-valid");
}

void reserved_short_double_vector_is_early_illegal()
{
	test::engine_tests::EngineHarness harness;
	const fp::Shape reserved{3U, 8U, 0U, false, false};
	for (unsigned destination = 0; destination < 32U; ++destination)
		for (unsigned left = 0; left < 32U; ++left)
			for (unsigned right = 0; right < 32U; ++right)
				harness.check_illegal("fmulx-reserved-short-double",
					test::instruction(reserved, destination, left,
						right),
					base::state(71U + destination + left + right));
	harness.print_summary("arm64-simd-fmulx-allocation-reserved");
}

} // namespace

int main()
{
	every_scalar_and_vector_precision();
	reserved_short_double_vector_is_early_illegal();
	return 0;
}
