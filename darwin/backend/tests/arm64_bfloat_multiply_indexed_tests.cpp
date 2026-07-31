// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_multiply_test_support.hpp"

#include <bit>
#include <cstdio>

namespace test = arm64_bfloat_multiply_test;

namespace {

void every_index_and_source_half()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	for (const auto form : {test::Form::indexed_bottom,
	     test::Form::indexed_top})
		for (unsigned index = 0U; index < 8U; ++index) {
			auto initial = test::shared::state(
				200U + index +
				100U * static_cast<unsigned>(test::top(form)));
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
						-static_cast<float>(lane + 1U)));
			}
			for (unsigned lane = 0U; lane < 8U; ++lane)
				test::shared::write_bfloat(
					initial.vectors[2].bytes, lane,
					test::shared::bfloat(
						static_cast<float>(lane + 1U)));
			const auto expected = test::expected_state(initial,
				form, index, {0U, 1U, 2U});
			for (unsigned lane = 0U; lane < 4U; ++lane) {
				const float sign =
					test::top(form) ? -1.0F : 1.0F;
				const float value = sign *
					static_cast<float>((lane + 1U) *
						(index + 1U));
				test::shared::require(
					test::shared::read_single(
						expected.vectors[0].bytes,
						lane) ==
						std::bit_cast<std::uint32_t>(
							value),
					"BFMLAL indexed lane oracle mismatch");
			}
			test::check(harness,
				"BFMLAL indexed exact lane selection",
				form, index, {0U, 1U, 2U}, initial,
				counts);
		}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-multiply-indexed-exact");
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
	std::uint64_t random = 0x082e'fa98'ec4e'6c89ULL;
	unsigned seed = 1000U;
	for (const auto form : {test::Form::indexed_bottom,
	     test::Form::indexed_top})
		for (unsigned index = 0U; index < 8U; ++index)
			for (unsigned sample = 0U; sample < 64U; ++sample) {
				auto initial = test::shared::state(++seed);
				for (unsigned lane = 0U; lane < 8U;
				     ++lane) {
					test::shared::write_bfloat(
						initial.vectors[29].bytes,
						lane,
						finite_bfloat(
							next_random(random)));
					test::shared::write_bfloat(
						initial.vectors[15].bytes,
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
				char name[160];
				std::snprintf(name, sizeof(name),
					"BFMLAL indexed deterministic "
					"top=%u index=%u sample=%u",
					static_cast<unsigned>(
						test::top(form)),
					index, sample);
				test::check(harness, name, form, index,
					{28U, 29U, 15U}, initial,
					counts);
			}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-multiply-indexed-patterns");
}

} // namespace

int main()
{
	every_index_and_source_half();
	deterministic_patterns_for_every_index();
	return 0;
}
