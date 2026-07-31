// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_move_wide_test_support.hpp"

namespace test = arm64_base_move_wide_test;

namespace {

void every_reserved_encoding(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 1'600'000U;
	for (const bool wide : {false, true})
		for (unsigned halfword = 0; halfword < 4U; ++halfword)
			for (unsigned immediate = 0; immediate < 65'536U;
			     ++immediate)
				harness.check_illegal(
					"base-move-wide-reserved-opcode",
					test::instruction(wide, 1U, halfword,
						immediate),
					test::state(++seed));
	for (const unsigned operation : {0U, 2U, 3U})
		for (unsigned halfword = 2U; halfword < 4U; ++halfword)
			for (unsigned immediate = 0; immediate < 65'536U;
			     ++immediate)
				harness.check_illegal(
					"base-move-wide-reserved-32-bit-shift",
					test::instruction(false, operation,
						halfword, immediate),
					test::state(++seed));
}

void reserved_encodings_with_every_destination(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 2'600'000U;
	for (unsigned destination = 0; destination < 32U; ++destination) {
		harness.check_illegal(
			"base-move-wide-reserved-opcode-destination",
			test::instruction(true, 1U, 3U, 0xa55aU,
				destination),
			test::state(++seed));
		harness.check_illegal(
			"base-move-wide-reserved-shift-destination",
			test::instruction(false, 3U, 3U, 0x5aa5U,
				destination),
			test::state(++seed));
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_reserved_encoding(harness);
	reserved_encodings_with_every_destination(harness);
	harness.print_summary("arm64-base-move-wide-reserved");
	return 0;
}
