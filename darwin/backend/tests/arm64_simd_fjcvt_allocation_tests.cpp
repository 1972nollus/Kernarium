// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fjcvt_test_support.hpp"

namespace test = arm64_simd_fjcvt_test;
namespace base = arm64_simd_test;

int main()
{
	base::Harness harness;
	for (const unsigned destination : {0U, 28U, 31U}) {
		auto initial = base::state(destination);
		test::set_source(initial, 0x3ff0'0000'0000'0000ULL);
		test::require_result(destination, 1U, 0x4000'0000U, 0U,
			initial, harness);
	}
	return 0;
}
