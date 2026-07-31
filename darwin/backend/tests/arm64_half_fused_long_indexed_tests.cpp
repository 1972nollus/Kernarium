// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_fused_long_test_support.hpp"

namespace test = arm64_half_fused_long_test;

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts{};
	unsigned seed = 300U;
	for (const bool subtract : {false, true})
		for (const bool upper : {false, true})
			for (const bool wide : {false, true})
				for (unsigned index = 0U; index < 8U; ++index) {
					auto initial = test::state(++seed);
					for (unsigned lane = 0U; lane < 4U;
					     ++lane)
						test::write_single(initial, 0U,
							lane, 0U);
					for (unsigned lane = 0U; lane < 8U;
					     ++lane) {
						test::write_half(initial, 1U,
							lane, test::half(
								static_cast<float>(
									lane + 1U)));
						test::write_half(initial, 2U,
							lane, test::half(
								static_cast<float>(
									lane + 2U)));
					}
					const auto form = test::indexed_form(
						subtract, upper, wide, index);
					test::check(harness,
						"half-fused-long-indexed-behavior",
						form, {0U, 1U, 2U}, initial,
						counts);
					test::check(harness,
						"half-fused-long-indexed-all-alias",
						form, {3U, 3U, 3U},
						test::state(++seed), counts);
				}

	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-fused-long-indexed");
}
