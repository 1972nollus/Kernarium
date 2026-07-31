// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_matrix_test_support.hpp"

namespace test = arm64_half_matrix_test;

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts{};
	auto initial = test::state(300U);
	test::clear_vectors(initial);
	for (unsigned lane = 0U; lane < 4U; ++lane)
		test::write_single(initial, 0U, lane,
			std::bit_cast<std::uint32_t>(
				static_cast<float>(lane + 1U)));
	for (unsigned lane = 0U; lane < 8U; ++lane) {
		test::write_half(initial, 1U, lane,
			test::half(static_cast<float>(lane + 1U)));
		test::write_half(initial, 2U, lane,
			test::half(static_cast<float>(lane + 9U)));
	}
	test::check(harness, "half-matrix-widening-behavior",
		true, {0U, 1U, 2U}, initial, counts);

	unsigned seed = 400U;
	for (const auto registers : test::register_aliases)
		test::check(harness, "half-matrix-widening-alias",
			true, registers, test::state(++seed), counts);

	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-matrix-widening");
}
