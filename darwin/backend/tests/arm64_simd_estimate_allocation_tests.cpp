// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t estimate(bool q, bool reciprocal_sqrt,
	bool invalid_size, unsigned destination = 0, unsigned source = 1)
{
	return 0x0ea1'c800U | (q ? 0x4000'0000U : 0U) |
		(reciprocal_sqrt ? 0x2000'0000U : 0U) |
		(invalid_size ? 0x0040'0000U : 0U) |
		((source & 31U) << 5) | (destination & 31U);
}

void complete_allocation()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (const bool reciprocal_sqrt : {false, true})
			for (const bool invalid_size : {false, true}) {
				const auto instruction = estimate(q, reciprocal_sqrt,
					invalid_size);
				if (invalid_size)
					harness.require_illegal(instruction);
				else
					harness.require_differential(instruction,
						test::state(7U + q + reciprocal_sqrt * 2U));
			}
}

} // namespace

int main()
{
	complete_allocation();
	return 0;
}
