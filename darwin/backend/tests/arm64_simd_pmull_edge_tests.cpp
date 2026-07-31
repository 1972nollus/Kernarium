// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_pmull_test_support.hpp"

namespace test = arm64_simd_pmull_test;

namespace {

void encoding_examples()
{
	test::require(test::instruction({
		.high = false,
		.size = 0,
		.registers = {0, 1, 2},
	}) == 0x0e22'e020U, "PMULL byte encoding diagram mismatch");
	test::require(test::instruction({
		.high = true,
		.size = 0,
		.registers = {0, 1, 2},
	}) == 0x4e22'e020U, "PMULL2 byte encoding diagram mismatch");
	test::require(test::instruction({
		.high = false,
		.size = 3,
		.registers = {0, 1, 2},
	}) == 0x0ee2'e020U,
		"PMULL doubleword encoding diagram mismatch");
	test::require(test::instruction({
		.high = true,
		.size = 3,
		.registers = {0, 1, 2},
	}) == 0x4ee2'e020U,
		"PMULL2 doubleword encoding diagram mismatch");
}

void directed_values_and_aliases()
{
	test::engine_tests::EngineHarness harness;
	std::size_t size_zero_cases = 0;
	std::size_t size_three_cases = 0;
	for (const unsigned size : {0U, 3U})
		for (const bool high : {false, true})
			for (const auto registers :
			     test::alias_register_cases)
				for (unsigned pattern = 0;
				     pattern < test::edge_values.size();
				     ++pattern) {
					test::check_one(
						harness,
						size == 0U ?
							"pmull-byte-edge" :
							"pmull-doubleword-edge",
						{
							.high = high,
							.size = size,
							.registers = registers,
						},
						test::edge_state(
							pattern +
							size * 19U +
							high * 41U));
					if (size == 0U)
						++size_zero_cases;
					else
						++size_three_cases;
				}
	constexpr std::size_t expected_per_size =
		2U * test::alias_register_cases.size() *
		test::edge_values.size();
	static_assert(expected_per_size == 320U);
	test::require(size_zero_cases == expected_per_size,
		"PMULL byte edge matrix is incomplete");
	test::require(size_three_cases == expected_per_size,
		"PMULL doubleword edge matrix is incomplete");
	test::require_engine_counts(
		harness, size_zero_cases, size_three_cases);
	harness.print_summary("arm64-simd-pmull-directed-edges");
}

} // namespace

int main()
{
	encoding_examples();
	directed_values_and_aliases();
	return 0;
}
