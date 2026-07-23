// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>

namespace test = arm64_simd_test;

namespace {

constexpr std::array opcodes{3U, 10U, 26U, 27U};

[[nodiscard]] constexpr std::uint32_t reduction(bool q, bool unsigned_group,
	unsigned size, unsigned opcode, unsigned destination = 0,
	unsigned source = 1)
{
	return 0x0e30'0800U | (q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((opcode & 31U) << 12) |
		((source & 31U) << 5) | (destination & 31U);
}

void complete_allocation()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (const bool unsigned_group : {false, true})
			for (unsigned size = 0; size < 4; ++size)
				for (const auto opcode : opcodes) {
					const auto instruction = reduction(q, unsigned_group,
						size, opcode);
					const bool shape = size < 3U && (q || size < 2U);
					const bool operation = opcode != 27U || !unsigned_group;
					if (shape && operation)
						harness.require_differential(instruction,
							test::state(opcode + size));
					else
						harness.require_illegal(instruction);
				}
}

} // namespace

int main()
{
	complete_allocation();
	return 0;
}
