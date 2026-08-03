// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_pmull_test_support.hpp"

namespace test = arm64_simd_pmull_test;

namespace {

void forced_portable_doubleword_path()
{
	/* This option changes only gadget selection. The native engine still
	 * follows the instruction's real host requirement and is skipped by the
	 * shared harness on a host without the optional extension.
	 */
	test::engine_tests::EngineHarness harness{
		{.force_software_pmull = true}};
	std::size_t cases = 0;
	for (const bool high : {false, true})
		for (const auto registers : test::alias_register_cases)
			for (unsigned pattern = 0;
			     pattern < test::edge_values.size(); ++pattern) {
				test::check_one(
					harness, "pmull-forced-portable",
					{
						.high = high,
						.size = 3,
						.registers = registers,
					},
					test::edge_state(
						pattern + high * 101U));
				++cases;
			}
	constexpr std::size_t expected_cases =
		2U * test::alias_register_cases.size() *
		test::edge_values.size();
	static_assert(expected_cases == 320U);
	test::require(cases == expected_cases,
		"forced-portable PMULL coverage is incomplete");
	test::require_engine_counts(harness, 0U, cases);
	harness.print_summary("arm64-simd-pmull-forced-portable");
}

} // namespace

int main()
{
	forced_portable_doubleword_path();
	return 0;
}
