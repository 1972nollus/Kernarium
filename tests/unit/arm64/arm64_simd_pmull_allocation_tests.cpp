// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_pmull_test_support.hpp"

namespace test = arm64_simd_pmull_test;

namespace {

void check_reserved(test::engine_tests::EngineHarness &harness,
	bool high, unsigned size, test::Registers registers,
	std::uint64_t seed)
{
	test::require(!test::valid_size(size),
		"reserved PMULL test constructed an allocated size");
	harness.check_illegal(
		"pmull-reserved-size",
		test::instruction({
			.high = high,
			.size = size,
			.registers = registers,
		}),
		test::state(seed));
}

void complete_reserved_size_partitions()
{
	test::engine_tests::EngineHarness harness;
	std::size_t cases = 0;
	std::uint64_t seed = 0;
	for (const unsigned size : {1U, 2U})
		for (const bool high : {false, true}) {
			for (unsigned field = 0; field < 32U; ++field) {
				check_reserved(
					harness, high, size,
					{field, 7U, 11U}, ++seed);
				check_reserved(
					harness, high, size,
					{13U, field, 11U}, ++seed);
				check_reserved(
					harness, high, size,
					{13U, 7U, field}, ++seed);
				cases += 3U;
			}
			for (const auto registers :
			     test::alias_register_cases) {
				check_reserved(
					harness, high, size, registers,
					++seed);
				++cases;
			}
		}
	constexpr std::size_t expected_cases =
		2U * 2U *
		(3U * 32U + test::alias_register_cases.size());
	static_assert(expected_cases == 424U);
	test::require(cases == expected_cases,
		"reserved PMULL partition coverage is incomplete");
	test::require_engine_counts(harness, 0U, 0U, cases);
	harness.print_summary("arm64-simd-pmull-reserved-allocation");
}

} // namespace

int main()
{
	complete_reserved_size_partitions();
	return 0;
}
