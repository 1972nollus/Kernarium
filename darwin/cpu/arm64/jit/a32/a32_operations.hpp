// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/a32_profile.hpp"

#include <cstdint>

namespace uml::cpu::arm64::jit::a32 {

/* The operations the compiler lowers, which group the decoder's forms (one
 * per XML encoding) by their mnemonic. The register or immediate shape of a
 * form, and its operands, come from the decoded instruction. */
enum class Op : std::uint8_t {
  none,
  // Data processing.
  adc, add, add_w, adr, and_, bic, cmn, cmp, eor, mov, mvn, orn, orr, rsb, rsc,
  sbc, sub, sub_w, teq, tst, movw, movt,
  // Bit fields, counts and extensions.
  bfc, bfi, sbfx, ubfx, clz, rbit, rev, rev16, revsh, sxtb, sxth, uxtb, uxth,
  sxtab, sxtah, uxtab, uxtah, sxtb16, uxtb16, sxtab16, uxtab16, pkh, sel,
  // Multiplies and divides.
  mul, mla, mls, umull, umlal, smull, smlal, umaal, sdiv, udiv, smla_xy,
  smul_xy, smlal_xy, smlaw, smulw, smlad, smlsd, smuad, smusd, smlald, smlsld,
  smmla, smmls, smmul, usad8, usada8,
  // Saturation.
  qadd, qsub, qdadd, qdsub, ssat, usat, ssat16, usat16,
  // Parallel add and subtract.
  parallel,
  // Memory.
  ldr, ldrb, ldrh, ldrsb, ldrsh, ldrd, ldm, str, strb, strh, strd, stm,
  ldrt, ldrbt, ldrht, ldrsbt, ldrsht, strt, strbt, strht, ldrex, ldrexb,
  ldrexh, ldrexd, strex, strexb, strexh, strexd, ldaex, ldaexb, ldaexh, ldaexd,
  stlex, stlexb, stlexh, stlexd, lda, ldab, ldah, stl, stlb, stlh, tb, prefetch,
  // Branches.
  b, bl, blx, bx, cbz, cbnz,
  // System.
  svc, bkpt, udf, mrs, msr, coprocessor, nop, dmb, dsb, isb, clrex, it,
  // VFP and Advanced SIMD (a32_vector_operations.hpp names the mnemonic).
  vector,
};

/* The suffix of a parallel add or subtract, with its signedness and mode. */
enum class ParallelKind : std::uint8_t {
  add16, add8, asx, sax, sub16, sub8,
};
enum class ParallelMode : std::uint8_t {
  signed_wrap, saturating, signed_halving, unsigned_wrap,
  unsigned_saturating, unsigned_halving,
};

/* How a form names its second operand: an immediate, a register (shifted by
 * an immediate), a register shifted by a register, or the literal pool. */
enum class Shape : std::uint8_t { other, immediate, reg, reg_shifted, literal };

struct OpInfo final {
  Op op{Op::none};
  ParallelKind kind{};
  ParallelMode mode{};
  Shape shape{Shape::other};
  /* The form takes SP as its Rn, which its operands leave out. */
  bool sp_base{};
};

[[nodiscard]] OpInfo op_info(aarch32::Form form) noexcept;

/* The load-exclusive and store-exclusive operations, with the acquire and
 * release forms. */
[[nodiscard]] constexpr bool is_exclusive_op(Op op) noexcept {
  return op >= Op::ldrex && op <= Op::stlexd;
}

/* Whether the instruction ends a block: it writes the PC, raises an event or
 * is not an instruction. */
[[nodiscard]] bool ends_block(const aarch32::Instruction &inst) noexcept;

/* The decoder level the compiler implements. */
[[nodiscard]] constexpr aarch32::Level a32_level() noexcept {
  return aarch32::Level::simd;
}

} // namespace uml::cpu::arm64::jit::a32
