// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_multiply_test_support.hpp"

#include <array>
#include <bit>
#include <cstdio>

namespace test = arm64_bfloat_multiply_test;

namespace {

void exact_bottom_top_lane_selection()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	auto initial = test::shared::state(100U);
	for (unsigned lane = 0U; lane < 4U; ++lane)
		test::shared::write_single(initial.vectors[0].bytes, lane,
			std::bit_cast<std::uint32_t>(
				10.0F * static_cast<float>(lane + 1U)));
	constexpr std::array<float, 8> left{
		1.0F, -1.0F, 2.0F, -2.0F,
		3.0F, -3.0F, 4.0F, -4.0F,
	};
	constexpr std::array<float, 8> right{
		5.0F, 2.0F, 6.0F, 3.0F,
		7.0F, 4.0F, 8.0F, 5.0F,
	};
	for (unsigned lane = 0U; lane < 8U; ++lane) {
		test::shared::write_bfloat(initial.vectors[1].bytes, lane,
			test::shared::bfloat(left[lane]));
		test::shared::write_bfloat(initial.vectors[2].bytes, lane,
			test::shared::bfloat(right[lane]));
	}
	constexpr std::array bottom_values{
		15.0F, 32.0F, 51.0F, 72.0F,
	};
	constexpr std::array top_values{
		8.0F, 14.0F, 18.0F, 20.0F,
	};
	for (const auto form : {test::Form::vector_bottom,
	     test::Form::vector_top}) {
		const auto expected = test::expected_state(initial, form,
			0U, {0U, 1U, 2U});
		const auto &values = test::top(form) ?
			top_values : bottom_values;
		for (unsigned lane = 0U; lane < 4U; ++lane)
			test::shared::require(test::shared::read_single(
				expected.vectors[0].bytes, lane) ==
				std::bit_cast<std::uint32_t>(values[lane]),
				"BFMLAL vector lane oracle mismatch");
		test::check(harness, "BFMLAL vector exact lane selection",
			form, 0U, {0U, 1U, 2U}, initial, counts);
	}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-multiply-vector-exact");
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

void deterministic_finite_patterns()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	std::uint64_t random = 0xa409'3822'299f'31d0ULL;
	unsigned seed = 500U;
	for (const auto form : {test::Form::vector_bottom,
	     test::Form::vector_top})
		for (unsigned sample = 0U; sample < 256U; ++sample) {
			auto initial = test::shared::state(++seed);
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
				"BFMLAL vector deterministic top=%u sample=%u",
				static_cast<unsigned>(test::top(form)), sample);
			test::check(harness, name, form, 0U,
				{28U, 29U, 30U}, initial, counts);
		}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-multiply-vector-patterns");
}

} // namespace

int main()
{
	exact_bottom_top_lane_selection();
	deterministic_finite_patterns();
	return 0;
}
