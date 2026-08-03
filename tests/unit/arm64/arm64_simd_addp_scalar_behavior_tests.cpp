// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_addp_scalar_test_support.hpp"

#include <array>

namespace test = arm64_simd_addp_scalar_test;

namespace {

void wrapping_sum_alias_and_zeroing()
{
	test::engine_tests::EngineHarness harness;
	for (const auto registers : {std::array{0U, 1U}, std::array{1U, 1U},
		std::array{31U, 30U}})
		for (unsigned seed = 0; seed < 128; ++seed) {
			const auto initial = test::state(seed);
			harness.check_one("scalar pairwise add behavior",
				test::instruction(registers[0], registers[1]), initial,
				test::expected_state(initial, registers[0], registers[1]),
				test::native_requirements());
		}
	harness.print_summary("simd-addp-scalar-behavior");
}

} // namespace

int main()
{
	wrapping_sum_alias_and_zeroing();
	return 0;
}
