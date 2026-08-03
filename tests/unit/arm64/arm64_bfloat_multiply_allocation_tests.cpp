// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_multiply_test_support.hpp"

namespace test = arm64_bfloat_multiply_test;

namespace {

[[nodiscard]] unsigned distinct_register(unsigned first,
	unsigned second = 32U, unsigned limit = 32U)
{
	for (unsigned reg = 0U; reg < limit; ++reg)
		if (reg != first && reg != second)
			return reg;
	std::abort();
}

[[nodiscard]] test::arm64::CpuState allocation_state(unsigned seed)
{
	auto initial = test::shared::state(seed);
	for (unsigned reg = 0U; reg < 32U; ++reg)
		for (unsigned lane = 0U; lane < 8U; ++lane)
			test::shared::write_bfloat(initial.vectors[reg].bytes,
				lane, static_cast<std::uint16_t>(
					0x3f00U + ((reg + lane + seed) & 0x3fU)));
	return initial;
}

void exact_encodings_match_the_specification_diagrams()
{
	test::shared::require(test::instruction(
		test::Form::vector_bottom, 0U, {0U, 1U, 2U}) ==
			0x2ec2'fc20U,
		"BFMLALB vector encoding mismatch");
	test::shared::require(test::instruction(
		test::Form::vector_top, 0U, {31U, 30U, 29U}) ==
			0x6edd'ffdfU,
		"BFMLALT vector encoding mismatch");
	test::shared::require(test::instruction(
		test::Form::indexed_bottom, 7U, {0U, 1U, 15U}) ==
			0x0fff'f820U,
		"BFMLALB indexed encoding mismatch");
	test::shared::require(test::instruction(
		test::Form::indexed_top, 3U, {31U, 30U, 14U}) ==
			0x4ffe'f3dfU,
		"BFMLALT indexed encoding mismatch");
}

void every_form_register_field_index_and_alias()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	unsigned seed = 1000U;
	for (const auto form : test::forms)
		for (unsigned index = 0U; index < test::index_count(form);
		     ++index) {
			const unsigned right_count =
				test::second_register_count(form);
			for (unsigned destination = 0U; destination < 32U;
			     ++destination) {
				const auto first =
					distinct_register(destination);
				const auto second = distinct_register(
					destination, first, right_count);
				test::check(harness,
					"BFMLAL destination field", form,
					index,
					{destination, first, second},
					allocation_state(++seed), counts);
			}
			for (unsigned first = 0U; first < 32U; ++first) {
				const auto destination =
					distinct_register(first);
				const auto second = distinct_register(
					destination, first, right_count);
				test::check(harness,
					"BFMLAL first source field", form,
					index,
					{destination, first, second},
					allocation_state(++seed), counts);
			}
			for (unsigned second = 0U; second < right_count;
			     ++second) {
				const auto destination =
					distinct_register(second);
				const auto first = distinct_register(
					destination, second);
				test::check(harness,
					"BFMLAL second source field", form,
					index,
					{destination, first, second},
					allocation_state(++seed), counts);
			}
			for (auto registers : test::alias_cases) {
				if (registers.second >= right_count)
					registers.second = right_count - 1U;
				test::check(harness,
					"BFMLAL source/destination alias",
					form, index, registers,
					allocation_state(++seed), counts);
			}
		}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-multiply-allocation");
}

void reserved_vector_sizes_are_illegal()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	const auto native = test::shared::native_requirements(0U, false);
	for (const bool q : {false, true})
		for (const unsigned size : {0U, 2U}) {
			const auto encoded =
				0x0e00'8400U |
				(static_cast<std::uint32_t>(q) << 30U) |
				(1U << 29U) | (size << 22U) |
				(15U << 11U) | (30U << 16U) |
				(29U << 5U) | 28U;
			harness.check_illegal(
				"BFMLAL/BFDOT reserved vector size",
				encoded, allocation_state(5000U + size + q),
				native);
			counts.add(native, true);
		}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-multiply-reserved");
}

} // namespace

int main()
{
	exact_encodings_match_the_specification_diagrams();
	every_form_register_field_index_and_alias();
	reserved_vector_sizes_are_illegal();
	return 0;
}
