// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

/*
 * The A64 encodings the non-streaming SVE lowering writes (jit_compiler_sve
 * and jit_compiler_sve_inline): general-register data processing and
 * AdvSIMD, 16 bytes. Host scratch: v0-v3 are the vector bindings'
 * fallbacks, v4 and v5 intermediate values; x8-x11 and x16 general.
 */
namespace uml::cpu::arm64::jit::encoders {

constexpr unsigned scratch_a = 4U;
constexpr unsigned scratch_b = 5U;

[[nodiscard]] constexpr unsigned field(std::uint32_t word, unsigned low,
                                       unsigned width) noexcept {
  return (word >> low) & ((1U << width) - 1U);
}

/* Condition codes. */
constexpr unsigned cond_eq = 0U;
constexpr unsigned cond_ne = 1U;
constexpr unsigned cond_hs = 2U;
constexpr unsigned cond_lo = 3U;
constexpr unsigned cond_pl = 5U;
constexpr unsigned cond_hi = 8U;
constexpr unsigned cond_ls = 9U;
constexpr unsigned cond_ge = 10U;
constexpr unsigned cond_lt = 11U;
constexpr unsigned cond_gt = 12U;
constexpr unsigned cond_le = 13U;

/* General-register data processing, 64-bit unless named W. */
[[nodiscard]] constexpr std::uint32_t three(std::uint32_t opcode, unsigned d,
                                            unsigned n, unsigned m) noexcept {
  return opcode | (m << 16) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t and_x(unsigned d, unsigned n,
                                            unsigned m) noexcept {
  return three(0x8a00'0000U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t eor_x(unsigned d, unsigned n,
                                            unsigned m) noexcept {
  return three(0xca00'0000U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t sub_x(unsigned d, unsigned n,
                                            unsigned m) noexcept {
  return three(0xcb00'0000U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t subs_x(unsigned d, unsigned n,
                                             unsigned m) noexcept {
  return three(0xeb00'0000U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t add_x(unsigned d, unsigned n,
                                            unsigned m) noexcept {
  return three(0x8b00'0000U, d, n, m);
}
/* CMP Xn, Xm and TST Xn, Xm. */
[[nodiscard]] constexpr std::uint32_t cmp_x(unsigned n, unsigned m) noexcept {
  return subs_x(31U, n, m);
}
[[nodiscard]] constexpr std::uint32_t tst_x(unsigned n, unsigned m) noexcept {
  return three(0xea00'0000U, 31U, n, m);
}
/* CMP Xn, #imm12 and CMN Wn, #imm12; Xn is never XZR here. */
[[nodiscard]] constexpr std::uint32_t cmp_immediate(unsigned n,
                                                    unsigned immediate) noexcept {
  return 0xf100'001fU | (immediate << 10) | (n << 5);
}
[[nodiscard]] constexpr std::uint32_t cmn_immediate(unsigned n, bool wide,
                                                    unsigned immediate) noexcept {
  return (wide ? 0xb100'001fU : 0x3100'001fU) | (immediate << 10) | (n << 5);
}
[[nodiscard]] constexpr std::uint32_t csel_x(unsigned d, unsigned n,
                                             unsigned m,
                                             unsigned condition) noexcept {
  return three(0x9a80'0000U, d, n, m) | (condition << 12);
}
[[nodiscard]] constexpr std::uint32_t csinc_x(unsigned d, unsigned n,
                                              unsigned m,
                                              unsigned condition) noexcept {
  return three(0x9a80'0400U, d, n, m) | (condition << 12);
}
/* CSET Wd, condition. */
[[nodiscard]] constexpr std::uint32_t cset_w(unsigned d,
                                             unsigned condition) noexcept {
  return 0x1a9f'07e0U | ((condition ^ 1U) << 12) | d;
}
/* ORR Wd, Wn, Wm, LSL #shift. */
[[nodiscard]] constexpr std::uint32_t orr_w_lsl(unsigned d, unsigned n,
                                                unsigned m,
                                                unsigned shift) noexcept {
  return three(0x2a00'0000U, d, n, m) | (shift << 10);
}
/* LSL Wd, Wn, #shift and LSR Xd, Xn, #shift. */
[[nodiscard]] constexpr std::uint32_t lsl_w(unsigned d, unsigned n,
                                            unsigned shift) noexcept {
  return 0x5300'0000U | (((32U - shift) & 31U) << 16) | ((31U - shift) << 10) |
         (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t lsr_x(unsigned d, unsigned n,
                                            unsigned shift) noexcept {
  return 0xd340'fc00U | (shift << 16) | (n << 5) | d;
}
/* SXTW Xd, Wn and MOV Wd, Wn (UXTW). */
[[nodiscard]] constexpr std::uint32_t sxtw(unsigned d, unsigned n) noexcept {
  return 0x9340'7c00U | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t uxtw(unsigned d, unsigned n) noexcept {
  return 0x2a00'03e0U | (n << 16) | d;
}
/* ADD Xd, Xn, Wm, SXTW #shift and ADD Xd, Xn, Wm, UXTW #shift. */
[[nodiscard]] constexpr std::uint32_t add_extended_word(
    bool is_signed, unsigned d, unsigned n, unsigned m,
    unsigned shift) noexcept {
  return 0x8b20'0000U | (m << 16) | ((is_signed ? 6U : 2U) << 13) |
         (shift << 10) | (n << 5) | d;
}
constexpr std::uint32_t msr_nzcv_x11 = 0xd51b'420bU;

/* AdvSIMD, 16 bytes. */
constexpr std::uint32_t vector_and = 0x4e20'1c00U;
constexpr std::uint32_t vector_bic = 0x4e60'1c00U;
constexpr std::uint32_t vector_orr = 0x4ea0'1c00U;
constexpr std::uint32_t vector_orn = 0x4ee0'1c00U;
constexpr std::uint32_t vector_eor = 0x6e20'1c00U;
constexpr std::uint32_t vector_bit = 0x6ea0'1c00U;
constexpr std::uint32_t vector_bif = 0x6ee0'1c00U;
/* ADD and SUB (vector) with SVE's size field. */
[[nodiscard]] constexpr std::uint32_t vector_add(bool subtract, unsigned size,
                                                 unsigned d, unsigned n,
                                                 unsigned m) noexcept {
  return three(subtract ? 0x6e20'8400U : 0x4e20'8400U, d, n, m) | (size << 22);
}
[[nodiscard]] constexpr std::uint32_t vector_move(unsigned d,
                                                  unsigned n) noexcept {
  return three(vector_orr, d, n, n);
}
/* DUP Vd.T, Rn and DUP Vd.T, Vn.T[index], with SVE's size field. */
[[nodiscard]] constexpr std::uint32_t dup_general(unsigned size, unsigned d,
                                                  unsigned n) noexcept {
  return 0x4e00'0c00U | ((1U << size) << 16) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t dup_element(unsigned size,
                                                  unsigned index, unsigned d,
                                                  unsigned n) noexcept {
  return 0x4e00'0400U | (((index << (size + 1U)) | (1U << size)) << 16) |
         (n << 5) | d;
}
/* MOVI Vd.2D or Dd with a mask of whole bytes. */
[[nodiscard]] constexpr std::uint32_t movi_bytes(bool both, unsigned d,
                                                 unsigned bytes) noexcept {
  return (both ? 0x6f00'e400U : 0x2f00'e400U) | ((bytes >> 5) << 16) |
         ((bytes & 31U) << 5) | d;
}
/* FMOV Dd, Xn and INS Vd.D[1], Xn. */
[[nodiscard]] constexpr std::uint32_t fmov_d_x(unsigned d,
                                               unsigned n) noexcept {
  return 0x9e67'0000U | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t ins_high(unsigned d,
                                               unsigned n) noexcept {
  return 0x4e18'1c00U | (n << 5) | d;
}

/* AdvSIMD, 16 bytes, with SVE's element size (0 byte to 3 doubleword)
 * where the instruction has one. */
[[nodiscard]] constexpr std::uint32_t sized(std::uint32_t opcode,
                                            unsigned size) noexcept {
  return opcode | (size << 22);
}
constexpr std::uint32_t vector_cmhi = 0x6e20'3400U;
constexpr std::uint32_t vector_cmeq = 0x6e20'8c00U;
constexpr std::uint32_t vector_umin = 0x6e20'6c00U;
/* UMINV Bd, Vn.16B and UMOV Wd, Vn.B[0]. */
[[nodiscard]] constexpr std::uint32_t uminv_b(unsigned d,
                                              unsigned n) noexcept {
  return 0x6e31'a800U | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t umov_b0(unsigned d,
                                              unsigned n) noexcept {
  return 0x0e01'3c00U | (n << 5) | d;
}
/* ADD Wd, Wn, #1. */
[[nodiscard]] constexpr std::uint32_t add_w_one(unsigned d,
                                                unsigned n) noexcept {
  return 0x1100'0400U | (n << 5) | d;
}
/* LDR and STR of a SIMD&FP register of 1 << log_size bytes (0 B to 4 Q),
 * unsigned offset zero. */
[[nodiscard]] constexpr std::uint32_t ldr_simd(unsigned log_size, unsigned t,
                                               unsigned n) noexcept {
  constexpr std::uint32_t base[]{0x3d40'0000U, 0x7d40'0000U, 0xbd40'0000U,
                                 0xfd40'0000U, 0x3dc0'0000U};
  return base[log_size] | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t str_simd(unsigned log_size, unsigned t,
                                               unsigned n) noexcept {
  constexpr std::uint32_t base[]{0x3d00'0000U, 0x7d00'0000U, 0xbd00'0000U,
                                 0xfd00'0000U, 0x3d80'0000U};
  return base[log_size] | (n << 5) | t;
}
/* UXTL and SXTL (USHLL and SSHLL #0), Q = 0: the source's element size is
 * log_size (0 B, 1 H, 2 S). */
[[nodiscard]] constexpr std::uint32_t extend_long(bool is_signed,
                                                  unsigned log_size,
                                                  unsigned d,
                                                  unsigned n) noexcept {
  return (is_signed ? 0x0f00'a400U : 0x2f00'a400U) |
         ((8U << log_size) << 16) | (n << 5) | d;
}
/* XTN Vd.<narrow>, Vn.<wide>, Q = 0: log_size is the narrow element's. */
[[nodiscard]] constexpr std::uint32_t narrow(unsigned log_size, unsigned d,
                                             unsigned n) noexcept {
  return 0x0e21'2800U | (log_size << 22) | (n << 5) | d;
}
/* LD1R, and LD2-LD4 and ST2-ST4 (multiple structures) of Vt.. at [Xn],
 * all Q = 1, with SVE's element size. */
[[nodiscard]] constexpr std::uint32_t ld1r(unsigned size, unsigned t,
                                           unsigned n) noexcept {
  return 0x4d40'c000U | (size << 10) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t neon_structure(bool load, unsigned registers,
                                                unsigned size, unsigned t,
                                                unsigned n) noexcept {
  const std::uint32_t opcode = registers == 2U   ? 0x8000U
                               : registers == 3U ? 0x4000U
                                                 : 0x0000U;
  return (load ? 0x4c40'0000U : 0x4c00'0000U) | opcode | (size << 10) |
         (n << 5) | t;
}
/* UMOV Wd, Vn.S[index] and UMOV Xd, Vn.D[index]. */
[[nodiscard]] constexpr std::uint32_t umov_s(unsigned d, unsigned n,
                                             unsigned index) noexcept {
  return 0x0e04'3c00U | (index << 19) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t umov_d(unsigned d, unsigned n,
                                             unsigned index) noexcept {
  return 0x4e08'3c00U | (index << 20) | (n << 5) | d;
}
/* LD1 and ST1 (single structure) of the lane `index` of a 16-byte Vt at
 * [Xn], zero offset, the lane 1 << log_size bytes wide (0 B to 3 D). */
[[nodiscard]] constexpr std::uint32_t lane_memory(bool load,
                                                  unsigned log_size,
                                                  unsigned index, unsigned t,
                                                  unsigned n) noexcept {
  /* Q:S:size packs the index: a byte's is Q:S:size, a halfword's Q:S:size<1>,
   * a word's Q:S and a doubleword's Q. */
  const unsigned opcode = log_size == 0U ? 0U : log_size == 1U ? 2U : 4U;
  unsigned q = index;
  unsigned s = 0;
  unsigned size = 1;
  if (log_size == 0U) {
    q = index >> 3;
    s = (index >> 2) & 1U;
    size = index & 3U;
  } else if (log_size == 1U) {
    q = index >> 2;
    s = (index >> 1) & 1U;
    size = (index & 1U) << 1;
  } else if (log_size == 2U) {
    q = index >> 1;
    s = index & 1U;
    size = 0;
  }
  return (load ? 0x0d40'0000U : 0x0d00'0000U) | (q << 30) | (opcode << 13) |
         (s << 12) | (size << 10) | (n << 5) | t;
}
/* Scalar loads into Wt or Xt at [Xn], zero offset, for LD1R's extending
 * forms: LDRB, LDRH, LDR W and LDR X, then LDRSB W, LDRSB X, LDRSH W,
 * LDRSH X and LDRSW X. */
[[nodiscard]] constexpr std::uint32_t load_scalar(unsigned log_size,
                                                  bool is_signed, bool wide,
                                                  unsigned t,
                                                  unsigned n) noexcept {
  constexpr std::uint32_t unsigned_load[]{0x3940'0000U, 0x7940'0000U,
                                          0xb940'0000U, 0xf940'0000U};
  std::uint32_t opcode = unsigned_load[log_size];
  if (is_signed)
    opcode = (opcode & 0xff00'0000U) | (wide ? 0x0080'0000U : 0x00c0'0000U);
  if (is_signed && log_size == 2U)
    opcode = 0xb980'0000U; // LDRSW X
  return opcode | (n << 5) | t;
}

} // namespace uml::cpu::arm64::jit::encoders
