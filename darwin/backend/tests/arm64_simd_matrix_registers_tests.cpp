// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_matrix_test_support.hpp"

namespace test = arm64_simd_matrix_test;

namespace {

void every_register_field_combination()
{
	test::engine_tests::EngineHarness harness;
	for (const auto mode : test::modes)
		for (unsigned destination = 0; destination < 32U; ++destination)
			for (unsigned left = 0; left < 32U; ++left)
				for (unsigned right = 0; right < 32U; ++right) {
					const test::Registers registers{
						destination, left, right};
					const auto initial = test::state(
						destination * 19U + left * 7U + right +
						static_cast<unsigned>(mode) * 43U);
					test::check(harness,
						"matrix every register combination",
						mode, registers, initial);
				}
	harness.print_summary("simd-matrix-registers");
}

} // namespace

int main()
{
	every_register_field_combination();
	return 0;
}
