// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_reduce_test_support.hpp"

#include <array>

namespace test = arm64_simd_reduce_test;

namespace {

constexpr std::array opcodes{3U, 10U, 26U, 27U};

void complete_allocation(test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1};
	for (const bool q : {false, true})
		for (const bool unsigned_group : {false, true})
			for (unsigned size = 0; size < 4; ++size)
				for (const auto opcode : opcodes) {
					const auto encoded = test::instruction(q,
						unsigned_group, size, opcode, registers);
					const auto initial = test::state(
						opcode + size * 37U + unsigned_group * 71U +
						q * 113U);
					if (test::valid(q, unsigned_group, size, opcode)) {
						harness.check_one(test::operation_name(
							unsigned_group, opcode), encoded, initial,
							test::expected_state(initial, q,
								unsigned_group, size, opcode, registers),
							test::native_requirements());
					} else {
						harness.check_illegal(
							"simd-reduce-reserved", encoded, initial);
					}
				}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	complete_allocation(harness);
	harness.print_summary("arm64-simd-reduce-allocation");
	return 0;
}
