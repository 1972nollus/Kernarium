// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_shift_left_long_test_support.hpp"

namespace test = arm64_simd_shift_left_long_test;

namespace {

void every_size_shift_half_signedness_and_alias()
{
	test::engine_tests::EngineHarness harness;
	for (unsigned source_bits : {8U, 16U, 32U}) {
		for (unsigned shift = 0; shift < source_bits; ++shift)
			for (const bool high : {false, true})
				for (const bool unsigned_source : {false, true})
					for (const bool alias : {false, true}) {
						const unsigned destination = alias ? 27U : 26U;
						const unsigned source = 27U;
						auto initial = test::state(source_bits + shift);
						const auto encoded_instruction = test::instruction(high,
							unsigned_source, source_bits + shift,
							destination, source);
						const auto name = shift == 0U ?
							(unsigned_source ?
								"FPSIMD-0461 UXTL UXTL2" :
								"FPSIMD-0400 SXTL SXTL2") :
							"shift-left-long behavior";
						harness.check_one(name,
							encoded_instruction, initial,
							test::expected_state(initial, high,
								unsigned_source, source_bits, shift,
								destination, source),
							test::native_requirements());
					}
	}
	harness.print_summary("simd-shift-left-long-behavior");
}

} // namespace

int main()
{
	/* shift==0 covers the SXTL/SXTL2 and UXTL/UXTL2 aliases. */
	every_size_shift_half_signedness_and_alias();
	return 0;
}
