// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_widening_test_support.hpp"

#include <array>
#include <limits>

namespace test = arm64_simd_widening_test;

namespace {

[[nodiscard]] unsigned pick_register(unsigned first,
	unsigned second = std::numeric_limits<unsigned>::max())
{
	for (unsigned reg = 0; reg < 32U; ++reg)
		if (reg != first && reg != second)
			return reg;
	std::abort();
}

[[nodiscard]] test::Registers destination_case(unsigned destination)
{
	const auto left = pick_register(destination);
	return {destination, left, pick_register(destination, left)};
}

[[nodiscard]] test::Registers left_case(unsigned left)
{
	const auto destination = pick_register(left);
	return {destination, left, pick_register(destination, left)};
}

[[nodiscard]] test::Registers right_case(unsigned right)
{
	const auto destination = pick_register(right);
	return {destination, pick_register(destination, right), right};
}

void encoding_examples()
{
	test::require(test::instruction(test::Operation::add_long, false,
		{0U, false}, {0U, 1U, 2U}) == 0x0e22'0020U,
		"SADDL encoding mismatch");
	test::require(test::instruction(test::Operation::add_wide, true,
		{1U, true}, {3U, 4U, 5U}) == 0x6e65'1083U,
		"UADDW2 encoding mismatch");
	test::require(test::instruction(test::Operation::subtract_long, false,
		{2U, true}, {31U, 30U, 29U}) == 0x4ebd'23dfU,
		"SSUBL2 encoding mismatch");
	test::require(test::instruction(test::Operation::subtract_wide, true,
		{0U, false}, {12U, 13U, 14U}) == 0x2e2e'31acU,
		"USUBW encoding mismatch");
	test::require(test::instruction(
		test::Operation::absolute_accumulate, false, {1U, false},
		{3U, 4U, 5U}) == 0x0e65'5083U,
		"SABAL encoding mismatch");
	test::require(test::instruction(test::Operation::absolute_long, true,
		{2U, true}, {31U, 30U, 29U}) == 0x6ebd'73dfU,
		"UABDL2 encoding mismatch");
}

void check_valid(test::engine_tests::EngineHarness &harness,
	test::Operation operation, bool unsigned_values, test::Shape shape,
	test::Registers registers, unsigned seed)
{
	const auto initial = test::state(seed);
	harness.check_one("simd-widening-valid-allocation",
		test::instruction(operation, unsigned_values, shape, registers),
		initial,
		test::expected_state(initial, operation, unsigned_values,
			shape, registers),
		test::native_requirements());
}

void every_register_field_arrangement_and_alias(
	test::engine_tests::EngineHarness &harness)
{
	constexpr std::array aliases{
		test::Registers{3U, 3U, 4U},
		test::Registers{3U, 4U, 3U},
		test::Registers{3U, 4U, 4U},
		test::Registers{3U, 3U, 3U},
		test::Registers{31U, 30U, 29U},
	};
	unsigned seed = 1000U;
	for (const auto operation : test::operations)
		for (const bool unsigned_values : {false, true})
			for (const auto shape : test::shapes) {
				for (unsigned destination = 0; destination < 32U;
				     ++destination)
					check_valid(harness, operation, unsigned_values,
						shape, destination_case(destination), ++seed);
				for (unsigned left = 0; left < 32U; ++left)
					check_valid(harness, operation, unsigned_values,
						shape, left_case(left), ++seed);
				for (unsigned right = 0; right < 32U; ++right)
					check_valid(harness, operation, unsigned_values,
						shape, right_case(right), ++seed);
				for (const auto registers : aliases)
					check_valid(harness, operation, unsigned_values,
						shape, registers, ++seed);
			}
}

void every_reserved_arrangement(test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{31U, 30U, 29U};
	unsigned seed = 20'000U;
	for (const auto operation : test::operations)
		for (const bool unsigned_values : {false, true}) {
			for (const bool upper : {false, true})
				harness.check_illegal(
					"simd-widening-reserved-size",
					test::instruction(operation, unsigned_values,
						{3U, upper}, registers),
					test::state(++seed),
					test::native_requirements());
			for (unsigned size = 0; size < 4U; ++size)
				harness.check_illegal(
					"simd-widening-reserved-scalar",
					test::scalar_instruction(operation,
						unsigned_values, size, registers),
					test::state(++seed),
					test::native_requirements());
		}
}

} // namespace

int main()
{
	encoding_examples();
	test::engine_tests::EngineHarness harness;
	every_register_field_arrangement_and_alias(harness);
	every_reserved_arrangement(harness);
	/* The 7,272 allocated vector cases need Advanced SIMD. The 72
	 * architecturally invalid encodings still exercise JIT rejection. */
	test::require_engine_counts(harness, 7344U, 7272U);
	harness.print_summary("arm64-simd-widening-allocation");
	return 0;
}
