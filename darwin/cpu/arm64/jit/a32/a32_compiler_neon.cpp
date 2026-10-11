// SPDX-License-Identifier: GPL-2.0-or-later
//
// Advanced SIMD: the dispatch, the operand plumbing shared by the families,
// and the operations on registers of one width, integer and floating point.
//
// A Q operand is its resident host register and a D operand is the low
// (even) or high (odd) half of one. Every D-register operation therefore runs
// as the 64-bit arrangement of the A64 instruction on sources moved to the
// scratch registers V0 and V1, writes its result to V5, and inserts it into
// the destination half, which keeps the other half. The operations A64 has no
// 64-bit arrangement for (the 64-bit lanes of the arithmetic, saturating and
// shift groups) run on the 128-bit arrangement with each D source in both
// halves, whose low half is the result.
//
// Floating-point operations run under the Standard FPSCR value, which the
// translation sets before the first one (set_fp_mode), and the saturating
// operations set QC in the host FPSR, which is why a block holding one owns
// the native floating-point environment.

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_operations.hpp"
#include "a32_vector_encoders.hpp"

#include <bit>

namespace uml::cpu::arm64::jit::a32 {

namespace {

constexpr unsigned s_n = 0;
constexpr unsigned s_m = 1;
constexpr unsigned s_acc = 2;
constexpr unsigned s_result = 5;
constexpr unsigned s_extra = 6;

} // namespace

// ---------------------------------------------------------------------------
// Operand plumbing.

unsigned A32Compiler::esize_log2(int esize) noexcept {
  return static_cast<unsigned>(
      std::countr_zero(static_cast<unsigned>(esize) / 8U));
}

void A32Compiler::vmov(unsigned d, unsigned n) {
  if (d != n)
    emit(enc::vec::bitwise_op(true, enc::vec::orr, d, n, n));
}

unsigned A32Compiler::vsrc(unsigned reg, bool quad, unsigned scratch) {
  return quad ? q_use(reg >> 1, scratch) : d_use(reg, scratch);
}

/* dest = op(n, m) on D or Q registers. `base` is the A64 instruction with its
 * register fields clear and its arrangement set for `quad || wide`; `wide`
 * runs a D operation on the 128-bit arrangement with both halves equal. An
 * operation that accumulates starts from the destination's value. */
void A32Compiler::neon_op3(std::uint32_t base, bool quad, bool wide,
                           unsigned d, unsigned n, unsigned m,
                           bool reads_dest) {
  if (quad) {
    const unsigned a = q_use(n >> 1, s_n);
    const unsigned b = q_use(m >> 1, s_m);
    const auto dest = q_def(d >> 1, reads_dest, s_result);
    emit_vec3(base, dest.host, a, b);
    q_done(dest, d >> 1);
    return;
  }
  const unsigned a = wide ? d_use_twice(n, s_n) : d_use(n, s_n);
  const unsigned b = wide ? d_use_twice(m, s_m) : d_use(m, s_m);
  if (reads_dest)
    vmov(s_result, wide ? d_use_twice(d, s_acc) : d_use(d, s_acc));
  emit_vec3(base, s_result, a, b);
  d_write(d, s_result);
}

void A32Compiler::neon_op2(std::uint32_t base, bool quad, bool wide,
                           unsigned d, unsigned m, bool reads_dest) {
  if (quad) {
    const unsigned a = q_use(m >> 1, s_m);
    const auto dest = q_def(d >> 1, reads_dest, s_result);
    emit_vec2(base, dest.host, a);
    q_done(dest, d >> 1);
    return;
  }
  const unsigned a = wide ? d_use_twice(m, s_m) : d_use(m, s_m);
  if (reads_dest)
    vmov(s_result, wide ? d_use_twice(d, s_acc) : d_use(d, s_acc));
  emit_vec2(base, s_result, a);
  d_write(d, s_result);
}

/* The Q register q takes `result` (a scratch register or a resident one). */
void A32Compiler::q_put(unsigned q, unsigned result) {
  const auto dest = q_def(q, false, result);
  vmov(dest.host, result);
  q_done(dest, q);
}

/* D or Q register `reg` takes `result`. */
void A32Compiler::neon_put(unsigned reg, bool quad, unsigned result) {
  if (quad)
    q_put(reg >> 1, result);
  else
    d_write(reg, result);
}

// ---------------------------------------------------------------------------
// The dispatch.

/* Whether the instruction is a half-precision variant of an Advanced SIMD
 * floating-point form. The variants share the form (and the mnemonic) of the
 * single-precision ones and differ in the operands' esize (16), so the
 * single-precision lowering cannot run them. */
bool A32Compiler::neon_half_precision(const VInfo &v) const {
  const auto &o = cur().inst.operands;
  if (o.esize != 16)
    return false;
  switch (v.mnemonic) {
  case VM::vfma: case VM::vfms: case VM::vacge: case VM::vacgt:
  case VM::vrecps: case VM::vrsqrts:
    return true;
  case VM::vceq:
    return v.variant == Variant::i ? o.floating_point : !o.int_operation;
  case VM::vcge:
    return v.variant == Variant::i ? o.floating_point
                                   : o.vtype_vcge == aarch32::VCGEType::fp;
  case VM::vcgt:
    return v.variant == Variant::i ? o.floating_point
                                   : o.vtype_vcgt == aarch32::VCGTtype::fp;
  case VM::vcle: case VM::vclt: case VM::vabs: case VM::vneg: case VM::vrecpe:
  case VM::vrsqrte:
    return o.floating_point;
  case VM::vcvt:
    return v.variant == Variant::is || v.variant == Variant::xs;
  default:
    return v.variant == Variant::f || o.floating_point;
  }
}

A32Compiler::Flow A32Compiler::emit_neon(const VInfo &v) {
  const auto &o = cur().inst.operands;
  if (neon_half_precision(v))
    return emit_arith_through_helper();
  switch (v.mnemonic) {
  case VM::vmul: case VM::vmla: case VM::vmls: case VM::vmlal:
  case VM::vmlsl: case VM::vmull: case VM::vqdmulh: case VM::vqrdmulh:
  case VM::vqdmull: case VM::vqdmlal: case VM::vqdmlsl:
    if (v.variant == Variant::s || o.scalar_form)
      return emit_neon_scalar(v);
    break;
  default:
    break;
  }
  switch (v.mnemonic) {
  case VM::vadd: case VM::vsub: case VM::vmul: case VM::vmla: case VM::vmls:
  case VM::vaba: case VM::vabd: case VM::vhadd: case VM::vhsub:
  case VM::vrhadd: case VM::vqadd: case VM::vqsub: case VM::vrshl:
  case VM::vqrshl: case VM::vmax: case VM::vmin: case VM::vpadd:
  case VM::vpmax: case VM::vpmin: case VM::vtst: case VM::vqdmulh:
  case VM::vqrdmulh: case VM::vfma: case VM::vfms: case VM::vacge:
  case VM::vacgt: case VM::vrecps: case VM::vrsqrts:
    return emit_neon_same(v);
  case VM::vshl: case VM::vqshl:
    return v.variant == Variant::i ? emit_neon_shift(v) : emit_neon_same(v);
  case VM::vceq: case VM::vcge: case VM::vcgt: case VM::vcle: case VM::vclt:
    return v.variant == Variant::i ? emit_neon_misc(v) : emit_neon_same(v);
  case VM::vand: case VM::vbic: case VM::vorr: case VM::vorn: case VM::veor:
  case VM::vbsl: case VM::vbit: case VM::vbif:
    return v.variant == Variant::i ? emit_neon_immediate(v)
                                   : emit_neon_logic(v);
  case VM::vmvn:
    return v.variant == Variant::i ? emit_neon_immediate(v)
                                   : emit_neon_logic(v);
  case VM::vmov:
    return emit_neon_immediate(v);
  case VM::vshr: case VM::vsra: case VM::vrshr: case VM::vrsra: case VM::vsli:
  case VM::vsri: case VM::vqshlu: case VM::vshrn: case VM::vrshrn:
  case VM::vqshrn: case VM::vqrshrn: case VM::vqshrun: case VM::vqrshrun:
  case VM::vshll: case VM::vmovl:
    return emit_neon_shift(v);
  case VM::vaddl: case VM::vaddw: case VM::vsubl: case VM::vsubw:
  case VM::vabal: case VM::vabdl: case VM::vmlal: case VM::vmlsl:
  case VM::vmull: case VM::vqdmull: case VM::vqdmlal: case VM::vqdmlsl:
  case VM::vaddhn: case VM::vsubhn: case VM::vraddhn: case VM::vrsubhn:
    return emit_neon_diff(v);
  case VM::vabs: case VM::vneg: case VM::vqabs: case VM::vqneg: case VM::vcls:
  case VM::vclz: case VM::vcnt: case VM::vrev16: case VM::vrev32:
  case VM::vrev64: case VM::vpaddl: case VM::vpadal: case VM::vrecpe:
  case VM::vrsqrte: case VM::vmovn: case VM::vqmovn: case VM::vqmovun:
  case VM::vcvt:
    return emit_neon_misc(v);
  case VM::vdup: case VM::vext: case VM::vswp: case VM::vtrn: case VM::vzip:
  case VM::vuzp: case VM::vtbl: case VM::vtbx:
    return emit_neon_permute(v);
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// Three registers of the same width.

A32Compiler::Flow A32Compiler::emit_neon_same(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  const bool quad = v.quad;
  using enc::vec::FloatSame;
  using enc::vec::Same;

  const bool is_float =
      v.variant == Variant::f || v.mnemonic == VM::vfma ||
      v.mnemonic == VM::vfms || v.mnemonic == VM::vacge ||
      v.mnemonic == VM::vacgt || v.mnemonic == VM::vrecps ||
      v.mnemonic == VM::vrsqrts ||
      (v.mnemonic == VM::vceq && !o.int_operation) ||
      (v.mnemonic == VM::vcge && o.vtype_vcge == aarch32::VCGEType::fp) ||
      (v.mnemonic == VM::vcgt && o.vtype_vcgt == aarch32::VCGTtype::fp);
  if (is_float) {
    touch_fp_environment();
    set_fp_mode(true);
    FloatSame op = enc::vec::fadd;
    bool accumulate = false;
    switch (v.mnemonic) {
    case VM::vadd: op = enc::vec::fadd; break;
    case VM::vsub: op = enc::vec::fsub; break;
    case VM::vmul: op = enc::vec::fmul; break;
    case VM::vabd: op = enc::vec::fabd; break;
    case VM::vmax: op = enc::vec::fmax; break;
    case VM::vmin: op = enc::vec::fmin; break;
    case VM::vpadd: op = enc::vec::faddp; break;
    case VM::vpmax: op = enc::vec::fmaxp; break;
    case VM::vpmin: op = enc::vec::fminp; break;
    case VM::vrecps:
    case VM::vrsqrts:
      return emit_neon_step(v.mnemonic == VM::vrsqrts, quad, d, n, m);
    case VM::vacge: op = enc::vec::facge; break;
    case VM::vacgt: op = enc::vec::facgt; break;
    case VM::vceq: op = enc::vec::fcmeq; break;
    case VM::vcge: op = enc::vec::fcmge; break;
    case VM::vcgt: op = enc::vec::fcmgt; break;
    case VM::vfma: op = enc::vec::fmla; accumulate = true; break;
    case VM::vfms: op = enc::vec::fmls; accumulate = true; break;
    case VM::vmla:
    case VM::vmls: {
      // The product is rounded before it is added: two operations.
      const unsigned a = vsrc(n, quad, s_n);
      const unsigned b = vsrc(m, quad, s_m);
      emit_vec3(enc::vec::float_same(quad, enc::vec::fmul, false, 0, 0, 0),
                s_extra, a, b);
      const unsigned old = vsrc(d, quad, s_acc);
      emit_vec3(enc::vec::float_same(quad,
                                     v.mnemonic == VM::vmla ? enc::vec::fadd
                                                            : enc::vec::fsub,
                                     false, 0, 0, 0),
                s_result, old, s_extra);
      neon_put(d, quad, s_result);
      return Flow::next;
    }
    default:
      exit_illegal();
      return Flow::end;
    }
    neon_op3(enc::vec::float_same(quad, op, false, 0, 0, 0), quad, false, d, n,
             m, accumulate);
    return Flow::next;
  }

  const unsigned size = esize_log2(o.esize);
  const bool wide64 = !quad && o.esize == 64;
  Same op = enc::vec::add;
  bool swap = false;
  bool accumulate = false;
  bool saturating = false;
  const bool u = o.unsigned_;
  switch (v.mnemonic) {
  case VM::vadd: op = enc::vec::add; break;
  case VM::vsub: op = enc::vec::sub; break;
  case VM::vmul: op = o.polynomial ? enc::vec::pmul : enc::vec::mul; break;
  case VM::vmla: op = enc::vec::mla; accumulate = true; break;
  case VM::vmls: op = enc::vec::mls; accumulate = true; break;
  case VM::vaba: op = u ? enc::vec::uaba : enc::vec::saba; accumulate = true; break;
  case VM::vabd: op = u ? enc::vec::uabd : enc::vec::sabd; break;
  case VM::vhadd: op = u ? enc::vec::uhadd : enc::vec::shadd; break;
  case VM::vhsub: op = u ? enc::vec::uhsub : enc::vec::shsub; break;
  case VM::vrhadd: op = u ? enc::vec::urhadd : enc::vec::srhadd; break;
  case VM::vqadd: op = u ? enc::vec::uqadd : enc::vec::sqadd; saturating = true; break;
  case VM::vqsub: op = u ? enc::vec::uqsub : enc::vec::sqsub; saturating = true; break;
  case VM::vshl: op = u ? enc::vec::ushl : enc::vec::sshl; swap = true; break;
  case VM::vrshl: op = u ? enc::vec::urshl : enc::vec::srshl; swap = true; break;
  case VM::vqshl: op = u ? enc::vec::uqshl : enc::vec::sqshl; swap = true; saturating = true; break;
  case VM::vqrshl: op = u ? enc::vec::uqrshl : enc::vec::sqrshl; swap = true; saturating = true; break;
  case VM::vmax: op = u ? enc::vec::umax : enc::vec::smax; break;
  case VM::vmin: op = u ? enc::vec::umin : enc::vec::smin; break;
  case VM::vpadd: op = enc::vec::addp; break;
  case VM::vpmax: op = u ? enc::vec::umaxp : enc::vec::smaxp; break;
  case VM::vpmin: op = u ? enc::vec::uminp : enc::vec::sminp; break;
  case VM::vtst: op = enc::vec::cmtst; break;
  case VM::vqdmulh: op = enc::vec::sqdmulh; saturating = true; break;
  case VM::vqrdmulh: op = enc::vec::sqrdmulh; saturating = true; break;
  case VM::vceq: op = enc::vec::cmeq; break;
  case VM::vcge:
    op = o.vtype_vcge == aarch32::VCGEType::unsigned_ ? enc::vec::cmhs
                                                      : enc::vec::cmge;
    break;
  case VM::vcgt:
    op = o.vtype_vcgt == aarch32::VCGTtype::unsigned_ ? enc::vec::cmhi
                                                      : enc::vec::cmgt;
    break;
  default:
    exit_illegal();
    return Flow::end;
  }
  if (saturating)
    touch_fp_environment();
  neon_op3(enc::vec::same(quad || wide64, op, size, 0, 0, 0), quad, wide64, d,
           swap ? m : n, swap ? n : m, accumulate);
  return Flow::next;
}

/* VRECPS and VRSQRTS. A64 FRECPS and FRSQRTS fuse the multiplication, which
 * rounds and raises flags differently, so the step is the product rounded
 * first and then 2 - p (or (3 - p) / 2). An infinity times a zero (a
 * denormal is a zero under the Standard FPSCR) gives 2.0 (or 1.5) without
 * raising Invalid Operation, which the multiplication would. The infinite
 * operand of such a lane is replaced by 1.0 before the product, so the
 * other operand still raises Input Denormal, and the constant takes the
 * lane afterwards. The classification is on integer images of the operands
 * so that it raises nothing. */
A32Compiler::Flow A32Compiler::emit_neon_step(bool rsqrt, bool quad, unsigned d,
                                              unsigned n, unsigned m) {
  using enc::vec::Logical;
  const unsigned a = vsrc(n, quad, s_n);
  const unsigned b = vsrc(m, quad, s_m);
  constexpr unsigned a_sign = 2, b_sign = 3, t_inf = 4, t_mask = 6, t_special = 7;
  const auto fmov = [&](unsigned reg, unsigned imm8) {
    emit(enc::vec::modified_immediate(quad, 0, 15, imm8, reg));
  };
  emit_vec2(enc::vec::shift_imm(quad, enc::vec::shl, 33, 0, 0), a_sign, a);
  emit_vec2(enc::vec::shift_imm(quad, enc::vec::shl, 33, 0, 0), b_sign, b);
  // |x| << 1 of an infinity is 0xff000000, and below 0x01000000 for a zero.
  emit(enc::vec::modified_immediate(quad, 0, 6, 0xff, t_inf));
  emit_vec3(enc::vec::same(quad, enc::vec::cmeq, 2, 0, 0, 0), t_mask, a_sign,
            t_inf);
  emit_vec3(enc::vec::same(quad, enc::vec::cmeq, 2, 0, 0, 0), t_inf, b_sign,
            t_inf);
  emit(enc::vec::modified_immediate(quad, 0, 6, 0x01, t_special));
  emit_vec3(enc::vec::same(quad, enc::vec::cmhi, 2, 0, 0, 0), a_sign, t_special,
            a_sign);
  emit_vec3(enc::vec::same(quad, enc::vec::cmhi, 2, 0, 0, 0), b_sign, t_special,
            b_sign);
  emit_vec3(enc::vec::bitwise_op(quad, Logical::and_, 0, 0, 0), t_mask, t_mask,
            b_sign);
  emit_vec3(enc::vec::bitwise_op(quad, Logical::and_, 0, 0, 0), t_inf, t_inf,
            a_sign);
  emit_vec3(enc::vec::bitwise_op(quad, Logical::orr, 0, 0, 0), t_special,
            t_mask, t_inf);
  fmov(a_sign, 0x70); // 1.0
  emit_vec3(enc::vec::bitwise_op(quad, Logical::bsl, 0, 0, 0), t_mask, a_sign,
            a);
  emit_vec3(enc::vec::bitwise_op(quad, Logical::bsl, 0, 0, 0), t_inf, a_sign,
            b);
  emit_vec3(enc::vec::float_same(quad, enc::vec::fmul, false, 0, 0, 0),
            s_result, t_mask, t_inf);
  fmov(a_sign, rsqrt ? 0x08 : 0x00); // 3.0 or 2.0
  emit_vec3(enc::vec::float_same(quad, enc::vec::fsub, false, 0, 0, 0),
            s_result, a_sign, s_result);
  if (rsqrt) {
    fmov(a_sign, 0x60); // 0.5
    emit_vec3(enc::vec::float_same(quad, enc::vec::fmul, false, 0, 0, 0),
              s_result, s_result, a_sign);
    fmov(a_sign, 0x78); // 1.5
  } else {
    fmov(a_sign, 0x00); // 2.0
  }
  emit_vec3(enc::vec::bitwise_op(quad, Logical::bit, 0, 0, 0), s_result, a_sign,
            t_special);
  neon_put(d, quad, s_result);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// Bitwise operations on whole registers.

A32Compiler::Flow A32Compiler::emit_neon_logic(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  const bool quad = v.quad;
  using enc::vec::Logical;
  if (v.mnemonic == VM::vmvn) {
    neon_op2(enc::vec::misc(quad, enc::vec::not_, 0, 0, 0), quad, false, d, m,
             false);
    return Flow::next;
  }
  Logical op = enc::vec::and_;
  bool accumulate = false;
  switch (v.mnemonic) {
  case VM::vand: op = enc::vec::and_; break;
  case VM::vbic: op = enc::vec::bic; break;
  case VM::vorr: op = enc::vec::orr; break;
  case VM::vorn: op = enc::vec::orn; break;
  case VM::veor: op = enc::vec::eor; break;
  case VM::vbsl: op = enc::vec::bsl; accumulate = true; break;
  case VM::vbit: op = enc::vec::bit; accumulate = true; break;
  case VM::vbif: op = enc::vec::bif; accumulate = true; break;
  default:
    exit_illegal();
    return Flow::end;
  }
  neon_op3(enc::vec::bitwise_op(quad, op, 0, 0, 0), quad, false, d, n, m,
           accumulate);
  return Flow::next;
}

} // namespace uml::cpu::arm64::jit::a32
