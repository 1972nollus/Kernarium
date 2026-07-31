// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_move_wide_test_support.hpp"

#include <array>

namespace test = arm64_base_move_wide_test;

namespace {

void boundary_values_and_preservation(
	test::engine_tests::EngineHarness &harness)
{
	const std::array old_values{
		std::uint64_t{0}, ~std::uint64_t{0},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
		std::uint64_t{0x8000'0000'0000'0001ULL},
	};
	unsigned seed = 1'400'000U;
	for (const bool wide : {false, true})
		for (const unsigned operation : {0U, 2U, 3U})
			for (unsigned halfword = 0; halfword < (wide ? 4U : 2U);
			     ++halfword)
				for (const unsigned immediate :
				     {0U, 1U, 0x7fffU, 0x8000U, 0xffffU})
					for (const auto old_value : old_values) {
						auto initial = test::state(++seed);
						initial.registers[7] = old_value;
						test::check(harness,
							"base-move-wide-boundary",
							wide, operation, halfword,
							immediate, 7U, initial);
					}
}

void composed_sequence(test::engine_tests::EngineHarness &harness)
{
	const std::array code{
		test::engine_tests::ProgramWord{
			.offset = 0U,
			.instruction = test::instruction(true, 2U, 0U,
				0x4567U, 7U)},
		test::engine_tests::ProgramWord{
			.offset = 4U,
			.instruction = test::instruction(true, 3U, 1U,
				0x0123U, 7U)},
		test::engine_tests::ProgramWord{
			.offset = 8U,
			.instruction = test::instruction(true, 3U, 2U,
				0xcdefU, 7U)},
		test::engine_tests::ProgramWord{
			.offset = 12U,
			.instruction = test::instruction(true, 3U, 3U,
				0x89abU, 7U)},
		test::engine_tests::ProgramWord{
			.offset = 16U, .instruction = 0xd400'0001U},
	};
	const auto initial = test::state(1'500'000U);
	auto expected = initial;
	expected.registers[7] = 0x89ab'cdef'0123'4567ULL;
	expected.pc = test::engine_tests::engine_test_code_base + 20U;
	harness.check_program({
		.name = "base-move-wide-composed-sequence",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = test::engine_tests::NativeRequirements::baseline(),
	});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	boundary_values_and_preservation(harness);
	composed_sequence(harness);
	harness.print_summary("arm64-base-move-wide-behavior");
	return 0;
}
