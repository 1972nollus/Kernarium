// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_three_same_test_support.hpp"

namespace test = arm64_simd_three_same_test;

namespace {

void complete_integer_allocation()
{
	test::engine_tests::EngineHarness harness;
	constexpr test::Registers registers{0, 1, 2};
	for (const bool scalar : {false, true})
		for (const bool q : {false, true}) {
			if (scalar && q)
				continue;
			for (const bool unsigned_group : {false, true})
				for (unsigned size = 0; size < 4; ++size)
					for (unsigned opcode = 0; opcode < 24; ++opcode) {
						/*
						 * Opcode 3 is the overlapping bitwise-register
						 * family and has its own exhaustive suites.
						 */
						if (opcode == 3U)
							continue;
						const auto encoded = test::instruction(
							scalar, q, unsigned_group, size, opcode,
							registers);
						const auto initial = test::state(
							opcode + size * 17U +
							unsigned_group * 37U + q * 53U +
							scalar * 71U);
						if (test::valid(
						    scalar, q, unsigned_group, size, opcode)) {
							harness.check_one(
								"simd-three-same-allocation", encoded,
								initial,
								test::expected_state(initial, scalar, q,
									unsigned_group, size, opcode,
									registers),
								test::native_requirements());
						} else {
							harness.check_illegal(
								"simd-three-same-reserved", encoded,
								initial);
						}
					}
		}
	harness.print_summary("arm64-simd-three-same-allocation");
}

} // namespace

int main()
{
	complete_integer_allocation();
	return 0;
}
