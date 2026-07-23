// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t addp_scalar(
	unsigned destination = 0, unsigned source = 1)
{
	return 0x5ef1'b800U | ((source & 31U) << 5) | (destination & 31U);
}

void complete_allocation()
{
	test::Harness harness;
	for (unsigned destination = 0; destination < 32; ++destination)
		harness.require_differential(addp_scalar(destination, 31U),
			test::state(destination));
}

} // namespace

int main()
{
	complete_allocation();
	return 0;
}
