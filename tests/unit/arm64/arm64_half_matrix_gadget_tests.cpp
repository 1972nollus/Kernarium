// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_matrix_test_support.hpp"

namespace test = arm64_half_matrix_test;

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts{};
	unsigned seed = 700U;
	for (const bool widening : {false, true}) {
		for (const auto registers : test::register_aliases) {
			auto ordinary = test::state(++seed);
			ordinary.fpcr = (seed & 3U) << 22U;
			test::check(harness, "half-matrix-native-gadget",
				widening, registers, ordinary, counts);
			auto controlled = test::state(++seed);
			controlled.fpcr = test::shared::fpcr_ah |
				test::shared::fpcr_fiz |
				test::shared::fpcr_fz16 |
				test::shared::fpcr_fz |
				((seed & 3U) << 22U);
			test::check(harness, "half-matrix-portable-gadget",
				widening, registers, controlled, counts);
		}
	}
	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-matrix-gadget");
}
