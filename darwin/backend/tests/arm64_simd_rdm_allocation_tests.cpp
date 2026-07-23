// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_indexed_test_support.hpp"

#include <array>

namespace base = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t instruction(bool scalar, bool q,
	bool subtract, unsigned size, unsigned destination,
	unsigned left, unsigned right)
{
	return (scalar ? 0x7e00'8400U : 0x2e00'8400U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(subtract ? 0x800U : 0U) | ((size & 3U) << 22) |
		((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

void complete_allocation_and_aliases()
{
	base::Harness harness;
	struct Registers final { unsigned destination; unsigned left; unsigned right; };
	constexpr std::array registers{
		Registers{28U, 29U, 30U}, Registers{1U, 1U, 2U},
		Registers{2U, 1U, 2U}, Registers{1U, 2U, 2U},
		Registers{3U, 3U, 3U}};
	for (const bool scalar : {false, true})
		for (const bool q : {false, true}) {
			if (scalar && q)
				continue;
			for (const bool subtract : {false, true})
				for (unsigned size = 0; size < 4U; ++size)
					for (const auto regs : registers) {
						const auto encoded = instruction(scalar, q, subtract,
							size, regs.destination, regs.left, regs.right);
						if (size == 1U || size == 2U)
							harness.require_differential(encoded,
								base::state(size + subtract + q + scalar));
						else
							harness.require_illegal(encoded,
								base::state(size + subtract + q + scalar));
					}
		}
}

} // namespace

int main()
{
	complete_allocation_and_aliases();
	return 0;
}
