// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_dot_test_support.hpp"

namespace test = arm64_half_dot_test;

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
	auto initial = test::state(seed);
	for (unsigned reg = 0U; reg < 32U; ++reg) {
		for (unsigned lane = 0U; lane < 8U; ++lane)
			test::write_half(initial.vectors[reg].bytes, lane,
				test::half(static_cast<float>(
					1U + ((reg * 3U + lane + seed) % 11U))));
		for (unsigned lane = 0U; lane < 4U; ++lane)
			test::exact::write_single(initial.vectors[reg].bytes,
				lane, std::bit_cast<std::uint32_t>(
					static_cast<float>(reg + lane + 1U)));
	}
	return initial;
}

void exact_encodings_match_the_specification_diagrams()
{
	test::require(test::instruction(
		{false, false, 0U}, {0U, 1U, 2U}) == 0x0e82'fc20U,
		"FDOT vector 2S encoding mismatch");
	test::require(test::instruction(
		{false, true, 0U}, {31U, 30U, 29U}) == 0x4e9d'ffdfU,
		"FDOT vector 4S encoding mismatch");
	test::require(test::instruction(
		{true, false, 3U}, {0U, 1U, 18U}) == 0x0f72'9820U,
		"FDOT indexed 2S encoding mismatch");
	test::require(test::instruction(
		{true, true, 2U}, {31U, 30U, 17U}) == 0x4f51'9bdfU,
		"FDOT indexed 4S encoding mismatch");
}

void every_form_register_field_and_alias()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	unsigned seed = 1000U;
	for (const auto form : test::forms) {
		for (unsigned destination = 0U; destination < 32U;
		     ++destination) {
			const auto first = distinct_register(destination);
			const auto second =
				distinct_register(destination, first);
			test::check(harness, "FDOT destination register field",
				form, {destination, first, second},
				allocation_state(++seed), counts);
		}
		for (unsigned first = 0U; first < 32U; ++first) {
			const auto destination = distinct_register(first);
			const auto second =
				distinct_register(destination, first);
			test::check(harness, "FDOT first source register field",
				form, {destination, first, second},
				allocation_state(++seed), counts);
		}
		for (unsigned second = 0U; second < 32U; ++second) {
			const auto destination = distinct_register(second);
			const auto first =
				distinct_register(destination, second);
			test::check(harness, "FDOT second source register field",
				form, {destination, first, second},
				allocation_state(++seed), counts);
		}
		for (const auto registers : test::register_aliases)
			test::check(harness, "FDOT destructive register alias",
				form, registers, allocation_state(++seed), counts);
	}
	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-dot-allocation");
}

} // namespace

int main()
{
	exact_encodings_match_the_specification_diagrams();
	every_form_register_field_and_alias();
	return 0;
}
