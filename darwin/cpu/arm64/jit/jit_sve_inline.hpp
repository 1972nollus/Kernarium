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
inline constexpr std::uint16_t fnmla_p = form_of(0x65a2'4020U);
inline constexpr std::uint16_t fnmls_p = form_of(0x65a2'6020U);
inline constexpr std::uint16_t fnmad_p = form_of(0x65a2'c020U);
inline constexpr std::uint16_t fnmsb_p = form_of(0x65a2'e020U);
inline constexpr std::uint16_t fmul_i = form_of(0x64aa'2020U);
inline constexpr std::uint16_t fabd_p = form_of(0x6588'8040U);
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
inline constexpr std::uint16_t fadda_p = form_of(0x6598'2020U);
inline constexpr std::uint16_t faddv_p = form_of(0x6580'2020U);
inline constexpr std::uint16_t fabs_p = form_of(0x049c'a020U);
inline constexpr std::uint16_t fneg_p = form_of(0x049d'a020U);
inline constexpr std::uint16_t scvtf_p = form_of(0x6552'a020U);
inline constexpr std::uint16_t ucvtf_p = form_of(0x6553'a020U);
inline constexpr std::uint16_t fcvtzs_p = form_of(0x655a'a020U);
inline constexpr std::uint16_t fcvtzu_p = form_of(0x655b'a020U);

/* Floating point by an immediate (0.5, 1.0, 2.0 or 0.0, by bit 5), and the
 * rest of the predicated arithmetic AdvSIMD has directly. */
inline constexpr std::uint16_t fadd_imm = form_of(0x6558'8000U);
inline constexpr std::uint16_t fsub_imm = form_of(0x6559'8000U);
inline constexpr std::uint16_t fmul_imm = form_of(0x655a'8000U);
inline constexpr std::uint16_t fsubr_imm = form_of(0x655b'8000U);
inline constexpr std::uint16_t fmaxnm_imm = form_of(0x655c'8000U);
inline constexpr std::uint16_t fminnm_imm = form_of(0x655d'8000U);
inline constexpr std::uint16_t fmax_imm = form_of(0x655e'8000U);
inline constexpr std::uint16_t fmin_imm = form_of(0x655f'8000U);
inline constexpr std::uint16_t fdiv_p = form_of(0x654d'8020U);
inline constexpr std::uint16_t fdivr_p = form_of(0x654c'8020U);
inline constexpr std::uint16_t fsqrt_p = form_of(0x654d'a020U);
inline constexpr std::uint16_t frint_p = form_of(0x6540'a020U);
inline constexpr std::uint16_t fcadd_p = form_of(0x6440'8020U);

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
inline constexpr std::uint16_t mul_u = form_of(0x0420'6020U);
inline constexpr std::uint16_t asr_v = form_of(0x0450'8020U);
inline constexpr std::uint16_t lsr_v = form_of(0x0451'8020U);
inline constexpr std::uint16_t lsl_v = form_of(0x0453'8020U);
inline constexpr std::uint16_t asrr_v = form_of(0x0454'8020U);
inline constexpr std::uint16_t lsrr_v = form_of(0x0455'8020U);
inline constexpr std::uint16_t lslr_v = form_of(0x0457'8020U);
inline constexpr std::uint16_t asr_pi = form_of(0x0400'83e0U);
inline constexpr std::uint16_t lsr_pi = form_of(0x0401'83e0U);
inline constexpr std::uint16_t lsl_pi = form_of(0x0403'8220U);
inline constexpr std::uint16_t asrd_pi = form_of(0x0404'83e0U);
inline constexpr std::uint16_t addp_p = form_of(0x4451'a020U);
inline constexpr std::uint16_t usqadd_p = form_of(0x445d'8020U);
inline constexpr std::uint16_t sabd_p = form_of(0x040c'0020U);
inline constexpr std::uint16_t uabd_p = form_of(0x040d'0020U);
inline constexpr std::uint16_t shadd_p = form_of(0x4410'8020U);
inline constexpr std::uint16_t uhadd_p = form_of(0x4411'8020U);
inline constexpr std::uint16_t shsub_p = form_of(0x4412'8020U);
inline constexpr std::uint16_t uhsub_p = form_of(0x4413'8020U);
inline constexpr std::uint16_t srhadd_p = form_of(0x4414'8020U);
inline constexpr std::uint16_t urhadd_p = form_of(0x4415'8020U);
inline constexpr std::uint16_t shsubr_p = form_of(0x4416'8020U);
inline constexpr std::uint16_t uhsubr_p = form_of(0x4417'8020U);
inline constexpr std::uint16_t shrnb = form_of(0x452d'1020U);
inline constexpr std::uint16_t shrnt = form_of(0x452d'1420U);
inline constexpr std::uint16_t rshrnb = form_of(0x452d'1820U);
inline constexpr std::uint16_t rshrnt = form_of(0x452d'1c20U);
inline constexpr std::uint16_t raddhnb = form_of(0x4562'6820U);
inline constexpr std::uint16_t raddhnt = form_of(0x4562'6c20U);
inline constexpr std::uint16_t subhnb = form_of(0x4562'7020U);
inline constexpr std::uint16_t subhnt = form_of(0x4562'7420U);
inline constexpr std::uint16_t rsubhnb = form_of(0x4562'7820U);
inline constexpr std::uint16_t rsubhnt = form_of(0x4562'7c20U);
/* Predicated merging unary integer operations: Zd, Pg/M, Zn. */
inline constexpr std::uint16_t sxt_p = form_of(0x0450'a440U);
inline constexpr std::uint16_t uxt_p = form_of(0x0451'a440U);
inline constexpr std::uint16_t abs_p = form_of(0x0496'a440U);
inline constexpr std::uint16_t neg_p = form_of(0x0497'a440U);
inline constexpr std::uint16_t cls_p = form_of(0x0498'a440U);
inline constexpr std::uint16_t clz_p = form_of(0x0499'a440U);
inline constexpr std::uint16_t cnt_p = form_of(0x049a'a440U);
inline constexpr std::uint16_t cnot_p = form_of(0x049b'a440U);
inline constexpr std::uint16_t not_p = form_of(0x049e'a440U);
inline constexpr std::uint16_t add_i = form_of(0x2520'c020U);
inline constexpr std::uint16_t sub_i = form_of(0x2521'c020U);
inline constexpr std::uint16_t subr_i = form_of(0x2523'c020U);
inline constexpr std::uint16_t uaddv_p = form_of(0x0401'2020U);
inline constexpr std::uint16_t sunpk = form_of(0x0570'3820U);
inline constexpr std::uint16_t uunpk = form_of(0x0572'3820U);
inline constexpr std::uint16_t punpk = form_of(0x0530'4020U);
inline constexpr std::uint16_t rev_v = form_of(0x0538'3820U);
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
inline constexpr std::uint16_t sdot_i = form_of(0x44a2'0020U);
inline constexpr std::uint16_t udot_i = form_of(0x44a2'0420U);

/* Floating-point compares into a predicate: two vectors (FCMGE, FCMGT,
 * FCMEQ, FCMNE, FCMUO), the absolute compares, and a vector against 0.0. */
inline constexpr std::uint16_t fcmp_vector = form_of(0x6583'4440U);
inline constexpr std::uint16_t facmp = form_of(0x6583'c450U);
inline constexpr std::uint16_t fcmp_zero = form_of(0x6590'2440U);

/* Predicate permutes. */
inline constexpr std::uint16_t zip_p = form_of(0x0522'4020U);
inline constexpr std::uint16_t uzp_p = form_of(0x0522'4820U);
inline constexpr std::uint16_t trn_p = form_of(0x0522'5020U);

/* Integer compares into a predicate: two vectors, or a vector and an
 * immediate. */
inline constexpr std::uint16_t cmp_vector = form_of(0x2483'a440U);
inline constexpr std::uint16_t cmp_immediate = form_of(0x2583'8440U);

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
