// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_indexed_test_support.hpp"

#include <array>

namespace test = arm64_simd_indexed_test;

namespace {

struct Values final {
	std::uint64_t accumulator;
	std::uint64_t left;
	std::uint64_t right;
};

void every_size_half_signedness_and_wrapping_boundary(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed = 90'000U;
	for (const auto family : test::vector_long_families)
		for (const auto shape : test::vector_long_shapes) {
			const unsigned narrow_bytes = 1U << shape.size;
			const unsigned wide_bytes = narrow_bytes * 2U;
			const unsigned narrow_bits = narrow_bytes * 8U;
			const unsigned wide_bits = wide_bytes * 8U;
			const auto narrow_maximum = test::mask(narrow_bits);
			const auto wide_maximum = test::mask(wide_bits);
			const auto narrow_minimum =
				std::uint64_t{1} << (narrow_bits - 1U);
			const std::array values{
				Values{0U, 0U, narrow_maximum},
				Values{wide_maximum, narrow_maximum,
					narrow_maximum},
				Values{1U, narrow_minimum, 2U},
				Values{wide_maximum >> 1U, 3U,
					narrow_maximum},
			};
			for (const auto value : values) {
				auto initial = test::state(++seed);
				test::fill_lanes(initial, 28U, wide_bytes,
					value.accumulator);
				test::fill_lanes(initial, 29U, narrow_bytes,
					value.left);
				test::fill_lanes(initial, 30U, narrow_bytes,
					value.right);
				test::check_one(harness,
					"simd-vector-long-boundary",
					family, shape, {28U, 29U, 30U},
					initial, counts);
			}

			auto initial = test::state(++seed);
			test::fill_lanes(initial, 28U, wide_bytes, 7U);
			for (unsigned lane = 0; lane < 16U / narrow_bytes;
			     ++lane) {
				test::set_lane(initial, 29U, narrow_bytes,
					lane, lane * 5U + 1U);
				test::set_lane(initial, 30U, narrow_bytes,
					lane, lane * 7U + 3U);
			}
			test::check_one(harness,
				"simd-vector-long-lane-half",
				family, shape, {28U, 29U, 30U},
				initial, counts);
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	every_size_half_signedness_and_wrapping_boundary(harness, counts);
	test::require_counts(harness, counts);
	harness.print_summary("arm64-simd-indexed-vector-long");
	return 0;
}
