// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_high_narrow_test_support.hpp"

#include <array>
#include <limits>

namespace test = arm64_simd_high_narrow_test;

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
	test::require(test::instruction(test::Operation::add, {0U, false},
		{0U, 1U, 2U}) == 0x0e22'4020U,
		"ADDHN encoding mismatch");
	test::require(test::instruction(test::Operation::rounded_add,
		{1U, false}, {3U, 4U, 5U}) == 0x2e65'4083U,
		"RADDHN encoding mismatch");
	test::require(test::instruction(test::Operation::rounded_subtract,
		{2U, true}, {31U, 30U, 29U}) == 0x6ebd'63dfU,
		"RSUBHN2 encoding mismatch");
	test::require(test::instruction(test::Operation::subtract, {2U, false},
		{12U, 13U, 14U}) == 0x0eae'61acU,
		"SUBHN encoding mismatch");
}

void check_valid(test::engine_tests::EngineHarness &harness,
	test::Operation operation, test::Shape shape, test::Registers registers,
	unsigned seed)
{
	const auto initial = test::state(seed);
	harness.check_one("simd-high-narrow-valid-allocation",
		test::instruction(operation, shape, registers), initial,
		test::expected_state(initial, operation, shape, registers),
		test::native_requirements());
}

void every_register_field_arrangement_and_alias(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 1000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes) {
			for (unsigned destination = 0; destination < 32U;
			     ++destination)
				check_valid(harness, operation, shape,
					destination_case(destination), ++seed);
			for (unsigned left = 0; left < 32U; ++left)
				check_valid(harness, operation, shape,
					left_case(left), ++seed);
			for (unsigned right = 0; right < 32U; ++right)
				check_valid(harness, operation, shape,
					right_case(right), ++seed);

			constexpr std::array aliases{
				test::Registers{3U, 3U, 4U},
				test::Registers{3U, 4U, 3U},
				test::Registers{3U, 4U, 4U},
				test::Registers{3U, 3U, 3U},
				test::Registers{31U, 30U, 29U},
			};
			for (const auto registers : aliases)
				check_valid(harness, operation, shape,
					registers, ++seed);
		}
}

void every_reserved_arrangement(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 20'000U;
	for (const auto operation : test::operations)
		for (const bool upper : {false, true})
			harness.check_illegal("simd-high-narrow-reserved-size",
				test::instruction(operation, {3U, upper},
					{31U, 30U, 29U}),
				test::state(++seed), test::native_requirements());
}

} // namespace

int main()
{
	encoding_examples();
	test::engine_tests::EngineHarness harness;
	every_register_field_arrangement_and_alias(harness);
	every_reserved_arrangement(harness);
	test::require(harness.counts().interpreter == 2432U,
		"unexpected interpreter allocation count");
	test::require(harness.counts().gadgets == 2432U,
		"unexpected gadget allocation count");
	harness.print_summary("arm64-simd-high-narrow-allocation");
	return 0;
}
