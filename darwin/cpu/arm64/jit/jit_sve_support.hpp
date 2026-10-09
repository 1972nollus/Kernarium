// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/sve_profile.hpp"

#include <cstdint>

namespace uml::cpu::arm64::jit {

/* How much of the non-streaming SVE profile the JIT implements: all of it.
 * An SM=0 translation lowers the forms below inline (implements_sve_form,
 * the control core) and sends every other row the virtual CPU implements to
 * the semantic library (jit_semantic_sve.cpp), so its guest sees the whole
 * profile (sve_host_for_support) whenever the policy enables it. */
inline constexpr SveSupport sve_support = SveSupport::complete;

namespace sve_forms {

/* A table form, named by a word that decodes to it, so a regenerated table
 * that renumbers its forms cannot misroute one. One form covers every row
 * of its encoding, such as each element size of CNT[BHWD]. */
[[nodiscard]] constexpr std::uint16_t form_of(std::uint32_t word) noexcept {
  return detail::find_sme_encoding(word)->form;
}

inline constexpr std::uint16_t ptrue = form_of(0x2518'e000U);
inline constexpr std::uint16_t ptrues = form_of(0x2519'e000U);
inline constexpr std::uint16_t pfalse = form_of(0x2518'e400U);
inline constexpr std::uint16_t ptest = form_of(0x2550'c000U);
/* The predicate logical operations, zeroing, and their flag-setting
 * forms; SEL. */
inline constexpr std::uint16_t and_p = form_of(0x2500'4000U);
inline constexpr std::uint16_t ands_p = form_of(0x2540'4000U);
inline constexpr std::uint16_t bic_p = form_of(0x2500'4010U);
inline constexpr std::uint16_t bics_p = form_of(0x2540'4010U);
inline constexpr std::uint16_t eor_p = form_of(0x2500'4200U);
inline constexpr std::uint16_t eors_p = form_of(0x2540'4200U);
inline constexpr std::uint16_t orr_p = form_of(0x2580'4000U);
inline constexpr std::uint16_t orrs_p = form_of(0x25c0'4000U);
inline constexpr std::uint16_t orn_p = form_of(0x2580'4010U);
inline constexpr std::uint16_t orns_p = form_of(0x25c0'4010U);
inline constexpr std::uint16_t nand_p = form_of(0x2580'4210U);
inline constexpr std::uint16_t nands_p = form_of(0x25c0'4210U);
inline constexpr std::uint16_t nor_p = form_of(0x2580'4200U);
inline constexpr std::uint16_t nors_p = form_of(0x25c0'4200U);
inline constexpr std::uint16_t sel_p = form_of(0x2500'4210U);
/* Predicate counts. */
inline constexpr std::uint16_t cntp = form_of(0x2520'8000U);
inline constexpr std::uint16_t incp_scalar = form_of(0x252c'8800U);
inline constexpr std::uint16_t decp_scalar = form_of(0x252d'8800U);
inline constexpr std::uint16_t incp_vector = form_of(0x256c'8000U);
inline constexpr std::uint16_t decp_vector = form_of(0x256d'8000U);
/* Element counts at the vector length. */
inline constexpr std::uint16_t cnt = form_of(0x0420'e000U);
inline constexpr std::uint16_t inc_scalar = form_of(0x0430'e000U);
inline constexpr std::uint16_t dec_scalar = form_of(0x0430'e400U);
inline constexpr std::uint16_t inc_vector = form_of(0x0470'c000U);
inline constexpr std::uint16_t dec_vector = form_of(0x0470'c400U);
inline constexpr std::uint16_t rdvl = form_of(0x04bf'5000U);
inline constexpr std::uint16_t addvl = form_of(0x0420'5000U);
inline constexpr std::uint16_t addpl = form_of(0x0460'5000U);
/* FFR. */
inline constexpr std::uint16_t setffr = form_of(0x252c'9000U);
inline constexpr std::uint16_t rdffr = form_of(0x2519'f000U);
inline constexpr std::uint16_t rdffr_predicated = form_of(0x2518'f000U);
inline constexpr std::uint16_t rdffrs = form_of(0x2558'f000U);
inline constexpr std::uint16_t wrffr = form_of(0x2528'9000U);
/* Moves: MOVPRFX, DUP, DUPM, FDUP, CPY, FCPY, SEL and the unpredicated
 * logical forms, which cover every MOV alias of a vector. */
inline constexpr std::uint16_t movprfx = form_of(0x0420'bc00U);
inline constexpr std::uint16_t movprfx_predicated = form_of(0x0410'2000U);
inline constexpr std::uint16_t dup_scalar = form_of(0x0520'3800U);
inline constexpr std::uint16_t dup_immediate = form_of(0x2538'c000U);
inline constexpr std::uint16_t dup_indexed = form_of(0x0521'2000U);
inline constexpr std::uint16_t dupm = form_of(0x05c0'0000U);
/* The bitmask-immediate logical forms, which share DUPM's reserved
 * immediates (sve_word_reserved). */
inline constexpr std::uint16_t orr_immediate = form_of(0x0500'0000U);
inline constexpr std::uint16_t eor_immediate = form_of(0x0540'0000U);
inline constexpr std::uint16_t and_immediate = form_of(0x0580'0000U);
inline constexpr std::uint16_t fdup = form_of(0x2579'c000U);
inline constexpr std::uint16_t cpy_merging = form_of(0x0510'4000U);
inline constexpr std::uint16_t cpy_zeroing = form_of(0x0510'0000U);
inline constexpr std::uint16_t cpy_scalar = form_of(0x0528'a000U);
inline constexpr std::uint16_t cpy_simd = form_of(0x0520'8000U);
inline constexpr std::uint16_t fcpy = form_of(0x0550'c000U);
inline constexpr std::uint16_t sel_z = form_of(0x0520'c000U);
inline constexpr std::uint16_t and_z = form_of(0x0420'3000U);
inline constexpr std::uint16_t orr_z = form_of(0x0460'3000U);
inline constexpr std::uint16_t eor_z = form_of(0x04a0'3000U);
inline constexpr std::uint16_t bic_z = form_of(0x04e0'3000U);
/* WHILE* on two general registers, and the conflict forms. */
inline constexpr std::uint16_t whilelt = form_of(0x2520'0400U);
inline constexpr std::uint16_t whilele = form_of(0x2520'0410U);
inline constexpr std::uint16_t whilelo = form_of(0x2520'0c00U);
inline constexpr std::uint16_t whilels = form_of(0x2520'0c10U);
inline constexpr std::uint16_t whilegt = form_of(0x2520'0010U);
inline constexpr std::uint16_t whilege = form_of(0x2520'0000U);
inline constexpr std::uint16_t whilehi = form_of(0x2520'0810U);
inline constexpr std::uint16_t whilehs = form_of(0x2520'0800U);
inline constexpr std::uint16_t whilewr = form_of(0x2520'3000U);
inline constexpr std::uint16_t whilerw = form_of(0x2520'3010U);

} // namespace sve_forms

/* The forms an SM=0 translation lowers inline: the control core. Every
 * other row the non-streaming profile implements runs through the
 * semantic library, and a reserved encoding (sve_word_reserved) is the
 * illegal-instruction exit. None of these forms can raise a
 * floating-point exception. */
[[nodiscard]] constexpr bool implements_sve_form(std::uint16_t form) noexcept {
  using namespace sve_forms;
  for (const auto implemented :
       {ptrue,       ptrues,       pfalse,      ptest,
        and_p,       ands_p,       bic_p,       bics_p,
        eor_p,       eors_p,       orr_p,       orrs_p,
        orn_p,       orns_p,       nand_p,      nands_p,
        nor_p,       nors_p,       sel_p,       cntp,
        incp_scalar, decp_scalar,  incp_vector, decp_vector,
        cnt,         inc_scalar,   dec_scalar,  inc_vector,
        dec_vector,  rdvl,         addvl,       addpl,
        setffr,      rdffr,        rdffr_predicated, rdffrs,
        wrffr,       movprfx,      movprfx_predicated, dup_scalar,
        dup_immediate, dup_indexed, dupm,       fdup,
        cpy_merging, cpy_zeroing,  cpy_scalar,  cpy_simd,
        fcpy,        sel_z,        and_z,       orr_z,
        eor_z,       bic_z,        whilelt,     whilele,
        whilelo,     whilels,      whilegt,     whilege,
        whilehi,     whilehs,      whilewr,     whilerw})
    if (form == implemented)
      return true;
  return false;
}

/* Whether `word`, of table form `form`, is UNDEFINED although the table
 * allocates its row: a reserved bitmask immediate (AND, ORR, EOR and DUPM
 * share the decoder), a shifted byte immediate of DUP or CPY, or a
 * byte-sized FDUP, FCPY or vector INC and DEC. Both the translation and
 * the executor's trapped-exception mode ask. */
[[nodiscard]] inline bool sve_word_reserved(std::uint16_t form,
                                            std::uint32_t word) noexcept {
  using namespace sve_forms;
  const unsigned size = (word >> 22) & 3U;
  /* TODO(SVE V5): call detail::sve_reserved_encoding, the shared test the
   * gadget engine's lane is adding to the library, once it is on main;
   * this is the temporary local form of its bitmask part (the immediate
   * decoder is the shared primitive). */
  if (form == dupm || form == orr_immediate || form == eor_immediate ||
      form == and_immediate)
    return !detail::decode_logical_immediate(true, (word >> 17) & 1U,
                                             (word >> 11) & 0x3fU,
                                             (word >> 5) & 0x3fU)
                .has_value();
  if (form == dup_immediate || form == cpy_merging || form == cpy_zeroing)
    return size == 0U && ((word >> 13) & 1U) != 0U;
  if (form == fdup || form == fcpy || form == inc_vector ||
      form == dec_vector || form == incp_vector || form == decp_vector)
    return size == 0U;
  return false;
}

static_assert(sve_forms::cnt == sve_forms::form_of(0x04e0'e000U));
static_assert(sve_forms::inc_vector == sve_forms::form_of(0x04f0'c000U));
static_assert(sve_forms::incp_vector == sve_forms::form_of(0x25ac'8000U));
static_assert(sve_forms::cpy_merging != sve_forms::cpy_zeroing);
static_assert(implements_sve_form(sve_forms::whilerw) &&
              !implements_sve_form(sve_forms::form_of(0x2558'c000U)));

} // namespace uml::cpu::arm64::jit
