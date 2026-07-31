// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_fused_long_test_support.hpp"

namespace test = arm64_half_fused_long_test;

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts{};
	unsigned seed = 700U;
	for (const auto form : test::vector_forms) {
		auto ordinary = test::state(++seed);
		ordinary.fpcr = (seed & 3U) << 22U;
		test::check(harness, "half-fused-long-gadget-vector",
			form, {31U, 30U, 29U}, ordinary, counts);
		auto controlled = test::state(++seed);
		controlled.fpcr = test::shared::fpcr_ah |
			test::shared::fpcr_fiz | test::shared::fpcr_fz16 |
			test::shared::fpcr_fz | ((seed & 3U) << 22U);
		test::check(harness, "half-fused-long-portable-vector",
			form, {30U, 31U, 30U}, controlled, counts);
	}
	for (const bool subtract : {false, true})
		for (const bool upper : {false, true})
			for (const bool wide : {false, true})
				for (unsigned index = 0U; index < 8U; ++index) {
					auto initial = test::state(++seed);
					initial.fpcr = (seed & 3U) << 22U;
					test::check(harness,
						"half-fused-long-gadget-indexed",
						test::indexed_form(subtract, upper,
							wide, index),
						{15U, 14U, 13U}, initial,
						counts);
				}

	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-fused-long-gadget");
}
