// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>

namespace test = arm64_simd_test;

namespace {

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

constexpr std::array register_cases{
	Registers{0, 1, 2}, Registers{1, 1, 2},
	Registers{2, 1, 2}, Registers{1, 1, 1},
	Registers{31, 30, 29},
};

void every_valid_operation_and_alias()
{
	test::Harness harness;
	for (const bool scalar : {false, true}) {
		for (const bool q : {false, true}) {
			if (scalar && q)
				continue;
			for (const bool unsigned_group : {false, true}) {
				for (unsigned size = 0; size < 4; ++size) {
					for (unsigned opcode = 0; opcode < 24; ++opcode) {
						if (opcode == 3 || !test::valid_integer_three_same(
						    scalar, q, unsigned_group, size, opcode))
							continue;
						for (const auto registers : register_cases) {
							const auto instruction = test::three_same(
								scalar, q, unsigned_group, size, opcode,
								registers.destination, registers.left,
								registers.right);
							for (unsigned seed = 0; seed < 3; ++seed)
								harness.require_differential(instruction,
									test::state(seed + opcode * 3 + size));
						}
					}
				}
			}
		}
	}
}

} // namespace

int main()
{
	every_valid_operation_and_alias();
	return 0;
}
