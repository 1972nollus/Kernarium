// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_extract_test_support.hpp"

namespace test = arm64_base_extract_test;

namespace {

void every_shift_and_register_field(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 0U;
	for (const bool wide : {false, true}) {
		const unsigned width = wide ? 64U : 32U;
		for (unsigned shift = 0; shift < width; ++shift) {
			test::check(harness, "base-extract-shift", wide, shift,
				1U, 2U, 7U, test::state(++seed));
			for (unsigned high = 0; high < 32U; ++high)
				test::check(harness, "base-extract-high-field",
					wide, shift, high, 18U, 17U,
					test::state(++seed));
			for (unsigned low = 0; low < 32U; ++low)
				test::check(harness, "base-extract-low-field",
					wide, shift, 19U, low, 17U,
					test::state(++seed));
			for (unsigned destination = 0; destination < 32U;
			     ++destination)
				test::check(harness,
					"base-extract-destination-field", wide,
					shift, 19U, 18U, destination,
					test::state(++seed));
			for (unsigned reg = 0; reg < 32U; ++reg) {
				test::check(harness, "base-ror-immediate", wide,
					shift, reg, reg, 17U,
					test::state(++seed));
				test::check(harness,
					"base-extract-destination-high-alias",
					wide, shift, reg, 18U, reg,
					test::state(++seed));
				test::check(harness,
					"base-extract-destination-low-alias",
					wide, shift, 19U, reg, reg,
					test::state(++seed));
			}
		}
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_shift_and_register_field(harness);
	harness.print_summary("arm64-base-extract-allocation");
	return 0;
}
