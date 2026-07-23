// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

void complete_integer_allocation()
{
	test::Harness harness;
	for (const bool scalar : {false, true}) {
		for (const bool q : {false, true}) {
			if (scalar && q)
				continue;
			for (const bool unsigned_group : {false, true}) {
				for (unsigned size = 0; size < 4; ++size) {
					for (unsigned opcode = 0; opcode < 24; ++opcode) {
						/* Opcode 3 is the overlapping bitwise-register
						 * family and has its own exhaustive tests. */
						if (opcode == 3)
							continue;
						const auto instruction = test::three_same(scalar, q,
							unsigned_group, size, opcode, 0, 1, 2);
						if (test::valid_integer_three_same(scalar, q,
						    unsigned_group, size, opcode))
							harness.require_differential(instruction,
								test::state(opcode + size));
						else
							harness.require_illegal(instruction);
					}
				}
			}
		}
	}
}

} // namespace

int main()
{
	complete_integer_allocation();
	return 0;
}
