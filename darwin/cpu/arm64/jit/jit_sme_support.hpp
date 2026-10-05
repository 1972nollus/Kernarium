// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/sme_profile.hpp"

#include <cstdint>

namespace uml::cpu::arm64::jit {

/* How much of the SME profile the JIT implements: the control core
 * (design 5.3, S4 milestone M1). SMSTART, SMSTOP, MRS and MSR SVCR and
 * TPIDR2_EL0 are base-space system instructions; the table forms are those
 * implements_sme_form() names. Every other SVE and SME word, and every
 * FP&SIMD word in an SM=1 translation, is the illegal-instruction exit. */
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

} // namespace sme_forms

/* Whether the JIT translates a table form at all; the form's own mode
 * checks (detail::sme_mode_permits) still apply. */
[[nodiscard]] constexpr bool implements_sme_form(std::uint16_t form) noexcept {
  using namespace sme_forms;
  return form == rdsvl || form == addsvl || form == addspl ||
         form == ldr_za || form == str_za || form == ldr_zt0 ||
         form == str_zt0 || form == zero_za || form == zero_zt0;
}

static_assert(sme_forms::ldr_za != sme_forms::str_za);
static_assert(sme_forms::ldr_zt0 != sme_forms::zero_zt0);

} // namespace uml::cpu::arm64::jit
