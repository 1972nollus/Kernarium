// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_bitfield_test_support.hpp"

namespace test = arm64_base_bitfield_test;

namespace {

void every_legal_immediate(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 0U;
	for (const bool wide : {false, true}) {
		const unsigned width = wide ? 64U : 32U;
		for (unsigned operation = 0; operation < 3U; ++operation)
			for (unsigned immr = 0; immr < width; ++immr)
				for (unsigned imms = 0; imms < width; ++imms) {
					auto initial = test::state(++seed);
					initial.registers[1] ^=
						static_cast<std::uint64_t>(immr) << 32;
					initial.registers[7] ^=
						static_cast<std::uint64_t>(imms) << 24;
					test::check(harness,
						"base-bitfield-legal-allocation", wide,
						operation, immr, imms, 1U, 7U,
						initial);
				}
	}
}

void every_register_field_and_alias_partition(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 30'000U;
	for (const bool wide : {false, true}) {
		const unsigned width = wide ? 64U : 32U;
		for (unsigned operation = 0; operation < 3U; ++operation)
			for (const auto immediates :
			     {std::array{0U, width - 1U},
			      std::array{width - 9U, 7U},
			      std::array{7U, 15U}}) {
				for (unsigned source = 0; source < 32U; ++source)
					test::check(harness,
						"base-bitfield-source-field", wide,
						operation, immediates[0],
						immediates[1], source, 17U,
						test::state(++seed));
				for (unsigned destination = 0; destination < 32U;
				     ++destination)
					test::check(harness,
						"base-bitfield-destination-field",
						wide, operation, immediates[0],
						immediates[1], 18U, destination,
						test::state(++seed));
				for (unsigned reg = 0; reg < 32U; ++reg)
					test::check(harness,
						"base-bitfield-destructive-alias",
						wide, operation, immediates[0],
						immediates[1], reg, reg,
						test::state(++seed));
			}
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_legal_immediate(harness);
	every_register_field_and_alias_partition(harness);
	harness.print_summary("arm64-base-bitfield-allocation");
	return 0;
}
