// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fjcvt_test_support.hpp"

namespace test = arm64_simd_fjcvt_test;
namespace base = arm64_simd_test;

int main()
{
	base::Harness harness;
	auto initial = base::state(1U);
	initial.nzcv = 0xf000'0000U;
	initial.fpsr = 1U << 3;
	test::set_source(initial, 0x3fe0'0000'0000'0000ULL);
	test::require_result(28U, 0U, 0U, 1U << 4, initial, harness);
	return 0;
}
