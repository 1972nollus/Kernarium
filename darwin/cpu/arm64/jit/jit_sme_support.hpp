// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/sme_profile.hpp"

#include <cstdint>

namespace uml::cpu::arm64::jit {

/* How much of the SME profile the JIT implements (design 5.3, S4): the
 * control core (M1) and the register-only forms (M2). SMSTART, SMSTOP, MRS
 * and MSR SVCR and TPIDR2_EL0 are base-space system instructions; the
 * table rows are those implements_sme_row() names, less the reserved
 * words sme_word_reserved() finds. Every other SVE and SME word, every
 * AdvSIMD word and every SIMD&FP load or store in an SM=1 translation is
 * the illegal-instruction exit. */
inline constexpr SmeSupport sme_support = SmeSupport::control;

namespace sme_forms {

/* A table form, named by a word that decodes to it, so a regenerated table
 * that renumbers its forms cannot misroute one. */
[[nodiscard]] constexpr std::uint16_t form_of(std::uint32_t word) noexcept {
  return detail::find_sme_encoding(word)->form;
}

inline constexpr std::uint16_t rdsvl = form_of(0x04bf'5800U);
inline constexpr std::uint16_t addsvl = form_of(0x0420'5800U);
inline constexpr std::uint16_t addspl = form_of(0x0460'5800U);
inline constexpr std::uint16_t ldr_za = form_of(0xe100'0000U);
inline constexpr std::uint16_t str_za = form_of(0xe120'0000U);
inline constexpr std::uint16_t ldr_zt0 = form_of(0xe11f'8000U);
inline constexpr std::uint16_t str_zt0 = form_of(0xe13f'8000U);
inline constexpr std::uint16_t zero_za = form_of(0xc008'0000U);
inline constexpr std::uint16_t zero_zt0 = form_of(0xc048'0001U);
/* Register-only forms with a rule of their own. */
inline constexpr std::uint16_t movprfx = form_of(0x0420'bc00U);
inline constexpr std::uint16_t movprfx_predicated = form_of(0x0410'2000U);
inline constexpr std::uint16_t ptest = form_of(0x2550'c000U);
/* CTERMEQ and CTERMNE, one form. */
inline constexpr std::uint16_t cterm = form_of(0x25a0'2000U);
/* The bitmask-immediate forms: ORR, EOR and AND (immediate) and DUPM. */
inline constexpr std::uint16_t orr_immediate = form_of(0x0500'0000U);
inline constexpr std::uint16_t eor_immediate = form_of(0x0540'0000U);
inline constexpr std::uint16_t and_immediate = form_of(0x0580'0000U);
inline constexpr std::uint16_t dupm = form_of(0x05c0'0000U);

} // namespace sme_forms

/* The control core's forms, which have emitters of their own; the form's
 * own mode checks (detail::sme_mode_permits) still apply. */
[[nodiscard]] constexpr bool implements_sme_form(std::uint16_t form) noexcept {
  using namespace sme_forms;
  return form == rdsvl || form == addsvl || form == addspl ||
         form == ldr_za || form == str_za || form == ldr_zt0 ||
         form == str_zt0 || form == zero_za || form == zero_zt0;
}

/* Whether the JIT translates a row: the control core's forms, and every
 * register-only row of the SME1 profile. SME2's rows (S6b) and the memory
 * forms (M3) are not yet translated. */
[[nodiscard]] constexpr bool implements_sme_row(
    const detail::SmeEncoding &row) noexcept {
  return implements_sme_form(row.form) ||
         (row.memory == 0U &&
          (row.requirement & detail::sme_feature_sme2) == 0U);
}

/* Whether a word of a row the JIT implements is nevertheless UNDEFINED: a
 * bitmask immediate (N:immr:imms, bits 17:5) DecodeBitMasks reserves. The
 * table's rows include those values, and the hardware would raise SIGILL
 * in the host on them, so they are the illegal exit. */
[[nodiscard]] constexpr bool sme_word_reserved(
    const detail::SmeEncoding &row, std::uint32_t word) noexcept {
  using namespace sme_forms;
  if (row.form != orr_immediate && row.form != eor_immediate &&
      row.form != and_immediate && row.form != dupm)
    return false;
  return !detail::decode_logical_immediate(true, (word >> 17) & 1U,
                                           (word >> 11) & 63U,
                                           (word >> 5) & 63U);
}

/* Whether an SVE row's words can raise a floating-point exception: the SVE
 * floating-point groups (bits 31:25 = 0110010). The other SVE words named
 * for floating point (FABS, FNEG, FCPY, FDUP) raise none, and ZA's
 * floating-point forms raise none either (arm64-jit-sme-codegen checks
 * this against the evidence table's form names). */
[[nodiscard]] constexpr bool sve_raises_fp_exceptions(
    std::uint32_t word) noexcept {
  return (word & 0xfe00'0000U) == 0x6400'0000U;
}

/* Whether a row runs with the guest's NZCV in the host's and leaves its
 * result there: the forms that set flags write a predicate, apart from
 * PTEST and CTERMEQ/CTERMNE, which read them too. A predicate-writing form
 * that sets no flags leaves them as they were. */
[[nodiscard]] constexpr bool sme_row_uses_flags(
    const detail::SmeEncoding &row, bool predicate_destination) noexcept {
  using namespace sme_forms;
  return predicate_destination || row.form == ptest || row.form == cterm;
}

static_assert(sme_forms::ldr_za != sme_forms::str_za);
static_assert(sme_forms::ldr_zt0 != sme_forms::zero_zt0);
static_assert(sme_forms::cterm == sme_forms::form_of(0x25a0'2010U));
static_assert(sme_forms::movprfx != sme_forms::movprfx_predicated);
/* ORR Z0.S, Z0.S, #1 and the reserved N=0, imms=111111 of each form. */
static_assert(!sme_word_reserved(*detail::find_sme_encoding(0x0500'0000U),
                                 0x0500'0000U));
static_assert(sme_word_reserved(*detail::find_sme_encoding(0x0500'07e0U),
                                0x0500'07e0U));
static_assert(sme_word_reserved(*detail::find_sme_encoding(0x05c0'07e0U),
                                0x05c0'07e0U));

} // namespace uml::cpu::arm64::jit
