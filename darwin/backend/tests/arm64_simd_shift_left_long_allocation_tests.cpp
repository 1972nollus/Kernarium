// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_shift_left_long_test_support.hpp"

namespace test = arm64_simd_shift_left_long_test;

namespace {

void every_immediate_and_form()
{
	test::engine_tests::EngineHarness harness;
	/* immh == 0000 belongs to the modified-immediate allocation. */
	for (unsigned encoded = 8; encoded < 128; ++encoded)
		for (const bool high : {false, true})
			for (const bool unsigned_source : {false, true}) {
				const auto encoded_instruction = test::instruction(
					high, unsigned_source, encoded, 2U, 3U);
				const auto initial =
					test::state(encoded + high + unsigned_source);
				if (test::valid(encoded)) {
					const unsigned source_bits =
						encoded >= 32U ? 32U :
						encoded >= 16U ? 16U : 8U;
					const unsigned shift = encoded - source_bits;
					harness.check_one("shift-left-long allocation",
						encoded_instruction, initial,
						test::expected_state(initial, high,
							unsigned_source, source_bits, shift,
							2U, 3U),
						test::native_requirements());
				}
				else
					harness.check_illegal(
						"shift-left-long reserved allocation",
						encoded_instruction, initial,
						test::native_requirements());
			}
	harness.print_summary("simd-shift-left-long-allocation");
}

} // namespace

int main()
{
	every_immediate_and_form();
	return 0;
}
