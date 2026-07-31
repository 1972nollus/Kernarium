// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_indexed_test_support.hpp"

namespace test = arm64_simd_indexed_test;

namespace {

void check_values(test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts, const test::Family &family,
	test::Shape shape, std::uint64_t accumulator,
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
	test::fill_lanes(initial, 30U, narrow_bytes, right);
	test::check_one(harness, name, family, shape,
		{28U, 29U, 30U}, initial, counts);
}

void two_stage_saturation_qc_and_source_halves(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed = 130'000U;
	for (const auto family :
	     test::vector_saturating_long_families)
		for (const auto shape : test::saturating_shapes) {
			const unsigned narrow_bytes = 1U << shape.size;
			const unsigned narrow_bits = narrow_bytes * 8U;
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
			check_values(harness, counts, family, shape,
				accumulator, narrow_minimum, narrow_minimum,
				false, ++seed,
				"simd-vector-saturating-long-product-stage");
			check_values(harness, counts, family, shape,
				test::subtracts(family.operation) ?
					wide_minimum : wide_maximum,
				narrow_maximum, 2U, false, ++seed,
				"simd-vector-saturating-long-accumulator-stage");
			check_values(harness, counts, family, shape,
				3U, 4U, 5U, false, ++seed,
				"simd-vector-saturating-long-exact");
			check_values(harness, counts, family, shape,
				3U, 4U, 5U, true, ++seed,
				"simd-vector-saturating-long-cumulative-qc");

			auto initial = test::state(++seed);
			test::fill_lanes(initial, 28U, narrow_bytes * 2U, 1U);
			for (unsigned lane = 0; lane < 16U / narrow_bytes;
			     ++lane) {
				test::set_lane(initial, 29U, narrow_bytes,
					lane, lane + 1U);
				test::set_lane(initial, 30U, narrow_bytes,
					lane, lane * 3U + 2U);
			}
			test::check_one(harness,
				"simd-vector-saturating-long-half",
				family, shape, {28U, 29U, 30U},
				initial, counts);
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	two_stage_saturation_qc_and_source_halves(harness, counts);
	test::require_counts(harness, counts);
	harness.print_summary("arm64-simd-indexed-vector-saturating-long");
	return 0;
}
