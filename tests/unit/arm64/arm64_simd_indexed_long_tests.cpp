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

void signed_unsigned_wrap_and_every_index(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed = 70'000U;
	for (const auto family : test::indexed_long_families)
		for (const auto shape : test::indexed_vector_shapes) {
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
				Values{wide_maximum >> 1U,
					narrow_maximum, narrow_maximum},
			};
			for (const auto value : values) {
				auto initial = test::state(++seed);
				test::fill_lanes(initial, 28U, wide_bytes,
					value.accumulator);
				test::fill_lanes(initial, 29U, narrow_bytes,
					value.left);
				test::fill_lanes(initial, 15U, narrow_bytes,
					value.right);
				test::check_one(harness,
					"simd-indexed-long-boundary",
					family, shape, {28U, 29U, 15U, 0U},
					initial, counts);
			}
			const unsigned indices =
				test::lane_count(family, shape);
			for (unsigned index = 0; index < indices; ++index) {
				auto initial = test::state(++seed);
				test::fill_lanes(initial, 28U, wide_bytes, 3U);
				test::fill_lanes(initial, 29U, narrow_bytes, 5U);
				for (unsigned lane = 0; lane < indices; ++lane)
					test::set_lane(initial, 15U, narrow_bytes,
						lane, lane * 17U + 1U);
				test::check_one(harness,
					"simd-indexed-long-lane",
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
	signed_unsigned_wrap_and_every_index(harness, counts);
	test::require_counts(harness, counts);
	harness.print_summary("arm64-simd-indexed-long");
	return 0;
}
