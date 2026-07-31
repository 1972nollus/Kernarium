// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_fused_long_test_support.hpp"

namespace test = arm64_half_fused_long_test;

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts{};
	for (const auto form : test::vector_forms) {
		auto initial = test::state(100U + test::instruction(form));
		for (unsigned lane = 0U; lane < 4U; ++lane)
			test::write_single(initial, 0U, lane,
				std::bit_cast<std::uint32_t>(
					10.0F * static_cast<float>(lane + 1U)));
		for (unsigned lane = 0U; lane < 8U; ++lane) {
			test::write_half(initial, 1U, lane,
				test::half(static_cast<float>(lane + 1U)));
			test::write_half(initial, 2U, lane,
				test::half(static_cast<float>(lane + 2U)));
		}
		test::check(harness, "half-fused-long-vector-behavior",
			form, {0U, 1U, 2U}, initial, counts);
	}

	for (const auto form : test::vector_forms) {
		auto alias = test::state(200U + test::instruction(form));
		test::check(harness, "half-fused-long-vector-all-alias",
			form, {3U, 3U, 3U}, alias, counts);
		test::check(harness, "half-fused-long-vector-destination-first",
			form, {4U, 4U, 5U}, alias, counts);
		test::check(harness, "half-fused-long-vector-destination-second",
			form, {5U, 4U, 5U}, alias, counts);
	}

	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-fused-long-vector");
}
