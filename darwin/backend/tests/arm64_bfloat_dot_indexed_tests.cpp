// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_dot_test_support.hpp"

#include <bit>
#include <cstdio>

namespace test = arm64_bfloat_dot_test;

namespace {

void every_index_width_and_lane_mapping()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	for (const bool wide : {false, true})
		for (unsigned index = 0U; index < 4U; ++index) {
			auto initial = test::shared::state(
				200U + index + 10U * wide);
			for (unsigned lane = 0U; lane < 4U; ++lane) {
				test::shared::write_single(
					initial.vectors[0].bytes, lane, 0U);
				test::shared::write_bfloat(
					initial.vectors[1].bytes, lane * 2U,
					test::shared::bfloat(
						static_cast<float>(lane + 1U)));
				test::shared::write_bfloat(
					initial.vectors[1].bytes,
					lane * 2U + 1U,
					test::shared::bfloat(
						static_cast<float>(lane + 2U)));
			}
			for (unsigned pair = 0U; pair < 4U; ++pair) {
				test::shared::write_bfloat(
					initial.vectors[2].bytes, pair * 2U,
					test::shared::bfloat(
						static_cast<float>(pair + 1U)));
				test::shared::write_bfloat(
					initial.vectors[2].bytes,
					pair * 2U + 1U,
					test::shared::bfloat(
						static_cast<float>(pair + 3U)));
			}
			const auto form = test::Form{true, wide, index};
			const auto expected = test::expected_state(initial,
				form, {0U, 1U, 2U});
			for (unsigned lane = 0U;
			     lane < (wide ? 4U : 2U); ++lane) {
				const float value =
					static_cast<float>((lane + 1U) *
						(index + 1U) +
						(lane + 2U) *
						(index + 3U));
				test::shared::require(
					test::shared::read_single(
						expected.vectors[0].bytes,
						lane) ==
						std::bit_cast<std::uint32_t>(
							value),
					"BFDOT indexed lane oracle mismatch");
			}
			test::check(harness, "BFDOT indexed exact mapping",
				form, {0U, 1U, 2U}, initial, counts);
		}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-dot-indexed-exact");
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
		bits ^= 0x0100U;
	return bits;
}

[[nodiscard]] std::uint32_t finite_single(std::uint64_t value)
{
	auto bits = static_cast<std::uint32_t>(value);
	if ((bits & 0x7f80'0000U) == 0x7f80'0000U)
		bits ^= 0x0100'0000U;
	return bits;
}

void deterministic_patterns_for_every_index()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	std::uint64_t random = 0x1319'8a2e'0370'7344ULL;
	unsigned seed = 1000U;
	for (const bool wide : {false, true})
		for (unsigned index = 0U; index < 4U; ++index)
			for (unsigned sample = 0U; sample < 64U; ++sample) {
				auto initial = test::shared::state(++seed);
				initial.fpsr = 0x0800'009fU;
				for (unsigned lane = 0U; lane < 8U;
				     ++lane) {
					test::shared::write_bfloat(
						initial.vectors[29].bytes,
						lane,
						finite_bfloat(
							next_random(random)));
					test::shared::write_bfloat(
						initial.vectors[30].bytes,
						lane,
						finite_bfloat(
							next_random(random)));
				}
				for (unsigned lane = 0U; lane < 4U;
				     ++lane)
					test::shared::write_single(
						initial.vectors[28].bytes,
						lane,
						finite_single(
							next_random(random)));
				char name[144];
				std::snprintf(name, sizeof(name),
					"BFDOT indexed deterministic "
					"wide=%u index=%u sample=%u",
					static_cast<unsigned>(wide),
					index, sample);
				test::check(harness, name,
					{true, wide, index},
					{28U, 29U, 30U}, initial,
					counts);
			}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-dot-indexed-patterns");
}

} // namespace

int main()
{
	every_index_width_and_lane_mapping();
	deterministic_patterns_for_every_index();
	return 0;
}
