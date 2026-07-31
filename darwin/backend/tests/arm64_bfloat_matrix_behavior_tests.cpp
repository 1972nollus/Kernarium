// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_matrix_test_support.hpp"

#include <array>
#include <bit>
#include <cstdio>

namespace test = arm64_bfloat_matrix_test;

namespace {

void exact_two_by_four_times_four_by_two_mapping()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	auto initial = test::shared::state(100U);
	for (unsigned lane = 0U; lane < 4U; ++lane) {
		test::shared::write_single(initial.vectors[0].bytes, lane,
			std::bit_cast<std::uint32_t>(
				static_cast<float>(lane + 1U)));
		test::shared::write_bfloat(initial.vectors[1].bytes, lane,
			test::shared::bfloat(
				static_cast<float>(lane + 1U)));
		test::shared::write_bfloat(initial.vectors[1].bytes,
			lane + 4U, test::shared::bfloat(
				static_cast<float>(lane + 5U)));
		test::shared::write_bfloat(initial.vectors[2].bytes, lane,
			test::shared::bfloat(
				static_cast<float>(lane + 9U)));
		test::shared::write_bfloat(initial.vectors[2].bytes,
			lane + 4U, test::shared::bfloat(
				static_cast<float>(lane + 13U)));
	}
	const auto expected =
		test::expected_state(initial, {0U, 1U, 2U});
	constexpr std::array values{111.0F, 152.0F, 281.0F, 386.0F};
	for (unsigned lane = 0U; lane < 4U; ++lane)
		test::shared::require(test::shared::read_single(
			expected.vectors[0].bytes, lane) ==
				std::bit_cast<std::uint32_t>(values[lane]),
			"BFMMLA matrix lane oracle mismatch");
	test::check(harness, "BFMMLA exact matrix mapping",
		{0U, 1U, 2U}, initial, counts);
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-matrix-exact");
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

void deterministic_matrix_patterns_in_both_modes()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	std::uint64_t random = 0x4528'21e6'38d0'1377ULL;
	for (unsigned sample = 0U; sample < 512U; ++sample) {
		auto initial = test::shared::state(500U + sample);
		if ((sample & 1U) != 0U)
			initial.fpcr = test::shared::fpcr_ebf;
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
			"BFMMLA deterministic mode=%u sample=%u",
			static_cast<unsigned>((sample & 1U) != 0U), sample);
		test::check(harness, name, {28U, 29U, 30U},
			initial, counts);
	}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-matrix-patterns");
}

} // namespace

int main()
{
	exact_two_by_four_times_four_by_two_mapping();
	deterministic_matrix_patterns_in_both_modes();
	return 0;
}
