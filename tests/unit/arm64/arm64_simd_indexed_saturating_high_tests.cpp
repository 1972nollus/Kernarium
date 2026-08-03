// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_indexed_test_support.hpp"

namespace test = arm64_simd_indexed_test;

namespace {

void check_values(test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts, const test::Family &family,
	test::Shape shape, unsigned index, std::uint64_t left,
	std::uint64_t right, bool qc, unsigned seed, std::string_view name)
{
	const unsigned lane_bytes = 1U << shape.size;
	auto initial = test::state(seed);
	initial.fpsr = qc ? initial.fpsr | (1U << 27) :
		initial.fpsr & ~(1U << 27);
	test::fill_lanes(initial, 29U, lane_bytes, left);
	test::fill_lanes(initial, 15U, lane_bytes, right);
	test::check_one(harness, name, family, shape,
		{28U, 29U, 15U, index}, initial, counts);
}

void saturation_rounding_scalar_vector_and_every_index(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed = 150'000U;
	for (const auto family :
	     test::indexed_saturating_high_families)
		for (const auto shape : test::saturating_shapes) {
			const unsigned bits = (1U << shape.size) * 8U;
			const auto minimum =
				std::uint64_t{1} << (bits - 1U);
			const auto maximum = minimum - 1U;
			const auto half = std::uint64_t{1} << (bits - 2U);
			check_values(harness, counts, family, shape, 0U,
				minimum, minimum, false, ++seed,
				"simd-indexed-multiply-high-saturation");
			check_values(harness, counts, family, shape, 0U,
				maximum, maximum, false, ++seed,
				"simd-indexed-multiply-high-maximum");
			check_values(harness, counts, family, shape, 0U,
				1U, half, false, ++seed,
				"simd-indexed-multiply-high-positive-round");
			check_values(harness, counts, family, shape, 0U,
				test::mask(bits), half, false, ++seed,
				"simd-indexed-multiply-high-negative-round");
			check_values(harness, counts, family, shape, 0U,
				3U, 5U, true, ++seed,
				"simd-indexed-multiply-high-cumulative-qc");

			const unsigned indices =
				test::lane_count(family, shape);
			for (unsigned index = 0; index < indices; ++index) {
				const unsigned lane_bytes = 1U << shape.size;
				auto initial = test::state(++seed);
				test::fill_lanes(initial, 29U, lane_bytes, 7U);
				for (unsigned lane = 0; lane < indices; ++lane)
					test::set_lane(initial, 15U, lane_bytes,
						lane, lane * 11U + 1U);
				test::check_one(harness,
					"simd-indexed-multiply-high-lane",
					family, shape,
					{28U, 29U, 15U, index},
					initial, counts);
			}
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	saturation_rounding_scalar_vector_and_every_index(harness, counts);
	test::require_counts(harness, counts);
	harness.print_summary("arm64-simd-indexed-saturating-high");
	return 0;
}
