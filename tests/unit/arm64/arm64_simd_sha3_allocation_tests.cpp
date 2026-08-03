// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_sha3_test_support.hpp"

namespace test = arm64_simd_sha3_test;

namespace {

void every_four_register_field()
{
	test::engine_tests::EngineHarness harness;
	test::ProgramBatcher programs{
		harness, "simd-four-source-allocation"};
	for (const auto operation : {
		     test::Operation::three_way_exclusive,
		     test::Operation::clear_and_exclusive,
	     })
		for (unsigned destination = 0; destination < 32U;
		     ++destination)
			for (unsigned first = 0; first < 32U; ++first)
				for (unsigned second = 0; second < 32U;
				     ++second)
					for (unsigned third = 0; third < 32U;
					     ++third)
						programs.add({
							.operation = operation,
							.registers = {
								destination,
								first,
								second,
								third,
							},
						});
	programs.finish();
	const auto coverage = programs.coverage();
	constexpr std::size_t expected_instructions =
		2U * 32U * 32U * 32U * 32U;
	constexpr std::size_t expected_programs =
		(expected_instructions +
			test::maximum_program_instructions - 1U) /
		test::maximum_program_instructions;
	test::require(coverage.instructions == expected_instructions,
		"four-source allocation omitted a register field");
	test::require(coverage.programs == expected_programs,
		"four-source allocation batching is incomplete");
	test::require_engine_counts(harness, expected_programs);
	harness.print_summary("arm64-simd-logical-four-source-allocation");
}

} // namespace

int main()
{
	every_four_register_field();
	return 0;
}
