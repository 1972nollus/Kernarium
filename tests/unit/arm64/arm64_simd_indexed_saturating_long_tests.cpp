// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_indexed_test_support.hpp"

namespace test = arm64_simd_indexed_test;

namespace {

void check_values(test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts, const test::Family &family,
	test::Shape shape, unsigned index, std::uint64_t accumulator,
	std::uint64_t left, std::uint64_t right, bool qc, unsigned seed,
	std::string_view name)
{
	const unsigned narrow_bytes = 1U << shape.size;
	const unsigned wide_bytes = narrow_bytes * 2U;
	auto initial = test::state(seed);
	initial.fpsr = qc ? initial.fpsr | (1U << 27) :
		initial.fpsr & ~(1U << 27);
	test::fill_lanes(initial, 28U, wide_bytes, accumulator);
	test::fill_lanes(initial, 29U, narrow_bytes, left);
	test::fill_lanes(initial, 15U, narrow_bytes, right);
	test::check_one(harness, name, family, shape,
		{28U, 29U, 15U, index}, initial, counts);
}

void two_stage_saturation_qc_and_every_index(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed = 110'000U;
	for (const auto family :
	     test::indexed_saturating_long_families)
		for (const auto shape : test::saturating_shapes) {
			const unsigned narrow_bits = (1U << shape.size) * 8U;
			const unsigned wide_bits = narrow_bits * 2U;
			const auto narrow_minimum =
				std::uint64_t{1} << (narrow_bits - 1U);
			const auto narrow_maximum = narrow_minimum - 1U;
			const auto wide_minimum =
				std::uint64_t{1} << (wide_bits - 1U);
			const auto wide_maximum = wide_minimum - 1U;
			const auto accumulator =
				family.operation == test::Operation::sqdmlal ?
					wide_minimum :
				family.operation == test::Operation::sqdmlsl ?
					wide_maximum : 0U;
			check_values(harness, counts, family, shape, 0U,
				accumulator, narrow_minimum, narrow_minimum,
				false, ++seed,
				"simd-indexed-saturating-long-product-stage");
			check_values(harness, counts, family, shape, 0U,
				test::subtracts(family.operation) ?
					wide_minimum : wide_maximum,
				narrow_maximum, 2U, false, ++seed,
				"simd-indexed-saturating-long-accumulator-stage");
			check_values(harness, counts, family, shape, 0U,
				3U, 4U, 5U, false, ++seed,
				"simd-indexed-saturating-long-exact");
			check_values(harness, counts, family, shape, 0U,
				3U, 4U, 5U, true, ++seed,
				"simd-indexed-saturating-long-cumulative-qc");

			const unsigned indices =
				test::lane_count(family, shape);
			for (unsigned index = 0; index < indices; ++index) {
				const unsigned narrow_bytes = 1U << shape.size;
				const unsigned wide_bytes = narrow_bytes * 2U;
				auto initial = test::state(++seed);
				test::fill_lanes(initial, 28U, wide_bytes, 1U);
				test::fill_lanes(initial, 29U, narrow_bytes, 2U);
				for (unsigned lane = 0; lane < indices; ++lane)
					test::set_lane(initial, 15U, narrow_bytes,
						lane, lane + 1U);
				test::check_one(harness,
					"simd-indexed-saturating-long-lane",
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
	two_stage_saturation_qc_and_every_index(harness, counts);
	test::require_counts(harness, counts);
	harness.print_summary("arm64-simd-indexed-saturating-long");
	return 0;
}
