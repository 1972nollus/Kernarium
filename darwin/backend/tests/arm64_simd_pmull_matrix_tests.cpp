// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_pmull_test_support.hpp"

namespace test = arm64_simd_pmull_test;

namespace {

void every_valid_encoding_field()
{
	test::engine_tests::EngineHarness harness;
	std::size_t size_zero_programs = 0;
	std::size_t size_three_programs = 0;
	for (const unsigned size : {0U, 3U})
		for (const bool high : {false, true}) {
			test::ProgramBatcher programs{
				harness,
				size == 0U ?
					"pmull-byte-register-matrix" :
					"pmull-doubleword-register-matrix",
				size, high,
				size * 0x1'0000U + high * 0x8'0000U,
			};
			for (unsigned destination = 0; destination < 32U;
			     ++destination)
				for (unsigned left = 0; left < 32U; ++left)
					for (unsigned right = 0; right < 32U;
					     ++right)
						programs.add({
							destination,
							left,
							right,
						});
			programs.finish();
			const auto coverage = programs.coverage();
			constexpr std::size_t expected_instructions =
				32U * 32U * 32U;
			constexpr std::size_t expected_programs =
				(expected_instructions +
					test::maximum_products_per_program - 1U) /
				test::maximum_products_per_program;
			static_assert(expected_instructions == 32'768U);
			static_assert(expected_programs == 1'058U);
			test::require(
				coverage.instructions == expected_instructions,
				"PMULL register matrix omitted an encoding");
			test::require(coverage.programs == expected_programs,
				"PMULL register matrix batching is incomplete");
			if (size == 0U)
				size_zero_programs += coverage.programs;
			else
				size_three_programs += coverage.programs;
		}
	test::require(size_zero_programs == 2'116U,
		"PMULL byte-form program count is wrong");
	test::require(size_three_programs == 2'116U,
		"PMULL doubleword-form program count is wrong");
	test::require_engine_counts(
		harness, size_zero_programs, size_three_programs);
	harness.print_summary("arm64-simd-pmull-register-matrix");
}

} // namespace

int main()
{
	every_valid_encoding_field();
	return 0;
}
