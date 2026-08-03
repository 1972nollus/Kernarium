// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_fused_long_test_support.hpp"

namespace test = arm64_half_fused_long_test;

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts{};
	const auto native = test::native_requirements();
	const auto initial = test::state(900U);
	const std::array invalid{
		test::instruction(test::vector_forms[0]) | 0x0040'0000U,
		test::instruction(test::vector_forms[2]) | 0x0040'0000U,
		test::instruction(test::indexed_form(
			false, false, false, 0U)) | 0x0000'0400U,
		test::instruction(test::indexed_form(
			true, true, true, 7U)) | 0x0000'0400U,
	};
	for (const auto instruction : invalid) {
		harness.check_illegal("half-fused-long-reserved",
			instruction, initial, native);
		counts.add(native, true);
	}
	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-fused-long-reserved");
}
