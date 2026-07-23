// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t crypto_three(bool bcax,
	unsigned destination = 0, unsigned first = 1, unsigned second = 2,
	unsigned third = 3)
{
	return (bcax ? 0xce20'0000U : 0xce00'0000U) |
		((second & 31U) << 16) | ((third & 31U) << 10) |
		((first & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t rax1(unsigned destination = 0,
	unsigned first = 1, unsigned second = 2)
{
	return 0xce60'8c00U | ((second & 31U) << 16) |
		((first & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t xar(unsigned rotate,
	unsigned destination = 0, unsigned first = 1, unsigned second = 2)
{
	return 0xce80'0000U | ((second & 31U) << 16) |
		((rotate & 63U) << 10) | ((first & 31U) << 5) |
		(destination & 31U);
}

void complete_allocation()
{
	test::Harness harness;
	harness.require_differential(crypto_three(false, 31, 30, 29, 28),
		test::state(1));
	harness.require_differential(crypto_three(true, 31, 30, 29, 28),
		test::state(2));
	harness.require_differential(rax1(31, 30, 29), test::state(3));
	for (unsigned rotate = 0; rotate < 64; ++rotate)
		harness.require_differential(xar(rotate, 31, 30, 29),
			test::state(rotate));
}

} // namespace

int main()
{
	complete_allocation();
	return 0;
}
