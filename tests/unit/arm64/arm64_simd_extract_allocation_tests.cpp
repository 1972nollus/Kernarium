// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_extract_test_support.hpp"

namespace test = arm64_simd_extract_test;

namespace {

void complete_allocation()
{
	test::engine_tests::EngineHarness harness;
	for (const bool q : {false, true})
		for (unsigned offset = 0; offset < 16; ++offset) {
			const test::Registers registers{31, 30, 29};
			const auto instruction =
				test::instruction(q, offset, registers);
			const auto initial = test::state(offset + q * 17U);
			if (!test::valid(q, offset))
				harness.check_illegal("extract reserved allocation",
					instruction, initial,
					test::native_requirements());
			else
				harness.check_one("extract valid allocation",
					instruction, initial,
					test::expected_state(initial, q, offset,
						registers),
					test::native_requirements());
		}
	harness.print_summary("simd-extract-allocation");
}

} // namespace

int main()
{
	complete_allocation();
	return 0;
}
