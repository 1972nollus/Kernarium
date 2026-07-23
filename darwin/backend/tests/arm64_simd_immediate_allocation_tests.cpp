// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t modified_immediate(bool q, bool op,
	unsigned mode, bool o2, unsigned immediate, unsigned destination = 0)
{
	return 0x0f00'0400U | (q ? 0x4000'0000U : 0U) |
		(op ? 0x2000'0000U : 0U) | ((immediate & 0xe0U) << 11) |
		((mode & 0xfU) << 12) | (o2 ? 0x800U : 0U) |
		((immediate & 0x1fU) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr bool valid(bool q, bool op, unsigned mode, bool o2)
{
	if (o2)
		return !op && mode == 15U;
	return !op || mode != 15U || q;
}

void complete_allocation()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (const bool op : {false, true})
			for (unsigned mode = 0; mode < 16; ++mode)
				for (const bool o2 : {false, true}) {
					const auto instruction = modified_immediate(
						q, op, mode, o2, 0xa5U, 31U);
					if (valid(q, op, mode, o2))
						harness.require_differential(instruction,
							test::state(mode + q * 17U + op * 31U));
					else
						harness.require_illegal(instruction,
							test::state(mode + o2 * 19U));
				}
}

} // namespace

int main()
{
	complete_allocation();
	return 0;
}
