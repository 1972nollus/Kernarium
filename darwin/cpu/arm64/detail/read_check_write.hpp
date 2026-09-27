// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu::arm64::detail {

/* A FEAT_THE read-check-write word: RCWCAS or RCWCLR/RCWSWP/RCWSET on one
 * register, or the pair forms. */
struct ReadCheckWrite final {
	bool recognized{};
	bool pair{};
	bool compare_swap{};
	/* Bits 14:12; a modify's operation is 1 to 3 when it is valid. */
	unsigned operation{};
	/* Bit 30: the S form, which checks against RCWSMASK. */
	unsigned soft{};
};

/* The cells each read-check-write class claims. A scalar modify claims only
 * operations 1 to 3. A pair modify claims every operation except 000 with S
 * clear, so a caller still rejects its other operations, and its register
 * choices, as undefined. */
[[nodiscard]] constexpr ReadCheckWrite decode_read_check_write(
	std::uint32_t instruction) noexcept
{
	const unsigned operation = (instruction >> 12) & 7U;
	const unsigned soft = (instruction >> 30) & 1U;
	if ((instruction & 0xbf20'fc00U) == 0x1920'0800U)
		return {true, false, true, operation, soft};
	if ((instruction & 0xbf20'8c00U) == 0x3820'8000U &&
	    operation >= 1U && operation <= 3U)
		return {true, false, false, operation, soft};
	if ((instruction & 0xbf20'fc00U) == 0x1920'0c00U)
		return {true, true, true, operation, soft};
	if ((instruction & 0xbf20'8c00U) == 0x1920'8000U &&
	    (operation != 0U || soft != 0U))
		return {true, true, false, operation, soft};
	return {};
}

} // namespace uml::cpu::arm64::detail
