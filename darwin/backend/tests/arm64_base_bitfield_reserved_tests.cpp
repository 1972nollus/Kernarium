// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_bitfield_test_support.hpp"

namespace test = arm64_base_bitfield_test;

namespace {

void every_reserved_allocation(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 500'000U;
	for (const bool wide : {false, true}) {
		const unsigned width = wide ? 64U : 32U;
		for (unsigned immr = 0; immr < 64U; ++immr)
			for (unsigned imms = 0; imms < 64U; ++imms)
				harness.check_illegal("base-bitfield-reserved-opcode",
					test::instruction(wide, 3U, immr, imms),
					test::state(++seed));
		for (unsigned operation = 0; operation < 3U; ++operation)
			for (unsigned immr = 0; immr < width; ++immr)
				for (unsigned imms = 0; imms < width; ++imms)
					harness.check_illegal(
						"base-bitfield-reserved-n",
						test::instruction(wide, operation,
							immr, imms, 1U, 7U,
							wide ? 0U : 1U),
						test::state(++seed));
	}
	for (unsigned operation = 0; operation < 3U; ++operation)
		for (unsigned immr = 0; immr < 64U; ++immr)
			for (unsigned imms = 0; imms < 64U; ++imms) {
				if (immr < 32U && imms < 32U)
					continue;
				harness.check_illegal(
					"base-bitfield-reserved-32-bit-immediate",
					test::instruction(false, operation, immr,
						imms),
					test::state(++seed));
			}
}

void reserved_fields_still_reject_with_every_register(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 600'000U;
	for (unsigned reg = 0; reg < 32U; ++reg) {
		harness.check_illegal("base-bitfield-reserved-source-field",
			test::instruction(true, 3U, 7U, 15U, reg, 17U),
			test::state(++seed));
		harness.check_illegal("base-bitfield-reserved-destination-field",
			test::instruction(false, 0U, 32U, 15U, 18U, reg),
			test::state(++seed));
		harness.check_illegal("base-bitfield-reserved-alias-field",
			test::instruction(true, 2U, 7U, 15U, reg, reg, 0U),
			test::state(++seed));
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_reserved_allocation(harness);
	reserved_fields_still_reject_with_every_register(harness);
	harness.print_summary("arm64-base-bitfield-reserved");
	return 0;
}
