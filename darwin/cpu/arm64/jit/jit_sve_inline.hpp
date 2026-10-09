// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_sve_support.hpp"

#include "cpu/arm64/detail/sme_decode.hpp"

#include <cstdint>

namespace uml::cpu::arm64::jit {

/*
 * The forms an SM=0 translation lowers inline onto NEON beyond the control
 * core (design 4.3, SVE lane V4 M2): the hot set of the form census, the
 * contiguous memory forms and their structure, replicate and fill kin.
 * Every other row runs through the semantic library, and so does any word
 * of a form below that the lowering declines (an element size NEON lacks,
 * or a feature the host lacks); jit_compiler_sve_inline.cpp decides per
 * word. The lowering is the library's semantics by construction and by
 * arm64-jit-sve-forms, which runs every row against it.
 */
namespace sve_inline_forms {

using sve_forms::form_of;

/* Floating point. P: predicated, merging (Pg/M); U: unpredicated; I: by
 * element. */
inline constexpr std::uint16_t fmla_p = form_of(0x65a2'0020U);
inline constexpr std::uint16_t fmls_p = form_of(0x65a2'2020U);
inline constexpr std::uint16_t fmad_p = form_of(0x65a2'8020U);
inline constexpr std::uint16_t fmsb_p = form_of(0x65a2'a020U);
inline constexpr std::uint16_t fmla_i = form_of(0x64aa'0020U);
inline constexpr std::uint16_t fmls_i = form_of(0x64aa'0420U);
inline constexpr std::uint16_t fadd_u = form_of(0x6582'0020U);
inline constexpr std::uint16_t fsub_u = form_of(0x6582'0420U);
inline constexpr std::uint16_t fmul_u = form_of(0x6582'0820U);
inline constexpr std::uint16_t fadd_p = form_of(0x6580'8020U);
inline constexpr std::uint16_t fsub_p = form_of(0x6581'8020U);
inline constexpr std::uint16_t fsubr_p = form_of(0x6583'8020U);
inline constexpr std::uint16_t fmul_p = form_of(0x6582'8020U);
inline constexpr std::uint16_t fmaxnm_p = form_of(0x6584'8020U);
inline constexpr std::uint16_t fminnm_p = form_of(0x6585'8020U);
inline constexpr std::uint16_t fmax_p = form_of(0x6586'8020U);
inline constexpr std::uint16_t fmin_p = form_of(0x6587'8020U);
inline constexpr std::uint16_t fabs_p = form_of(0x049c'a020U);
inline constexpr std::uint16_t fneg_p = form_of(0x049d'a020U);
inline constexpr std::uint16_t scvtf_p = form_of(0x6552'a020U);
inline constexpr std::uint16_t ucvtf_p = form_of(0x6553'a020U);
inline constexpr std::uint16_t fcvtzs_p = form_of(0x655a'a020U);
inline constexpr std::uint16_t fcvtzu_p = form_of(0x655b'a020U);

/* Integer. */
inline constexpr std::uint16_t add_u = form_of(0x0422'0020U);
inline constexpr std::uint16_t sub_u = form_of(0x0422'0420U);
inline constexpr std::uint16_t uqadd_u = form_of(0x0422'1420U);
inline constexpr std::uint16_t uqsub_u = form_of(0x0422'1c20U);
inline constexpr std::uint16_t add_p = form_of(0x0400'0020U);
inline constexpr std::uint16_t sub_p = form_of(0x0401'0020U);
inline constexpr std::uint16_t subr_p = form_of(0x0403'0020U);
inline constexpr std::uint16_t mul_p = form_of(0x0410'0020U);
inline constexpr std::uint16_t mla_p = form_of(0x0402'4020U);
inline constexpr std::uint16_t mls_p = form_of(0x0402'6020U);
inline constexpr std::uint16_t mad_p = form_of(0x0401'c040U);
inline constexpr std::uint16_t msb_p = form_of(0x0401'e040U);
inline constexpr std::uint16_t smax_p = form_of(0x0408'0020U);
inline constexpr std::uint16_t umax_p = form_of(0x0409'0020U);
inline constexpr std::uint16_t smin_p = form_of(0x040a'0020U);
inline constexpr std::uint16_t umin_p = form_of(0x040b'0020U);
inline constexpr std::uint16_t orr_zp = form_of(0x0418'0020U);
inline constexpr std::uint16_t eor_zp = form_of(0x0419'0020U);
inline constexpr std::uint16_t and_zp = form_of(0x041a'0020U);
inline constexpr std::uint16_t bic_zp = form_of(0x041b'0020U);
inline constexpr std::uint16_t lsl_i = form_of(0x0429'9c20U);
inline constexpr std::uint16_t lsr_i = form_of(0x042f'9420U);
inline constexpr std::uint16_t asr_i = form_of(0x042f'9020U);
inline constexpr std::uint16_t sri = form_of(0x450f'f020U);
inline constexpr std::uint16_t usra = form_of(0x450f'e420U);
inline constexpr std::uint16_t ssra = form_of(0x450f'e020U);
inline constexpr std::uint16_t uqshl_p = form_of(0x0407'8120U);
inline constexpr std::uint16_t urshr_p = form_of(0x040d'81e0U);
inline constexpr std::uint16_t xar = form_of(0x042f'3420U);
inline constexpr std::uint16_t eor3 = form_of(0x0421'3840U);
inline constexpr std::uint16_t bcax = form_of(0x0461'3840U);
inline constexpr std::uint16_t bsl = form_of(0x0421'3c40U);
inline constexpr std::uint16_t umulh = form_of(0x0422'6c20U);
inline constexpr std::uint16_t smulh = form_of(0x0422'6820U);
inline constexpr std::uint16_t smullb = form_of(0x4542'7020U);
inline constexpr std::uint16_t smullt = form_of(0x4542'7420U);
inline constexpr std::uint16_t umullb = form_of(0x4542'7820U);
inline constexpr std::uint16_t umullt = form_of(0x4542'7c20U);
inline constexpr std::uint16_t smlalb = form_of(0x4442'4020U);
inline constexpr std::uint16_t smlalt = form_of(0x4442'4420U);
inline constexpr std::uint16_t umlalb = form_of(0x4442'4820U);
inline constexpr std::uint16_t umlalt = form_of(0x4442'4c20U);
inline constexpr std::uint16_t saddlb = form_of(0x4542'0020U);
inline constexpr std::uint16_t saddlt = form_of(0x4542'0420U);
inline constexpr std::uint16_t uaddlb = form_of(0x4542'0820U);
inline constexpr std::uint16_t uaddlt = form_of(0x4542'0c20U);
inline constexpr std::uint16_t addhnb = form_of(0x4562'6020U);
inline constexpr std::uint16_t addhnt = form_of(0x4562'6420U);
inline constexpr std::uint16_t uqshrnb = form_of(0x452f'3020U);
inline constexpr std::uint16_t uqshrnt = form_of(0x452f'3420U);
inline constexpr std::uint16_t sdot = form_of(0x4482'0020U);
inline constexpr std::uint16_t udot = form_of(0x4482'0420U);

/* Permutes and index. */
inline constexpr std::uint16_t zip = form_of(0x0522'6020U);
inline constexpr std::uint16_t uzp = form_of(0x0522'6820U);
inline constexpr std::uint16_t trn = form_of(0x0522'7020U);
inline constexpr std::uint16_t tbl = form_of(0x0522'3020U);
inline constexpr std::uint16_t rev = form_of(0x0564'8020U);
inline constexpr std::uint16_t ext = form_of(0x0520'0420U);
inline constexpr std::uint16_t index_ii = form_of(0x04a2'4020U);
inline constexpr std::uint16_t index_ir = form_of(0x04a1'4820U);
inline constexpr std::uint16_t index_ri = form_of(0x04a1'4420U);
inline constexpr std::uint16_t index_rr = form_of(0x04a2'4c20U);

} // namespace sve_inline_forms

} // namespace uml::cpu::arm64::jit
