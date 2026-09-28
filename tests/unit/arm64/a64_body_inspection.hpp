// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* A64 instruction classes the gadget and JIT body tests look for when they
 * walk emitted host code. Test-local on purpose: the oracle stays independent
 * of the program builder's own encodings. */

#include <cstdint>

namespace uml::testing::a64 {

/* B.cond, CBZ/CBNZ, and TBZ/TBNZ. */
[[nodiscard]] constexpr bool conditional_branch(std::uint32_t instruction) noexcept
{
	return (instruction & 0xff00'0010U) == 0x5400'0000U ||
		(instruction & 0x7e00'0000U) == 0x3400'0000U ||
		(instruction & 0x7e00'0000U) == 0x3600'0000U;
}

/* BR Xn, the threaded dispatch that ends a gadget body. */
[[nodiscard]] constexpr bool indirect_branch(std::uint32_t instruction) noexcept
{
	return (instruction & 0xffff'fc1fU) == 0xd61f'0000U;
}

} // namespace uml::testing::a64
