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
	const unsigned lane_bytes = 1U << shape.size;
	auto initial = test::state(seed);
	initial.fpsr = qc ? initial.fpsr | (1U << 27) :
		initial.fpsr & ~(1U << 27);
	test::fill_lanes(initial, 28U, lane_bytes, accumulator);
	test::fill_lanes(initial, 29U, lane_bytes, left);
	test::fill_lanes(initial, 30U, lane_bytes, right);
	test::check_one(harness, name, family, shape,
		{28U, 29U, 30U}, initial, counts);
}

void full_expression_rounding_saturation_and_lane_pairing(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed = 190'000U;
	for (const auto family : test::vector_rdm_families)
		for (const auto shape : test::saturating_shapes) {
			const unsigned lane_bytes = 1U << shape.size;
			const unsigned bits = lane_bytes * 8U;
			const auto minimum =
				std::uint64_t{1} << (bits - 1U);
			const auto maximum = minimum - 1U;
			const auto half = std::uint64_t{1} << (bits - 2U);
			check_values(harness, counts, family, shape,
				test::subtracts(family.operation) ?
					minimum : maximum,
				minimum, minimum, false, ++seed,
				"simd-vector-rdm-saturation");
			check_values(harness, counts, family, shape,
				0U, 1U, half, false, ++seed,
				"simd-vector-rdm-positive-round");
			check_values(harness, counts, family, shape,
				0U, test::mask(bits), half, false, ++seed,
				"simd-vector-rdm-negative-round");
			check_values(harness, counts, family, shape,
				3U, 4U, 5U, false, ++seed,
				"simd-vector-rdm-exact");
			check_values(harness, counts, family, shape,
				3U, 4U, 5U, true, ++seed,
				"simd-vector-rdm-cumulative-qc");

			auto initial = test::state(++seed);
			test::fill_lanes(initial, 28U, lane_bytes, 1U);
			for (unsigned lane = 0; lane < 16U / lane_bytes;
			     ++lane) {
				test::set_lane(initial, 29U, lane_bytes,
					lane, lane * 3U + 1U);
				test::set_lane(initial, 30U, lane_bytes,
					lane, lane * 5U + 2U);
			}
			test::check_one(harness,
				"simd-vector-rdm-lanes",
				family, shape, {28U, 29U, 30U},
				initial, counts);
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	full_expression_rounding_saturation_and_lane_pairing(harness, counts);
	test::require_counts(harness, counts);
	harness.print_summary("arm64-simd-indexed-vector-rdm");
	return 0;
}
