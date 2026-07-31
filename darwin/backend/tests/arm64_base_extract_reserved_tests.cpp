// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_extract_test_support.hpp"

namespace test = arm64_base_extract_test;

namespace {

void every_reserved_field(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 80'000U;
	for (unsigned shift = 32U; shift < 64U; ++shift)
		harness.check_illegal("base-extract-reserved-32-bit-shift",
			test::instruction(false, shift), test::state(++seed));
	for (const bool wide : {false, true}) {
		const unsigned width = wide ? 64U : 32U;
		for (unsigned shift = 0; shift < width; ++shift) {
			harness.check_illegal("base-extract-reserved-n",
				test::instruction(wide, shift, 1U, 2U, 7U,
					wide ? 0U : 1U),
				test::state(++seed));
			harness.check_illegal("base-extract-reserved-o0",
				test::instruction(wide, shift, 1U, 2U, 7U,
					2U, 0U, true),
				test::state(++seed));
			for (unsigned op21 = 1U; op21 < 4U; ++op21)
				harness.check_illegal("base-extract-reserved-op21",
					test::instruction(wide, shift, 1U, 2U,
						7U, 2U, op21),
					test::state(++seed));
		}
	}
	for (unsigned reg = 0; reg < 32U; ++reg) {
		harness.check_illegal("base-extract-reserved-source-register",
			test::instruction(true, 7U, reg, 18U, 17U, 2U,
				0U, true),
			test::state(++seed));
		harness.check_illegal(
			"base-extract-reserved-destination-register",
			test::instruction(false, 32U, 19U, 18U, reg),
			test::state(++seed));
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_reserved_field(harness);
	harness.print_summary("arm64-base-extract-reserved");
	return 0;
}
