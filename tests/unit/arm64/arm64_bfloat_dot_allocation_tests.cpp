// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_dot_test_support.hpp"

#include <cstdio>

namespace test = arm64_bfloat_dot_test;

namespace {

[[nodiscard]] unsigned distinct_register(unsigned first,
	unsigned second = 32U)
{
	for (unsigned reg = 0U; reg < 32U; ++reg)
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
		{false, false, 0U}, {0U, 1U, 2U}) == 0x2e42'fc20U,
		"BFDOT vector 2S encoding mismatch");
	test::shared::require(test::instruction(
		{false, true, 0U}, {31U, 30U, 29U}) ==
			0x6e5d'ffdfU,
		"BFDOT vector 4S encoding mismatch");
	test::shared::require(test::instruction(
		{true, false, 3U}, {0U, 1U, 18U}) ==
			0x0f72'f820U,
		"BFDOT indexed 2S encoding mismatch");
	test::shared::require(test::instruction(
		{true, true, 2U}, {31U, 30U, 17U}) ==
			0x4f51'fbdfU,
		"BFDOT indexed 4S encoding mismatch");
}

void every_form_register_field_index_and_alias()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	unsigned seed = 1000U;
	for (const auto form : test::forms) {
		for (unsigned destination = 0U; destination < 32U;
		     ++destination) {
			const auto first = distinct_register(destination);
			const auto second =
				distinct_register(destination, first);
			test::check(harness, "BFDOT destination field", form,
				{destination, first, second},
				allocation_state(++seed), counts);
		}
		for (unsigned first = 0U; first < 32U; ++first) {
			const auto destination = distinct_register(first);
			const auto second =
				distinct_register(destination, first);
			test::check(harness, "BFDOT first source field", form,
				{destination, first, second},
				allocation_state(++seed), counts);
		}
		for (unsigned second = 0U; second < 32U; ++second) {
			const auto destination = distinct_register(second);
			const auto first =
				distinct_register(destination, second);
			test::check(harness, "BFDOT second source field", form,
				{destination, first, second},
				allocation_state(++seed), counts);
		}
		for (const auto registers : test::alias_cases)
			test::check(harness, "BFDOT source/destination alias",
				form, registers, allocation_state(++seed), counts);
	}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-dot-allocation");
}

void reserved_vector_size_partition_is_illegal()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	const auto native = test::shared::native_requirements(0U, false);
	for (const bool q : {false, true})
		for (const unsigned size : {0U, 2U}) {
			const auto encoded = test::raw_vector_instruction(
				q, true, size, 15U, {28U, 29U, 30U});
			harness.check_illegal("BFDOT/BFMLAL reserved vector size",
				encoded, allocation_state(4000U + size + q),
				native);
			counts.add(native, true);
		}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-vector-reserved-partition");
}

} // namespace

int main()
{
	exact_encodings_match_the_specification_diagrams();
	every_form_register_field_index_and_alias();
	reserved_vector_size_partition_is_illegal();
	return 0;
}
