// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_estimate_test_support.hpp"

namespace test = arm64_simd_estimate_test;

namespace {

void complete_allocation(test::engine_tests::EngineHarness &harness)
{
	for (const bool q : {false, true})
		for (const bool reciprocal_sqrt : {false, true})
			for (const bool reserved_size : {false, true})
				for (unsigned source = 0; source < 32; ++source)
					for (unsigned destination = 0; destination < 32;
					     ++destination) {
						const test::Registers registers{
							destination, source};
						const auto encoded = test::instruction(q,
							reciprocal_sqrt, reserved_size, registers);
						const auto initial = test::state(
							destination + source * 37U + q * 71U +
							reciprocal_sqrt * 113U +
							reserved_size * 173U);
						if (reserved_size) {
							harness.check_illegal(
								"unsigned-estimate-reserved-size",
								encoded, initial);
						} else {
							harness.check_one(
								test::operation_name(reciprocal_sqrt),
								encoded, initial,
								test::expected_state(initial, q,
									reciprocal_sqrt, registers),
								test::native_requirements());
						}
					}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	complete_allocation(harness);
	harness.print_summary("arm64-simd-estimate-allocation");
	return 0;
}
