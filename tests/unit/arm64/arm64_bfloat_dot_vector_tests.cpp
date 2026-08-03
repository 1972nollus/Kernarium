// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_dot_test_support.hpp"

#include <array>
#include <bit>
#include <cstdio>

namespace test = arm64_bfloat_dot_test;

namespace {

void exact_lane_mapping_and_inactive_lane_clearing()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	auto initial = test::shared::state(100U);
	constexpr std::array<float, 8> left{
		1.0F, 2.0F, 3.0F, 4.0F,
		5.0F, 6.0F, 7.0F, 8.0F,
	};
	constexpr std::array<float, 8> right{
		5.0F, 6.0F, 7.0F, 8.0F,
		9.0F, 10.0F, 11.0F, 12.0F,
	};
	for (unsigned lane = 0U; lane < 8U; ++lane) {
		test::shared::write_bfloat(initial.vectors[1].bytes, lane,
			test::shared::bfloat(left[lane]));
		test::shared::write_bfloat(initial.vectors[2].bytes, lane,
			test::shared::bfloat(right[lane]));
	}
	for (unsigned lane = 0U; lane < 4U; ++lane)
		test::shared::write_single(initial.vectors[0].bytes, lane,
			std::bit_cast<std::uint32_t>(
				10.0F * static_cast<float>(lane + 1U)));
	constexpr std::array expected{
		27.0F, 73.0F, 135.0F, 213.0F,
	};
	for (const bool wide : {false, true}) {
		const auto form = test::Form{false, wide, 0U};
		const auto oracle = test::expected_state(initial, form,
			{0U, 1U, 2U});
		for (unsigned lane = 0U; lane < (wide ? 4U : 2U); ++lane)
			test::shared::require(test::shared::read_single(
				oracle.vectors[0].bytes, lane) ==
				std::bit_cast<std::uint32_t>(expected[lane]),
				"BFDOT vector lane oracle mismatch");
		for (unsigned lane = wide ? 4U : 2U; lane < 4U; ++lane)
			test::shared::require(test::shared::read_single(
				oracle.vectors[0].bytes, lane) == 0U,
				"BFDOT 2S did not clear inactive lanes");
		test::check(harness, "BFDOT vector exact mapping", form,
			{0U, 1U, 2U}, initial, counts);
	}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-dot-vector-exact");
}

[[nodiscard]] std::uint64_t next_random(std::uint64_t &random)
{
	random ^= random << 13U;
	random ^= random >> 7U;
	random ^= random << 17U;
	return random;
}

[[nodiscard]] std::uint16_t finite_bfloat(std::uint64_t value)
{
	auto bits = static_cast<std::uint16_t>(value);
	if ((bits & 0x7f80U) == 0x7f80U)
		bits ^= 0x0080U;
	return bits;
}

[[nodiscard]] std::uint32_t finite_single(std::uint64_t value)
{
	auto bits = static_cast<std::uint32_t>(value);
	if ((bits & 0x7f80'0000U) == 0x7f80'0000U)
		bits ^= 0x0080'0000U;
	return bits;
}

void deterministic_finite_and_denormal_patterns()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	std::uint64_t random = 0x243f'6a88'85a3'08d3ULL;
	unsigned seed = 500U;
	for (const bool wide : {false, true})
		for (unsigned sample = 0U; sample < 256U; ++sample) {
			auto initial = test::shared::state(++seed);
			initial.fpsr = 0x0800'009fU;
			for (unsigned lane = 0U; lane < 8U; ++lane) {
				test::shared::write_bfloat(
					initial.vectors[29].bytes, lane,
					finite_bfloat(next_random(random)));
				test::shared::write_bfloat(
					initial.vectors[30].bytes, lane,
					finite_bfloat(next_random(random)));
			}
			for (unsigned lane = 0U; lane < 4U; ++lane)
				test::shared::write_single(
					initial.vectors[28].bytes, lane,
					finite_single(next_random(random)));
			char name[128];
			std::snprintf(name, sizeof(name),
				"BFDOT vector deterministic wide=%u sample=%u",
				static_cast<unsigned>(wide), sample);
			test::check(harness, name, {false, wide, 0U},
				{28U, 29U, 30U}, initial, counts);
		}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-dot-vector-patterns");
}

} // namespace

int main()
{
	exact_lane_mapping_and_inactive_lane_clearing();
	deterministic_finite_and_denormal_patterns();
	return 0;
}
