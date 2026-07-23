// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t pmull(bool high, unsigned size,
	unsigned destination = 0, unsigned left = 1, unsigned right = 2)
{
	return 0x0e20'e000U | (high ? 0x4000'0000U : 0U) |
		((size & 3U) << 22) | ((right & 31U) << 16) |
		((left & 31U) << 5) | (destination & 31U);
}

void complete_allocation()
{
	test::Harness harness;
	for (const bool high : {false, true})
		for (unsigned size = 0; size < 4; ++size) {
			const auto instruction = pmull(high, size, 31, 30, 29);
			if (size == 0U || size == 3U)
				harness.require_differential(instruction,
					test::state(size + high * 17U));
			else
				harness.require_illegal(instruction, test::state(size));
		}
}

} // namespace

int main()
{
	complete_allocation();
	return 0;
}
