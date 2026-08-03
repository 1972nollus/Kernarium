// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fp_indexed_test_support.hpp"

#include <array>
#include <cstdlib>
#include <limits>

namespace test = arm64_simd_fp_indexed_test;

namespace {

[[nodiscard]] unsigned pick_register(unsigned limit, unsigned first,
	unsigned second = std::numeric_limits<unsigned>::max())
{
	for (unsigned reg = 0; reg < limit; ++reg)
		if (reg != first && reg != second)
			return reg;
	std::abort();
}

[[nodiscard]] test::Registers destination_case(const test::Shape &shape,
	unsigned destination)
{
	const auto left = pick_register(32U, destination);
	return {destination, left,
		pick_register(test::element_registers(shape), destination, left)};
}

[[nodiscard]] test::Registers left_case(const test::Shape &shape,
	unsigned left)
{
	const auto destination = pick_register(32U, left);
	return {destination, left,
		pick_register(test::element_registers(shape), destination, left)};
}

[[nodiscard]] test::Registers element_case(unsigned element)
{
	const auto destination = pick_register(32U, element);
	return {destination, pick_register(32U, destination, element), element};
}

void check_valid(test::engine_tests::EngineHarness &harness,
	test::Operation operation, const test::Shape &shape,
	test::Registers registers, unsigned index, unsigned seed)
{
	auto initial = test::state(seed);
	test::prepare_exact_inputs(initial, shape, registers, index);
	test::check_exact(harness, "fp-indexed-valid-allocation", operation,
		shape, registers, index, initial);
}

void every_register_field_shape_and_index(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 1000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes)
			for (unsigned index = 0; index < shape.indexes; ++index) {
				for (unsigned destination = 0; destination < 32U;
				     ++destination)
					check_valid(harness, operation, shape,
						destination_case(shape, destination),
						index, ++seed);
				for (unsigned left = 0; left < 32U; ++left)
					check_valid(harness, operation, shape,
						left_case(shape, left), index, ++seed);
				for (unsigned element = 0;
				     element < test::element_registers(shape);
				     ++element)
					check_valid(harness, operation, shape,
						element_case(element), index, ++seed);

				const std::array aliases{
					test::Registers{3U, 3U, 4U},
					test::Registers{3U, 4U, 3U},
					test::Registers{3U, 4U, 4U},
					test::Registers{3U, 3U, 3U},
					test::Registers{31U, 30U,
						shape.size == 0U ? 15U : 29U},
				};
				for (const auto registers : aliases)
					check_valid(harness, operation, shape,
						registers, index, ++seed);
			}
}

[[nodiscard]] std::uint32_t size_one_instruction(test::Operation operation,
	bool scalar, bool q, unsigned raw_index)
{
	const test::Shape reserved{1U, 2U, 0U, scalar, q};
	auto encoded = test::instruction(operation, reserved, 28U, 29U, 13U, 0U);
	encoded |= (raw_index & 1U) << 20;
	encoded |= (raw_index & 2U) << 20;
	encoded |= (raw_index & 4U) << 9;
	return encoded;
}

void every_reserved_shape_is_rejected_at_translation(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 20'000U;
	for (const auto operation : test::operations)
		for (unsigned raw_index = 0; raw_index < 8U; ++raw_index)
			harness.check_illegal("fp-indexed-reserved-scalar-size",
				size_one_instruction(operation, true, true, raw_index),
				test::state(++seed));

	for (const auto operation :
	     {test::Operation::fmla, test::Operation::fmls,
	      test::Operation::fmulx})
		for (const bool q : {false, true})
			for (unsigned raw_index = 0; raw_index < 8U; ++raw_index)
				harness.check_illegal(
					"fp-indexed-reserved-vector-size",
					size_one_instruction(operation, false, q,
						raw_index),
					test::state(++seed));

	const test::Shape short_double{3U, 8U, 2U, false, false};
	for (const auto operation : test::operations)
		for (unsigned index = 0; index < 2U; ++index)
			harness.check_illegal("fp-indexed-reserved-short-double",
				test::instruction(operation, short_double, 28U, 29U,
					19U, index),
				test::state(++seed));

	for (const auto operation : test::operations)
		for (const auto shape : {test::shapes[6], test::shapes[7]})
			for (unsigned index = 0; index < 2U; ++index)
				harness.check_illegal(
					"fp-indexed-reserved-double-index",
					test::instruction(operation, shape, 28U, 29U,
						19U, index) |
						0x0020'0000U,
					test::state(++seed));
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_register_field_shape_and_index(harness);
	every_reserved_shape_is_rejected_at_translation(harness);
	test::base::require(harness.counts().interpreter == 14'728U);
	test::base::require(harness.counts().gadgets == 14'728U);
	harness.print_summary("arm64-simd-fp-indexed-allocation");
	return 0;
}
