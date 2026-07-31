// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_sha3_test_support.hpp"

#include <array>

namespace test = arm64_simd_sha3_test;

namespace {

constexpr std::array four_register_cases{
	test::Registers{0, 1, 2, 3},
	test::Registers{1, 1, 2, 3},
	test::Registers{2, 1, 2, 3},
	test::Registers{3, 1, 2, 3},
	test::Registers{1, 1, 1, 1},
	test::Registers{0, 1, 1, 2},
	test::Registers{0, 1, 2, 1},
	test::Registers{0, 1, 2, 2},
	test::Registers{31, 30, 29, 28},
	test::Registers{30, 31, 29, 28},
	test::Registers{29, 30, 29, 28},
	test::Registers{28, 30, 29, 28},
};

constexpr std::array three_register_cases{
	test::Registers{0, 1, 2, 0},
	test::Registers{1, 1, 2, 0},
	test::Registers{2, 1, 2, 0},
	test::Registers{1, 1, 1, 0},
	test::Registers{31, 30, 29, 0},
	test::Registers{30, 30, 29, 0},
	test::Registers{29, 30, 29, 0},
	test::Registers{31, 31, 31, 0},
};

constexpr std::array<std::uint64_t, 12> edge_values{
	std::uint64_t{0},
	~std::uint64_t{},
	std::uint64_t{1},
	std::uint64_t{1} << 63U,
	0xaaaa'aaaa'aaaa'aaaaULL,
	0x5555'5555'5555'5555ULL,
	0x0123'4567'89ab'cdefULL,
	0xfedc'ba98'7654'3210ULL,
	0x8000'0000'0000'0001ULL,
	0x7fff'ffff'ffff'ffffULL,
	0x0000'0000'ffff'ffffULL,
	0xffff'ffff'0000'0000ULL,
};

[[nodiscard]] test::arm64::CpuState edge_state(unsigned pattern)
{
	auto cpu = test::state(pattern + 100U);
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned lane = 0; lane < 2U; ++lane)
			test::write_lane(cpu.vectors[reg].bytes, lane,
				edge_values[(pattern + reg * 5U + lane * 7U) %
					edge_values.size()]);
	return cpu;
}

void four_source_truth_tables_and_aliases(
	test::engine_tests::EngineHarness &harness)
{
	for (const auto operation : {
		     test::Operation::three_way_exclusive,
		     test::Operation::clear_and_exclusive,
	     })
		for (const auto registers : four_register_cases)
			for (unsigned pattern = 0; pattern < 16U; ++pattern)
				test::check_one(harness,
					{operation, registers, 0},
					edge_state(pattern));
}

void rotate_one_boundaries_and_aliases(
	test::engine_tests::EngineHarness &harness)
{
	for (const auto registers : three_register_cases)
		for (unsigned pattern = 0; pattern < 16U; ++pattern)
			test::check_one(harness,
				{
					test::Operation::rotate_one_and_exclusive,
					registers,
					0,
				},
				edge_state(pattern + 20U));
}

void every_rotation_and_alias(
	test::engine_tests::EngineHarness &harness)
{
	for (unsigned rotate = 0; rotate < 64U; ++rotate)
		for (const auto registers : three_register_cases)
			for (unsigned pattern = 0; pattern < 4U; ++pattern)
				test::check_one(harness,
					{
						test::Operation::
							exclusive_and_rotate,
						registers,
						rotate,
					},
					edge_state(pattern + rotate * 3U));
}

void run_matrix(test::engine_tests::EngineHarnessOptions options,
	std::string_view summary)
{
	test::engine_tests::EngineHarness harness{options};
	four_source_truth_tables_and_aliases(harness);
	rotate_one_boundaries_and_aliases(harness);
	every_rotation_and_alias(harness);
	constexpr std::size_t expected_cases =
		2U * four_register_cases.size() * 16U +
		three_register_cases.size() * 16U +
		64U * three_register_cases.size() * 4U;
	static_assert(expected_cases == 2'560U);
	test::require_engine_counts(harness, expected_cases);
	harness.print_summary(summary);
}

} // namespace

int main()
{
	run_matrix({}, "arm64-simd-logical-rotation-behavior");
	run_matrix(
		{.force_software_sha3 = true},
		"arm64-simd-logical-rotation-portable");
	return 0;
}
