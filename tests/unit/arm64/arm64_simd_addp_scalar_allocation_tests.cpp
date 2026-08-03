// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_addp_scalar_test_support.hpp"

namespace test = arm64_simd_addp_scalar_test;

namespace {

void complete_allocation()
{
	test::engine_tests::EngineHarness harness;
	for (unsigned destination = 0; destination < 32; ++destination)
		for (unsigned source = 0; source < 32; ++source) {
			const auto initial =
				test::state(destination * 32U + source);
			harness.check_one("scalar pairwise add allocation",
				test::instruction(destination, source), initial,
				test::expected_state(initial, destination, source),
				test::native_requirements());
		}
	harness.print_summary("simd-addp-scalar-allocation");
}

} // namespace

int main()
{
	complete_allocation();
	return 0;
}
