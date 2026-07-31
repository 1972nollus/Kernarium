// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_sha3_test_support.hpp"

namespace test = arm64_simd_sha3_test;

namespace {

void every_three_register_and_immediate_field()
{
	test::engine_tests::EngineHarness harness;
	test::ProgramBatcher programs{
		harness, "simd-logical-rotation-allocation"};
	for (unsigned destination = 0; destination < 32U; ++destination)
		for (unsigned first = 0; first < 32U; ++first)
			for (unsigned second = 0; second < 32U; ++second)
				programs.add({
					.operation =
						test::Operation::
							rotate_one_and_exclusive,
					.registers = {
						destination, first, second, 0,
					},
				});
	for (unsigned rotate = 0; rotate < 64U; ++rotate)
		for (unsigned destination = 0; destination < 32U;
		     ++destination)
			for (unsigned first = 0; first < 32U; ++first)
				for (unsigned second = 0; second < 32U;
				     ++second)
					programs.add({
						.operation =
							test::Operation::
								exclusive_and_rotate,
						.registers = {
							destination,
							first,
							second,
							0,
						},
						.rotate = rotate,
					});
	programs.finish();
	const auto coverage = programs.coverage();
	constexpr std::size_t expected_instructions =
		(1U + 64U) * 32U * 32U * 32U;
	constexpr std::size_t expected_programs =
		(expected_instructions +
			test::maximum_program_instructions - 1U) /
		test::maximum_program_instructions;
	test::require(coverage.instructions == expected_instructions,
		"rotation allocation omitted a field");
	test::require(coverage.programs == expected_programs,
		"rotation allocation batching is incomplete");
	test::require_engine_counts(harness, expected_programs);
	harness.print_summary("arm64-simd-logical-rotation-allocation");
}

} // namespace

int main()
{
	every_three_register_and_immediate_field();
	return 0;
}
