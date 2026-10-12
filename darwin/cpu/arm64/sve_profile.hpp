// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64 {

/* The state the removed non-streaming SVE profile held in CpuState, and the
 * predicate arithmetic streaming mode and the SM = 0 vector-length family
 * share. The virtual CPU implements no SVE outside streaming mode and
 * advertises none (platform::without_sve); P0-P15 and FFR stay in CpuState,
 * reserved and written only by a change of PSTATE.SM, which zeroes them, so
 * that no offset moves.
 *
 * A predicate is in mask form: a 16-byte image with 0xff in each active byte
 * and 0x00 in the others, one bit per byte of a 16-byte vector. The 16-bit
 * packed form exists only at the bridge. */
inline constexpr unsigned sve_vector_length = 16U;
inline constexpr unsigned sve_p_registers = 16U;

using SvePredicate = std::array<std::uint8_t, sve_vector_length>;

[[nodiscard]] constexpr SvePredicate sve_predicate_mask(
	std::uint16_t packed) noexcept
{
	SvePredicate mask{};
	for (unsigned bit = 0; bit < sve_vector_length; ++bit)
		mask[bit] = (packed >> bit & 1U) != 0U ? 0xffU : 0x00U;
	return mask;
}

/* The packed form of a mask; a byte counts as set by its top bit. */
[[nodiscard]] constexpr std::uint16_t sve_predicate_packed(
	const SvePredicate &mask) noexcept
{
	unsigned packed = 0;
	for (unsigned bit = 0; bit < sve_vector_length; ++bit)
		packed |= static_cast<unsigned>(mask[bit] >> 7U) << bit;
	return static_cast<std::uint16_t>(packed);
}

/* DecodePredCount(): how many of a vector's `elements` elements a
 * predicate constraint names (POW2, VL1-VL256, MUL4, MUL3, ALL); the
 * reserved patterns name none. It serves the streaming forms at SVL too. */
[[nodiscard]] constexpr unsigned predicate_pattern_count(unsigned pattern,
	unsigned elements) noexcept
{
	switch (pattern) {
	case 0: /* POW2 */
		return std::bit_floor(elements);
	case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
		return elements >= pattern ? pattern : 0U;
	case 9: case 10: case 11: case 12: case 13: {
		const unsigned count = 16U << (pattern - 9U);
		return elements >= count ? count : 0U;
	}
	case 29: /* MUL4 */
		return elements - elements % 4U;
	case 30: /* MUL3 */
		return elements - elements % 3U;
	case 31: /* ALL */
		return elements;
	default:
		return 0;
	}
}

static_assert(sve_predicate_packed(sve_predicate_mask(0xa5c3U)) == 0xa5c3U);
static_assert(predicate_pattern_count(0, 16) == 16 &&
	predicate_pattern_count(30, 8) == 6 &&
	predicate_pattern_count(9, 8) == 0 &&
	predicate_pattern_count(14, 16) == 0);

} // namespace uml::cpu::arm64
