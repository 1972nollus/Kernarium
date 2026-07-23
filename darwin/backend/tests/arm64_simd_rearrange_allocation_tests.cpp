// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>

namespace test = arm64_simd_test;

namespace {

constexpr std::array permute_opcodes{1U, 2U, 3U, 5U, 6U, 7U};

[[nodiscard]] constexpr std::uint32_t permute(bool q, unsigned size,
	unsigned opcode, unsigned destination = 0, unsigned left = 1,
	unsigned right = 2)
{
	return 0x0e00'0800U | (q ? 0x4000'0000U : 0U) |
		((size & 3U) << 22) | ((right & 31U) << 16) |
		((opcode & 7U) << 12) | ((left & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t table_lookup(bool q, bool extend,
	unsigned count, unsigned destination = 0, unsigned first = 1,
	unsigned indices = 2)
{
	return 0x0e00'0000U | (q ? 0x4000'0000U : 0U) |
		((indices & 31U) << 16) | (((count - 1U) & 3U) << 13) |
		(extend ? 0x1000U : 0U) | ((first & 31U) << 5) |
		(destination & 31U);
}

void complete_allocation()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (unsigned size = 0; size < 4; ++size)
			for (const auto opcode : permute_opcodes) {
				const auto instruction = permute(q, size, opcode, 31, 30, 29);
				if (!q && size == 3U)
					harness.require_illegal(instruction,
						test::state(size + opcode));
				else
					harness.require_differential(instruction,
						test::state(size + opcode + q * 13U));
			}
	for (const bool q : {false, true})
		for (const bool extend : {false, true})
			for (unsigned count = 1; count <= 4; ++count)
				harness.require_differential(table_lookup(q, extend, count,
					31, 30, 29), test::state(count + extend * 11U));
}

} // namespace

int main()
{
	complete_allocation();
	return 0;
}
