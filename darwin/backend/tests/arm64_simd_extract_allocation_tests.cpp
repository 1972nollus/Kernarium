// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t extract(bool q, unsigned offset,
	unsigned destination = 0, unsigned left = 1, unsigned right = 2)
{
	return 0x2e00'0000U | (q ? 0x4000'0000U : 0U) |
		((right & 31U) << 16) | ((offset & 15U) << 11) |
		((left & 31U) << 5) | (destination & 31U);
}

void complete_allocation()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (unsigned offset = 0; offset < 16; ++offset) {
			const auto instruction = extract(q, offset, 31, 30, 29);
			if (!q && offset >= 8U)
				harness.require_illegal(instruction, test::state(offset));
			else
				harness.require_differential(instruction,
					test::state(offset + q * 17U));
		}
}

} // namespace

int main()
{
	complete_allocation();
	return 0;
}
