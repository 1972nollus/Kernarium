// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_immediate_test_support.hpp"

namespace test = arm64_simd_immediate_test;

namespace {

void complete_allocation()
{
	test::engine_tests::EngineHarness harness;
	for (const bool q : {false, true})
		for (const bool op : {false, true})
			for (unsigned mode = 0; mode < 16; ++mode)
				for (const bool o2 : {false, true}) {
					constexpr unsigned immediate = 0xa5U;
					constexpr unsigned destination = 31U;
					const auto instruction = test::instruction(
						q, op, mode, o2, immediate, destination);
					const auto initial =
						test::state(mode + q * 17U + op * 31U + o2 * 43U);
					if (test::valid(q, op, mode, o2)) {
						harness.check_one("simd-immediate-allocation",
							instruction, initial,
							test::expected_state(initial, q, op, mode, o2,
								immediate, destination),
							test::native_requirements(o2));
					} else {
						harness.check_illegal(
							"simd-immediate-reserved", instruction,
							initial);
					}
				}
	harness.print_summary("arm64-simd-immediate-allocation");
}

} // namespace

int main()
{
	complete_allocation();
	return 0;
}
