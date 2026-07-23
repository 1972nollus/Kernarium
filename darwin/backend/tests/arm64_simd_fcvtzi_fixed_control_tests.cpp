// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void nan_and_reserved_scales()
{
	base::Harness harness;
	for (const bool unsigned_result : {false, true}) {
		auto initial = base::state(unsigned_result);
		test::set_gpr_source(initial, 29U, 32U,
			test::precision::quiet_nan(32U));
		test::require_gpr(test::gpr_fixed_instruction(unsigned_result,
			32U, 64U, 8U, 28U, 29U), 64U, 28U, 0U, 1U,
			initial, harness);

		harness.require_illegal(test::gpr_fixed_instruction(unsigned_result,
			32U, 32U, 33U, 28U, 29U), base::state(90U));
		harness.require_illegal(test::gpr_fixed_instruction(unsigned_result,
			32U, 32U, 1U, 28U, 29U) | 0x0080'0000U,
			base::state(91U));

		const test::Shape short_double{64U, 8U, false, false};
		harness.require_illegal(test::simd_fixed_instruction(unsigned_result,
			short_double, 1U, 28U, 29U), base::state(92U));
		const auto invalid_immediate = (test::simd_fixed_instruction(
			unsigned_result, test::shapes[3], 16U, 28U, 29U) &
			~0x007f'0000U) | (15U << 16);
		harness.require_illegal(invalid_immediate, base::state(93U));
	}
}

} // namespace

int main()
{
	nan_and_reserved_scales();
	return 0;
}
