// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fp_indexed_test_support.hpp"

#include <array>

namespace test = arm64_simd_fp_indexed_test;

namespace {

void every_operation_shape_and_index_selects_the_specified_element(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 30'000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes)
			for (unsigned index = 0; index < shape.indexes; ++index) {
				const test::Registers registers{
					28U, 29U, shape.size == 0U ? 13U : 19U};
				auto initial = test::state(++seed);
				test::prepare_exact_inputs(initial, shape, registers,
					index);
				test::check_exact(harness,
					"fp-indexed-element-selection", operation,
					shape, registers, index, initial);
			}
}

void every_source_destination_alias_reads_original_values(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 40'000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes) {
			const std::array aliases{
				test::Registers{3U, 3U, 4U},
				test::Registers{3U, 4U, 3U},
				test::Registers{3U, 4U, 4U},
				test::Registers{3U, 3U, 3U},
				test::Registers{31U, 30U,
					shape.size == 0U ? 15U : 29U},
			};
			for (const auto registers : aliases) {
				const auto index = shape.indexes - 1U;
				auto initial = test::state(++seed);
				test::prepare_exact_inputs(initial, shape, registers,
					index);
				test::check_exact(harness,
					"fp-indexed-register-alias", operation,
					shape, registers, index, initial);
			}
		}
}

void multiply_add_and_subtract_round_only_once(
	test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{28U, 29U, 19U};
	for (const auto operation :
	     {test::Operation::fmla, test::Operation::fmls})
		for (const auto shape : {test::shapes[3], test::shapes[5]}) {
			auto initial = test::state(50'000U +
				static_cast<unsigned>(operation) + shape.q);
			initial.fpcr = 0U;
			initial.fpsr = test::fpsr_saturation;
			test::fill_bits(initial.vectors[registers.left].bytes, 4U,
				0x3f80'0001U);
			test::fill_bits(initial.vectors[registers.element].bytes, 4U,
				0x3f80'0001U);
			const auto accumulator =
				operation == test::Operation::fmla ?
					0xbf80'0002U : 0x3f80'0002U;
			test::fill_bits(
				initial.vectors[registers.destination].bytes,
				4U, accumulator);
			const auto expected =
				operation == test::Operation::fmla ?
					0x2880'0000U : 0xa880'0000U;
			test::check_uniform(harness,
				"fp-indexed-single-rounding", operation, shape,
				registers, 3U, initial, expected);
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_operation_shape_and_index_selects_the_specified_element(harness);
	every_source_destination_alias_reads_original_values(harness);
	multiply_add_and_subtract_round_only_once(harness);
	test::base::require(harness.counts().interpreter == 324U);
	test::base::require(harness.counts().gadgets == 324U);
	harness.print_summary("arm64-simd-fp-indexed-arithmetic");
	return 0;
}
