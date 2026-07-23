// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t bitwise(bool q, unsigned operation,
	unsigned destination = 0, unsigned left = 1, unsigned right = 2)
{
	return 0x0e20'1c00U | (q ? 0x4000'0000U : 0U) |
		((operation & 3U) << 22) | ((operation & 4U) << 27) |
		((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

void complete_allocation()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (unsigned operation = 0; operation < 8; ++operation)
			harness.require_differential(bitwise(q, operation),
				test::state(operation + q * 11U));
}

} // namespace

int main()
{
	complete_allocation();
	return 0;
}
