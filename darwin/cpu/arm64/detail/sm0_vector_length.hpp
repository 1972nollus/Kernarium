// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/sme_profile.hpp"
#include "cpu/arm64/sve_profile.hpp"
#include "cpu/host_capabilities.hpp"

#include <cstdint>
#include <optional>

namespace uml::cpu::arm64::detail {

/*
 * The SM = 0 vector-length family: a deliberate departure from the
 * architecture, for a virtual CPU with FEAT_SME and without FEAT_SVE.
 *
 * Such a CPU executes no SVE word at PSTATE.SM = 0; the architecture makes
 * every one UNDEFINED, and an M4 traps them. Compilers target armv9-a, which
 * implies SVE2, so a locally-streaming function (an SME routine whose body
 * the compiler wraps in SMSTART and SMSTOP) saves the non-streaming
 * vector length with CNTD before it enters streaming mode, and would
 * SIGILL on every host that advertises SME. This virtual CPU therefore
 * executes the instructions that only count or scale a vector length at
 * SM = 0, for a constant non-streaming vector length of 16 bytes, the one
 * the SVE profile uses (sve_vector_length):
 *
 * - CNTB, CNTH, CNTW and CNTD;
 * - RDVL, ADDVL and ADDPL;
 * - the scalar INCB, INCH, INCW, INCD, DECB, DECH, DECW and DECD;
 *
 * each with every predicate pattern and multiplier. They touch no vector or
 * predicate register, so a program holding one never needs the FP state or
 * the SVE task state. Every other SVE word at SM = 0 stays UNDEFINED. In
 * streaming mode the same words keep their architectural meaning with the
 * streaming vector length.
 *
 * Both engines decode a family word to an ordinary base A64 word of the same
 * effect, which sm0_vector_length_equivalent() computes, and translate that
 * word instead. The family therefore has one definition, shared by
 * construction. Where the virtual CPU has the SVE profile the engines'
 * SVE path decodes the same words first and this one is not consulted.
 */

/* Whether the virtual CPU on `host` executes the family at PSTATE.SM = 0:
 * it implements FEAT_SME and not the SVE profile. */
[[nodiscard]] inline bool implements_sm0_vector_length(
	const HostCapabilities &host) noexcept
{
	return implements_sme(host) && !implements_sve(host);
}

/* The base A64 words the family translates to. */
inline constexpr std::uint32_t sm0_vector_length_nop = 0xd503'201fU;

/* Whether `word` is a family word: CNT[BHWD] (scalar), INC[BHWD] (scalar),
 * DEC[BHWD] (scalar), RDVL, ADDVL or ADDPL. */
[[nodiscard]] constexpr bool sm0_vector_length_word(
	std::uint32_t word) noexcept
{
	const std::uint32_t element_count = word & 0xff30'fc00U;
	return element_count == 0x0420'e000U || element_count == 0x0430'e000U ||
		element_count == 0x0430'e400U ||
		(word & 0xffff'f800U) == 0x04bf'5000U ||
		(word & 0xffa0'f800U) == 0x0420'5000U;
}

/*
 * The base A64 word with the effect of a family word at a vector length of
 * sve_vector_length bytes, or nothing for any other word:
 *
 * - CNTx Xd, pattern, MUL #m is MOVZ Xd, #count; RDVL Xd, #n is MOVZ or MOVN
 *   Xd, #n * 16;
 * - INCx and DECx Xdn, pattern, MUL #m are ADD and SUB Xdn, Xdn, #count;
 * - ADDVL Xd|SP, Xn|SP, #n is ADD or SUB Xd|SP, Xn|SP, #n * 16, and ADDPL
 *   likewise with n * 2, the predicate length;
 *
 * where count is the number of elements the pattern names (DecodePredCount)
 * times the multiplier. A destination of XZR (register 31 of CNT, RDVL, INC
 * and DEC) discards the result, and the family sets no flags, so each such
 * word, and an INC or DEC by zero, is a NOP.
 */
[[nodiscard]] constexpr std::optional<std::uint32_t>
sm0_vector_length_equivalent(std::uint32_t word) noexcept
{
	if (!sm0_vector_length_word(word))
		return std::nullopt;
	const std::uint32_t rd = word & 31U;
	const std::uint32_t pattern = (word >> 5) & 31U;
	const std::uint32_t immediate6 = (word >> 5) & 63U;
	const std::int32_t signed6 = immediate6 >= 32U ?
		static_cast<std::int32_t>(immediate6) - 64 :
		static_cast<std::int32_t>(immediate6);
	constexpr std::uint32_t move_wide_zero = 0xd280'0000U;
	constexpr std::uint32_t move_wide_not = 0x9280'0000U;
	constexpr std::uint32_t add_immediate = 0x9100'0000U;
	constexpr std::uint32_t subtract_immediate = 0xd100'0000U;

	if ((word & 0xffff'f800U) == 0x04bf'5000U || (word & 0xff30'fc00U) ==
		    0x0420'e000U) {
		if (rd == 31U)
			return sm0_vector_length_nop;
		std::int32_t value = signed6 * static_cast<std::int32_t>(
			sve_vector_length);
		if ((word & 0xff30'fc00U) == 0x0420'e000U) {
			const std::uint32_t elements = sve_vector_length >>
				((word >> 22) & 3U);
			value = static_cast<std::int32_t>(
				predicate_pattern_count(pattern, elements) *
				(((word >> 16) & 15U) + 1U));
		}
		return value >= 0 ?
			move_wide_zero | (static_cast<std::uint32_t>(value) << 5) | rd :
			move_wide_not | (static_cast<std::uint32_t>(~value) << 5) | rd;
	}
	if ((word & 0xffa0'f800U) == 0x0420'5000U) {
		const std::int32_t value = signed6 *
			(((word & 0x0040'0000U) != 0U) ?
				static_cast<std::int32_t>(sve_vector_length / 8U) :
				static_cast<std::int32_t>(sve_vector_length));
		const std::uint32_t rn = (word >> 16) & 31U;
		const std::uint32_t magnitude = static_cast<std::uint32_t>(
			value >= 0 ? value : -value);
		return (value >= 0 ? add_immediate : subtract_immediate) |
			(magnitude << 10) | (rn << 5) | rd;
	}
	/* INC and DEC (scalar): Rdn is both operand and destination. */
	const std::uint32_t elements = sve_vector_length >> ((word >> 22) & 3U);
	const std::uint32_t count = predicate_pattern_count(pattern, elements) *
		(((word >> 16) & 15U) + 1U);
	const std::uint32_t rdn = word & 31U;
	if (rdn == 31U || count == 0U)
		return sm0_vector_length_nop;
	return ((word & 0x0000'0400U) != 0U ? subtract_immediate :
					      add_immediate) |
		(count << 10) | (rdn << 5) | rdn;
}

static_assert(sm0_vector_length_equivalent(0x0420'e3e0U) == 0xd280'0200U,
	"CNTB X0 is MOVZ X0, #16");
static_assert(sm0_vector_length_equivalent(0x04e0'e3e0U) == 0xd280'0040U,
	"CNTD X0 is MOVZ X0, #2");
static_assert(sm0_vector_length_equivalent(0x04bf'5000U) == 0xd280'0000U,
	"RDVL X0, #0 is MOVZ X0, #0");
static_assert(sm0_vector_length_equivalent(0x04bf'57e0U) == 0x9280'01e0U,
	"RDVL X0, #-1 is MOVN X0, #15");
static_assert(sm0_vector_length_equivalent(0x0430'e3e0U) == 0x9100'4000U,
	"INCB X0 is ADD X0, X0, #16");
static_assert(sm0_vector_length_equivalent(0x0430'e7e1U) == 0xd100'4021U,
	"DECB X1 is SUB X1, X1, #16");
static_assert(sm0_vector_length_equivalent(0x0430'e3ffU) == sm0_vector_length_nop,
	"INCB XZR is a NOP");
static_assert(sm0_vector_length_equivalent(0x0461'5002U) == 0x9100'0022U,
	"ADDPL X2, X1, #0 is ADD X2, X1, #0");
static_assert(sm0_vector_length_equivalent(0x0421'57e2U) == 0xd100'4022U,
	"ADDVL X2, X1, #-1 is SUB X2, X1, #16");
static_assert(!sm0_vector_length_equivalent(0x0420'f3e0U),
	"SQINCB is not in the family");
static_assert(!sm0_vector_length_equivalent(0x04bf'5800U),
	"RDSVL is not in the family");

} // namespace uml::cpu::arm64::detail
