// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_indexed_test_support.hpp"

namespace test = arm64_simd_indexed_test;

namespace {

void every_reserved_size_and_scalar_hole(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	constexpr test::Registers registers{28U, 29U, 15U, 3U};
	unsigned seed = 30'000U;
	for (const auto family : test::all_families) {
		const bool normal = test::is_same_width(family.operation) ||
			test::is_normal_long(family.operation);
		const unsigned first_valid_size =
			family.encoding == test::Encoding::vector_long &&
			test::is_normal_long(family.operation) ? 0U : 1U;
		for (const bool scalar : {false, true})
			for (const bool q : {false, true}) {
				if (scalar && q)
					continue;
				test::check_illegal(harness,
					"simd-multiply-reserved-size",
					family, {3U, scalar, q}, registers,
					test::state(++seed), counts);
				if (!normal)
					test::check_illegal(harness,
						"simd-multiply-reserved-size",
						family, {0U, scalar, q}, registers,
						test::state(++seed), counts);
			}
		if (!normal)
			continue;
		for (unsigned size = first_valid_size;
		     size <= 2U; ++size)
			test::check_illegal(harness,
				"simd-multiply-reserved-scalar",
				family, {size, true, false}, registers,
				test::state(++seed), counts);
		if (family.encoding == test::Encoding::indexed) {
			for (const bool q : {false, true})
				test::check_illegal(harness,
					"simd-multiply-reserved-indexed-size",
					family, {0U, false, q}, registers,
					test::state(++seed), counts);
			test::check_illegal(harness,
				"simd-multiply-reserved-indexed-size",
				family, {0U, true, false}, registers,
				test::state(++seed), counts);
		}
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	every_reserved_size_and_scalar_hole(harness, counts);
	test::require_counts(harness, counts);
	harness.print_summary("arm64-simd-indexed-reserved");
	return 0;
}
