// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_move_wide_test_support.hpp"

namespace test = arm64_base_move_wide_test;

namespace {

void every_immediate_and_shift(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 0U;
	for (const bool wide : {false, true})
		for (const unsigned operation : {0U, 2U, 3U})
			for (unsigned halfword = 0; halfword < (wide ? 4U : 2U);
			     ++halfword)
				for (unsigned immediate = 0; immediate < 65'536U;
				     ++immediate) {
					auto initial = test::state(++seed);
					initial.registers[7] ^=
						static_cast<std::uint64_t>(immediate) *
						0x0001'0001'0001'0001ULL;
					const bool mov_alias = operation == 2U &&
						(immediate != 0U || halfword == 0U);
					test::check(harness,
						mov_alias ?
							"base-mov-wide-immediate" :
							operation == 0U ? "base-movn" :
							operation == 2U ? "base-movz" :
							"base-movk",
						wide, operation, halfword,
						immediate, 7U, initial);
				}
}

void every_destination_register(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 1'300'000U;
	for (const bool wide : {false, true})
		for (const unsigned operation : {0U, 2U, 3U})
			for (unsigned halfword = 0; halfword < (wide ? 4U : 2U);
			     ++halfword)
				for (const unsigned immediate :
				     {0U, 1U, 0x7fffU, 0x8000U, 0xffffU})
					for (unsigned destination = 0;
					     destination < 32U; ++destination)
						test::check(harness,
							"base-move-wide-destination-field",
							wide, operation, halfword,
							immediate, destination,
							test::state(++seed));
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_immediate_and_shift(harness);
	every_destination_register(harness);
	harness.print_summary("arm64-base-move-wide-allocation");
	return 0;
}
