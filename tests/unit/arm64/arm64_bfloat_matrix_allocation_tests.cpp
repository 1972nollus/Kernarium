// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_matrix_test_support.hpp"

namespace test = arm64_bfloat_matrix_test;

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

void exact_encoding_matches_the_specification_diagram()
{
	test::shared::require(test::instruction({0U, 1U, 2U}) ==
		0x6e42'ec20U, "BFMMLA encoding mismatch");
	test::shared::require(test::instruction({31U, 30U, 29U}) ==
		0x6e5d'efdfU, "BFMMLA edge-register encoding mismatch");
}

void every_register_field_and_alias()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	unsigned seed = 1000U;
	for (unsigned destination = 0U; destination < 32U;
	     ++destination) {
		const auto first = distinct_register(destination);
		const auto second = distinct_register(destination, first);
		test::check(harness, "BFMMLA destination field",
			{destination, first, second}, allocation_state(++seed),
			counts);
	}
	for (unsigned first = 0U; first < 32U; ++first) {
		const auto destination = distinct_register(first);
		const auto second = distinct_register(destination, first);
		test::check(harness, "BFMMLA first source field",
			{destination, first, second}, allocation_state(++seed),
			counts);
	}
	for (unsigned second = 0U; second < 32U; ++second) {
		const auto destination = distinct_register(second);
		const auto first = distinct_register(destination, second);
		test::check(harness, "BFMMLA second source field",
			{destination, first, second}, allocation_state(++seed),
			counts);
	}
	for (const auto registers : test::alias_cases)
		test::check(harness, "BFMMLA source/destination alias",
			registers, allocation_state(++seed), counts);
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-matrix-allocation");
}

void reserved_matrix_partition_is_illegal()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	const auto native = test::shared::native_requirements(0U, false);
	const std::array encodings{
		test::dot::raw_vector_instruction(false, true, 1U, 13U,
			{28U, 29U, 30U}),
		test::dot::raw_vector_instruction(true, true, 3U, 13U,
			{28U, 29U, 30U}),
	};
	for (unsigned index = 0U; index < encodings.size(); ++index) {
		harness.check_illegal("BFMMLA reserved Q/size cell",
			encodings[index], allocation_state(3000U + index),
			native);
		counts.add(native, true);
	}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-matrix-reserved");
}

} // namespace

int main()
{
	exact_encoding_matches_the_specification_diagram();
	every_register_field_and_alias();
	reserved_matrix_partition_is_illegal();
	return 0;
}
