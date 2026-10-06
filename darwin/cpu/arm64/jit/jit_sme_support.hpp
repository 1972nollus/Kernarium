// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/sme_profile.hpp"

#include <cstdint>

namespace uml::cpu::arm64::jit {

/* How much of the SME profile the JIT implements (design 5.3, S4): the
 * control core (M1), the register-only forms (M2) and the SVE and SME1
 * memory forms (M3). SMSTART, SMSTOP, MRS and MSR SVCR and TPIDR2_EL0 are
 * base-space system instructions; the table rows are those
 * implements_sme_row() names, less the reserved words sme_word_reserved()
 * finds. Every other SVE and SME word, every AdvSIMD word and every
 * SIMD&FP load or store in an SM=1 translation is the illegal-instruction
 * exit. */
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

/* Whether the JIT runs a memory shape in streaming mode: every shape of
 * the SME1 profile but SME2's multi-vector and strided forms (S6b). LDR
 * and STR of ZA and ZT0 are the control core's forms. */
[[nodiscard]] constexpr bool implements_sme_memory(
    const detail::SmeMemory &memory) noexcept {
  using Shape = detail::SmeMemoryShape;
  switch (memory.shape) {
  case Shape::contiguous:
    return memory.registers == 1U;
  case Shape::structure:
  case Shape::replicate:
  case Shape::replicate_quad:
  case Shape::fill_z:
  case Shape::fill_p:
  case Shape::za_slice:
  case Shape::prefetch:
    return true;
  case Shape::strided:
  case Shape::za_vector:
  case Shape::zt0:
    break;
  }
  return false;
}

/* Whether the JIT translates a row: the control core's forms, and every
 * register-only and memory row of the SME1 profile whose shape
 * implements_sme_memory() names. SME2's rows (S6b) are not yet
 * translated. */
[[nodiscard]] constexpr bool implements_sme_row(
    const detail::SmeEncoding &row) noexcept {
  if (implements_sme_form(row.form))
    return true;
  if ((row.requirement & detail::sme_feature_sme2) != 0U)
    return false;
  return row.memory == 0U ||
         implements_sme_memory(*detail::sme_encoding_memory_of(row));
}

/* A memory form's element size in bytes for one word: ST1B's and its
 * kin's is an instruction field. */
[[nodiscard]] constexpr unsigned sme_memory_element_bytes(
    const detail::SmeMemory &memory, std::uint32_t word) noexcept {
  const unsigned shift =
      memory.esize_shift.width != 0U
          ? detail::sme_segment_value(memory.esize_shift, word)
          : 0U;
  return (unsigned{memory.esize} << shift) / 8U;
}

/* A memory form's immediate, signed where the form's is. */
[[nodiscard]] constexpr std::int64_t sme_memory_immediate(
    const detail::SmeMemory &memory, std::uint32_t word) noexcept {
  std::uint64_t value = 0;
  unsigned width = 0;
  for (unsigned index = 0; index < memory.immediate_segment_count; ++index) {
    const auto &segment = memory.immediate[index];
    value = (value << segment.width) | detail::sme_segment_value(segment, word);
    width += segment.width;
  }
  if (memory.immediate_signed && width != 0U &&
      ((value >> (width - 1U)) & 1U) != 0U)
    value -= std::uint64_t{1} << width;
  return static_cast<std::int64_t>(value);
}

/* The bytes a memory form's access spans at an SVL, whichever elements are
 * active: the range the same-page check covers, and the unit of a "MUL VL"
 * immediate. Never more than 4 * SVL, below every guest page. */
[[nodiscard]] constexpr std::uint64_t sme_memory_span(
    const detail::SmeMemory &memory, std::uint32_t word,
    std::uint64_t svl) noexcept {
  using Shape = detail::SmeMemoryShape;
  switch (memory.shape) {
  case Shape::contiguous:
  case Shape::structure:
    return svl * memory.registers * (memory.msize / 8U) /
           sme_memory_element_bytes(memory, word);
  case Shape::replicate:
    return memory.msize / 8U;
  case Shape::replicate_quad:
    return 16;
  case Shape::fill_z:
  case Shape::za_slice:
    return svl;
  case Shape::fill_p:
    return svl / 8U;
  default:
    return 0;
  }
}

/* A memory form's offset from its base, for an index register's value. */
[[nodiscard]] constexpr std::uint64_t sme_memory_offset(
    const detail::SmeMemory &memory, std::uint32_t word, std::uint64_t svl,
    std::uint64_t index) noexcept {
  const auto immediate =
      static_cast<std::uint64_t>(sme_memory_immediate(memory, word));
  switch (memory.offset) {
  case detail::SmeAddressOffset::immediate_footprint:
    return immediate * sme_memory_span(memory, word, svl);
  case detail::SmeAddressOffset::register_element:
    return index * (memory.msize / 8U);
  case detail::SmeAddressOffset::immediate_element:
    return immediate * (memory.msize / 8U);
  case detail::SmeAddressOffset::immediate_16:
    return immediate * 16U;
  case detail::SmeAddressOffset::none:
    break;
  }
  return 0;
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
/* LD3H {Z0.H-Z2.H}, P0/Z, [X0, #-9, MUL VL] at SVL 64: 3 * 64 bytes back
 * for each MUL VL; LD1SB {Z0.D}, P0/Z, [X0, #1, MUL VL]: 8 bytes. */
static_assert(sme_memory_span(*detail::sme_encoding_memory_of(
                                  *detail::find_sme_encoding(0xa4cd'e000U)),
                              0xa4cd'e000U, 64) == 192U);
static_assert(sme_memory_offset(*detail::sme_encoding_memory_of(
                                    *detail::find_sme_encoding(0xa4cd'e000U)),
                                0xa4cd'e000U, 64, 0) ==
              static_cast<std::uint64_t>(-576));
static_assert(sme_memory_span(*detail::sme_encoding_memory_of(
                                  *detail::find_sme_encoding(0xa581'a000U)),
                              0xa581'a000U, 64) == 8U);
/* ORR Z0.S, Z0.S, #1 and the reserved N=0, imms=111111 of each form. */
static_assert(!sme_word_reserved(*detail::find_sme_encoding(0x0500'0000U),
                                 0x0500'0000U));
static_assert(sme_word_reserved(*detail::find_sme_encoding(0x0500'07e0U),
                                0x0500'07e0U));
static_assert(sme_word_reserved(*detail::find_sme_encoding(0x05c0'07e0U),
                                0x05c0'07e0U));

} // namespace uml::cpu::arm64::jit
