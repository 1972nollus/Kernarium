// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t aes(unsigned operation,
	unsigned destination = 0, unsigned source = 1)
{
	return 0x4e28'0800U | ((operation & 7U) << 12) |
		((source & 31U) << 5) | (destination & 31U);
}

void complete_allocation()
{
	test::Harness harness;
	for (unsigned operation = 0; operation < 8; ++operation) {
		const auto instruction = aes(operation, 31, 30);
		if (operation < 4U)
			harness.require_illegal(instruction, test::state(operation));
		else
			harness.require_differential(instruction,
				test::state(operation * 13U));
	}
}

} // namespace

int main()
{
	complete_allocation();
	return 0;
}
