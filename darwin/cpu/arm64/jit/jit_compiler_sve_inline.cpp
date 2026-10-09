// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Non-streaming SVE in the arm64 JIT beyond the control core (design 4.3,
 * SVE lane V4 milestone M2): the hot set of the form census, lowered onto
 * NEON at VL 16 bytes, and the contiguous memory forms with the page split.
 * jit_compiler_sve.cpp has the register model; Z_n is the guest's V_n.
 *
 * Predicated forms lower for the predicate that is all-active, where the
 * form is its unpredicated NEON body. A predicate is checked once per block
 * and element size (emit_sve_full_guard); one that is not all-active leaves
 * the block at the word, which the semantic library then runs: the library
 * alone knows what inactive lanes must not do (raise, fault, set FPSR). A
 * PTRUE that sets every element makes a predicate known all-active with no
 * check. This replaces design 4.1.5's block versioning, which would emit
 * every body twice, with the library as the partial-predicate body.
 *
 * A lowering that declines a word does so before emitting anything, so the
 * word runs through the library as every form outside this file does.
 */
#include "jit_compiler.hpp"
#include "jit_sve_encoders.hpp"
#include "jit_sve_inline.hpp"
#include "jit_sme_support.hpp"

#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/sve_profile.hpp"

#include <array>
#include <bit>
#include <cstddef>

namespace uml::cpu::arm64 {

namespace {

namespace inl = jit::sve_inline_forms;
using namespace jit::encoders;

/* A floating-point NEON opcode, Q = 1, as a half-precision and a
 * single-precision template; the double-precision one sets bit 22. */
struct FloatingOp final {
  std::uint32_t half;
  std::uint32_t single;
};
constexpr FloatingOp op_fadd{0x4e40'1400U, 0x4e20'd400U};
constexpr FloatingOp op_fsub{0x4ec0'1400U, 0x4ea0'd400U};
constexpr FloatingOp op_fmul{0x6e40'1c00U, 0x6e20'dc00U};
constexpr FloatingOp op_fmla{0x4e40'0c00U, 0x4e20'cc00U};
constexpr FloatingOp op_fmls{0x4ec0'0c00U, 0x4ea0'cc00U};
constexpr FloatingOp op_fmax{0x4e40'3400U, 0x4e20'f400U};
constexpr FloatingOp op_fmin{0x4ec0'3400U, 0x4ea0'f400U};
constexpr FloatingOp op_fmaxnm{0x4e40'0400U, 0x4e20'c400U};
constexpr FloatingOp op_fminnm{0x4ec0'0400U, 0x4ea0'c400U};
constexpr FloatingOp op_fneg{0x6ef8'f800U, 0x6ea0'f800U};
constexpr FloatingOp op_fabs{0x4ef8'f800U, 0x4ea0'f800U};
constexpr FloatingOp op_scvtf{0x4e79'd800U, 0x4e21'd800U};
constexpr FloatingOp op_ucvtf{0x6e79'd800U, 0x6e21'd800U};
constexpr FloatingOp op_fcvtzs{0x4ef9'b800U, 0x4ea1'b800U};
constexpr FloatingOp op_fcvtzu{0x6ef9'b800U, 0x6ea1'b800U};
constexpr FloatingOp op_fdiv{0x6e40'3c00U, 0x6e20'fc00U};
constexpr FloatingOp op_fsqrt{0x6ef9'f800U, 0x6ea1'f800U};
constexpr FloatingOp op_frintn{0x4e79'8800U, 0x4e21'8800U};
constexpr FloatingOp op_frintp{0x4ef9'8800U, 0x4ea1'8800U};
constexpr FloatingOp op_frintm{0x4e79'9800U, 0x4e21'9800U};
constexpr FloatingOp op_frintz{0x4ef9'9800U, 0x4ea1'9800U};
constexpr FloatingOp op_frinta{0x6e79'8800U, 0x6e21'8800U};
constexpr FloatingOp op_frintx{0x6e79'9800U, 0x6e21'9800U};
constexpr FloatingOp op_frinti{0x6ef9'9800U, 0x6ea1'9800U};

/* The NEON word for SVE element size `size` (1 half, 2 single, 3
 * double), registers zero. */
[[nodiscard]] constexpr std::uint32_t floating_word(const FloatingOp &op,
                                                    unsigned size) noexcept {
  return size == 1U   ? op.half
         : size == 2U ? op.single
                      : op.single | 0x0040'0000U;
}

/* A predicated destructive binary form (Zdn, Pg/M, Zdn, Zm): the NEON
 * opcode, whether it has a doubleword form, and whether the operands swap
 * (SUBR, FSUBR). */
struct DestructiveOp final {
  std::uint16_t form;
  bool floating;
  FloatingOp fp;
  std::uint32_t opcode;
  bool sized;
  bool doubleword;
  bool reversed;
};

constexpr auto integer_op(std::uint16_t form, std::uint32_t opcode,
                          bool sized, bool doubleword, bool reversed) {
  return DestructiveOp{form, false, {}, opcode, sized, doubleword, reversed};
}
constexpr auto floating_op(std::uint16_t form, FloatingOp op, bool reversed) {
  return DestructiveOp{form, true, op, 0, true, true, reversed};
}

constexpr std::array destructive_ops{
    integer_op(inl::add_p, 0x4e20'8400U, true, true, false),
    integer_op(inl::sub_p, 0x6e20'8400U, true, true, false),
    integer_op(inl::subr_p, 0x6e20'8400U, true, true, true),
    integer_op(inl::mul_p, 0x4e20'9c00U, true, false, false),
    integer_op(inl::umin_p, 0x6e20'6c00U, true, false, false),
    integer_op(inl::umax_p, 0x6e20'6400U, true, false, false),
    integer_op(inl::smin_p, 0x4e20'6c00U, true, false, false),
    integer_op(inl::smax_p, 0x4e20'6400U, true, false, false),
    integer_op(inl::sabd_p, 0x4e20'7400U, true, false, false),
    integer_op(inl::uabd_p, 0x6e20'7400U, true, false, false),
    integer_op(inl::shadd_p, 0x4e20'0400U, true, false, false),
    integer_op(inl::uhadd_p, 0x6e20'0400U, true, false, false),
    integer_op(inl::shsub_p, 0x4e20'2400U, true, false, false),
    integer_op(inl::uhsub_p, 0x6e20'2400U, true, false, false),
    integer_op(inl::shsubr_p, 0x4e20'2400U, true, false, true),
    integer_op(inl::uhsubr_p, 0x6e20'2400U, true, false, true),
    integer_op(inl::srhadd_p, 0x4e20'1400U, true, false, false),
    integer_op(inl::urhadd_p, 0x6e20'1400U, true, false, false),
    integer_op(inl::orr_zp, 0x4ea0'1c00U, false, true, false),
    integer_op(inl::eor_zp, 0x6e20'1c00U, false, true, false),
    integer_op(inl::and_zp, 0x4e20'1c00U, false, true, false),
    integer_op(inl::bic_zp, 0x4e60'1c00U, false, true, false),
    floating_op(inl::fadd_p, op_fadd, false),
    floating_op(inl::fsub_p, op_fsub, false),
    floating_op(inl::fsubr_p, op_fsub, true),
    floating_op(inl::fmul_p, op_fmul, false),
    floating_op(inl::fmaxnm_p, op_fmaxnm, false),
    floating_op(inl::fminnm_p, op_fminnm, false),
    floating_op(inl::fmax_p, op_fmax, false),
    floating_op(inl::fmin_p, op_fmin, false),
};

/* The shift-immediate tsz:imm3 of a word whose tszh is bits 23:22, whose
 * tszl is at `low_bit` (20:19 or 9:8) and whose imm3 is at `imm3_low`. */
[[nodiscard]] constexpr unsigned shift_code(std::uint32_t word,
                                            unsigned low_bit,
                                            unsigned imm3_low) noexcept {
  return (field(word, 22, 2) << 5) | (field(word, low_bit, 2) << 3) |
         field(word, imm3_low, 3);
}

/* log2 of the element size a tsz:imm3 code names, or 4 when tsz is 0. */
[[nodiscard]] constexpr unsigned shift_log_size(unsigned code) noexcept {
  const unsigned tsz = code >> 3;
  return tsz == 0U ? 4U : static_cast<unsigned>(std::bit_width(tsz)) - 1U;
}

} // namespace

void JitCompiler::begin_sve_word() {
  /* P, FFR and Z are FP state: the executor makes it current first (design
   * 2.2). The first SVE word of the block marks the task as using SVE, so
   * a context switch saves its predicates. */
  accesses_fp_state_ = true;
  if (!sve_marked_) {
    emitter_.load_constant(8, 1);
    emitter_.store_b(8, 15, offsetof(CpuState, sve_live));
    sve_marked_ = true;
  }
}

void JitCompiler::emit_sve_full_guard(unsigned predicate, unsigned esize,
                                      Arm64Emitter::Label slow) {
  if (sve_predicate_known_full(predicate, esize))
    return;
  /*
   * Every element active: the lane mask's smallest byte is 0xff. Flag
   * neutral (ADD, TBZ), as the cold path's snapshot requires.
   */
  const auto governing = read_native_predicate(predicate, 0U);
  const unsigned mask = emit_sve_lane_mask(governing.host, esize, scratch_b);
  emitter_.emit(uminv_b(scratch_a, mask));
  emitter_.emit(umov_b0(8, scratch_a));
  emitter_.emit(add_w_one(8, 8));
  emitter_.test_zero(slow, 8, 8, false);
  note_sve_predicate_full(predicate, esize);
}

void JitCompiler::emit_sve_guard(unsigned predicate, unsigned esize,
                                 std::uint32_t instruction,
                                 std::uint64_t pc) {
  /* The cold exit is made only when a check is emitted, and before any
   * binding it would not flush. */
  if (!sve_predicate_known_full(predicate, esize))
    emit_sve_full_guard(
        predicate, esize,
        cold_exit(instruction, pc, ColdPath::Action::sve_slow));
}

bool JitCompiler::emit_sve_inline(const detail::SmeEncoding &row,
                                  std::uint32_t instruction,
                                  std::uint64_t pc) {
  if (!host_capabilities().advanced_simd)
    return false;
  if (row.memory != 0U)
    return emit_sve_memory(row, instruction, pc);
  if (row.form == inl::cmp_vector || row.form == inl::cmp_immediate)
    return emit_sve_compare(row.form, instruction);
  return emit_sve_floating(row.form, instruction, pc) ||
         emit_sve_integer(row.form, instruction, pc) ||
         emit_sve_narrow(row.form, instruction) ||
         emit_sve_permute(row.form, instruction, pc);
}

bool JitCompiler::emit_sve_floating(std::uint16_t form,
                                    std::uint32_t instruction,
                                    std::uint64_t pc) {
  const auto &host = host_capabilities();
  if (!host.floating_point)
    return false;
  const unsigned size = field(instruction, 22, 2);
  const unsigned d = field(instruction, 0, 5);
  const unsigned n = field(instruction, 5, 5);
  const unsigned m = field(instruction, 16, 5);
  const unsigned g = field(instruction, 10, 3);
  /* Half precision needs the host's FP16 arithmetic. */
  const bool supported = size != 0U && (size != 1U || host.fp16);

  for (const auto &op : destructive_ops) {
    if (op.form != form || !op.floating)
      continue;
    if (!supported)
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    /* Zdn is bits 0-4 and Zm bits 5-9. */
    emit_native_vector_binary(
        three(floating_word(op.fp, size), d, op.reversed ? n : d,
              op.reversed ? d : n),
        false);
    mark_native_fp();
    return true;
  }

  if (form == inl::fmla_p || form == inl::fmls_p) {
    if (!supported)
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    emit_native_vector_accumulate(
        three(floating_word(form == inl::fmla_p ? op_fmla : op_fmls, size), d,
              n, m),
        true);
    mark_native_fp();
    return true;
  }
  if (form == inl::fmad_p || form == inl::fmsb_p) {
    /* Zdn = Zdn * Zm + Za (FMSB: Za - Zdn * Zm); Zm is bits 5-9, Za 16-20. */
    if (!supported)
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    const auto accumulator = read_native_vector(m, 0U);
    const auto multiplicand = read_native_vector(d, 1U);
    const auto multiplier = n == d   ? multiplicand
                            : n == m ? accumulator
                                     : read_native_vector(n, 2U);
    emitter_.emit(vector_move(scratch_a, accumulator.host));
    emitter_.emit(three(floating_word(form == inl::fmad_p ? op_fmla : op_fmls,
                                      size),
                        scratch_a, multiplicand.host, multiplier.host));
    const auto out = write_native_vector(d, false, 3U);
    emitter_.emit(vector_move(out.host, scratch_a));
    commit_native_vector(out, d);
    mark_native_fp();
    return true;
  }
  if (form == inl::fmla_i || form == inl::fmls_i) {
    /* Zm is 3 bits (4 for doubles) from bit 16; the index follows. */
    if (!supported)
      return false;
    begin_sve_word();
    std::uint32_t word;
    const bool subtract = form == inl::fmls_i;
    if (size == 1U) {
      const unsigned index =
          (field(instruction, 22, 1) << 2) | field(instruction, 19, 2);
      word = (subtract ? 0x4f00'5000U : 0x4f00'1000U) |
             (field(instruction, 16, 3) << 16) | ((index & 1U) << 20) |
             (((index >> 1) & 1U) << 21) | ((index >> 2) << 11);
    } else if (size == 2U) {
      const unsigned index = field(instruction, 19, 2);
      word = (subtract ? 0x4f80'5000U : 0x4f80'1000U) |
             (field(instruction, 16, 3) << 16) | ((index & 1U) << 21) |
             ((index >> 1) << 11);
    } else {
      word = (subtract ? 0x4fc0'5000U : 0x4fc0'1000U) |
             (field(instruction, 16, 4) << 16) |
             (field(instruction, 20, 1) << 11);
    }
    emit_native_vector_by_element(word | (n << 5) | d);
    mark_native_fp();
    return true;
  }
  if (form == inl::fadd_u || form == inl::fsub_u || form == inl::fmul_u) {
    if (!supported)
      return false;
    begin_sve_word();
    const auto &op = form == inl::fadd_u   ? op_fadd
                     : form == inl::fsub_u ? op_fsub
                                           : op_fmul;
    emit_native_vector_binary(three(floating_word(op, size), d, n, m), false);
    mark_native_fp();
    return true;
  }
  if (form == inl::fabs_p || form == inl::fneg_p) {
    if (!supported)
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    emit_native_vector_unary(
        three(floating_word(form == inl::fabs_p ? op_fabs : op_fneg, size), d,
              n, 0),
        false);
    /* FPCR.AH keeps the sign of a NaN, so the native environment matters. */
    mark_native_fp();
    return true;
  }
  if (form == inl::fadda_p || form == inl::faddv_p) {
    if (!supported)
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    const auto vector = read_native_vector(n, 0U);
    if (form == inl::fadda_p) {
      /* Vdn = Vdn + each element of Zm in turn, the scalar adds the
       * architecture orders. */
      const auto accumulator = read_native_vector(d, 1U);
      const std::uint32_t add = size == 1U   ? 0x1ee0'2800U
                                : size == 2U ? 0x1e20'2800U
                                             : 0x1e60'2800U;
      emitter_.emit(add | (vector.host << 16) | (accumulator.host << 5) |
                    scratch_a);
      for (unsigned element = 1; element < sve_vector_length >> size;
           ++element) {
        emitter_.emit(0x5e00'0400U |
                      (((element << (size + 1U)) | (1U << size)) << 16) |
                      (vector.host << 5) | scratch_b);
        emitter_.emit(add | (scratch_b << 16) | (scratch_a << 5) | scratch_a);
      }
    } else if (size == 3U) {
      emitter_.emit(0x7e70'd800U | (vector.host << 5) | scratch_a);
    } else {
      /* The halving tree of FADDV: pairs of neighbours, then pairs of
       * those, as FADDP. */
      const std::uint32_t pairs = size == 2U ? 0x6e20'd400U : 0x6e40'1400U;
      emitter_.emit(pairs | (vector.host << 16) | (vector.host << 5) |
                    scratch_a);
      if (size == 1U)
        emitter_.emit(pairs | (scratch_a << 16) | (scratch_a << 5) |
                      scratch_a);
      emitter_.emit((size == 2U ? 0x7e30'd800U : 0x5e30'd800U) |
                    (scratch_a << 5) | scratch_a);
    }
    /* The result is the low element of a zeroed register. A scalar add
     * does not leave the rest so under FPCR.NEP, where it keeps its
     * source's other elements, so insert the element into zero. */
    const auto out = write_native_vector(d, false, 2U);
    emitter_.emit(0x6f00'e400U | out.host);
    emitter_.emit(0x6e00'0400U | ((1U << size) << 16) | (scratch_a << 5) |
                  out.host);
    commit_native_vector(out, d);
    mark_native_fp();
    return true;
  }
  if (form == inl::fdiv_p || form == inl::fdivr_p) {
    if (!supported)
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    /* Zdn is bits 0-4 and Zm bits 5-9; FDIVR divides Zm by Zdn. */
    const bool reversed = form == inl::fdivr_p;
    emit_native_vector_binary(
        three(floating_word(op_fdiv, size), d, reversed ? n : d,
              reversed ? d : n),
        false);
    mark_native_fp();
    return true;
  }
  if (form == inl::fsqrt_p || form == inl::frint_p) {
    if (!supported)
      return false;
    /* FRINT<x>'s mode is bits 16-18: N, P, M, Z, A, (unallocated), X, I. */
    constexpr std::array frint_ops{&op_frintn, &op_frintp, &op_frintm,
                                   &op_frintz, &op_frinta, &op_frintn,
                                   &op_frintx, &op_frinti};
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    const auto &op = form == inl::fsqrt_p
                         ? op_fsqrt
                         : *frint_ops[field(instruction, 16, 3)];
    emit_native_vector_unary(three(floating_word(op, size), d, n, 0), false);
    mark_native_fp();
    return true;
  }
  if (form == inl::fcadd_p) {
    /* Zdn, Pg/M, Zdn, Zm, #90 or #270 (bit 16): the complex add. */
    if (!supported || !host.has(ArmFeature::fcma))
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    emit_native_vector_binary(
        three(0x6e00'e400U | (size << 22) | (field(instruction, 16, 1) << 12), d,
              d, n),
        false);
    mark_native_fp();
    return true;
  }
  if (form == inl::fadd_imm || form == inl::fsub_imm || form == inl::fmul_imm ||
      form == inl::fsubr_imm || form == inl::fmaxnm_imm ||
      form == inl::fminnm_imm || form == inl::fmax_imm ||
      form == inl::fmin_imm) {
    if (!supported)
      return false;
    /* Zdn, Pg/M, Zdn, #imm: bit 5 picks the second constant of the pair. */
    const bool second = field(instruction, 5, 1) != 0U;
    const bool arithmetic = form == inl::fadd_imm || form == inl::fsub_imm ||
                            form == inl::fsubr_imm;
    const bool extremum = !arithmetic && form != inl::fmul_imm;
    const double value = extremum     ? (second ? 1.0 : 0.0)
                         : arithmetic ? (second ? 1.0 : 0.5)
                                      : (second ? 2.0 : 0.5);
    std::uint64_t bits;
    if (size == 1U) {
      bits = value == 0.0 ? 0x0000U
             : value == 0.5 ? 0x3800U
             : value == 1.0 ? 0x3c00U
                            : 0x4000U;
    } else if (size == 2U) {
      bits = value == 0.0 ? 0U
             : value == 0.5 ? 0x3f00'0000U
             : value == 1.0 ? 0x3f80'0000U
                            : 0x4000'0000U;
    } else {
      bits = value == 0.0 ? 0U
             : value == 0.5 ? 0x3fe0'0000'0000'0000ULL
             : value == 1.0 ? 0x3ff0'0000'0000'0000ULL
                            : 0x4000'0000'0000'0000ULL;
    }
    const FloatingOp &op = form == inl::fadd_imm    ? op_fadd
                           : form == inl::fsub_imm  ? op_fsub
                           : form == inl::fsubr_imm ? op_fsub
                           : form == inl::fmul_imm  ? op_fmul
                           : form == inl::fmaxnm_imm ? op_fmaxnm
                           : form == inl::fminnm_imm ? op_fminnm
                           : form == inl::fmax_imm   ? op_fmax
                                                     : op_fmin;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    emit_sve_element_constant(scratch_b, bits, 1U << size);
    const auto first = read_native_vector(d, 0U);
    const auto out = write_native_vector(d, false, 2U);
    const bool reversed = form == inl::fsubr_imm;
    emitter_.emit(three(floating_word(op, size), out.host,
                        reversed ? scratch_b : first.host,
                        reversed ? first.host : scratch_b));
    commit_native_vector(out, d);
    mark_native_fp();
    return true;
  }
  if (form == inl::scvtf_p || form == inl::ucvtf_p || form == inl::fcvtzs_p ||
      form == inl::fcvtzu_p) {
    /* Only the conversions that keep the element size (bits 17-18 name the
     * integer's, bits 22-23 the float's) are one NEON instruction. */
    if (!supported || field(instruction, 17, 2) != size)
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    const auto &op = form == inl::scvtf_p    ? op_scvtf
                     : form == inl::ucvtf_p  ? op_ucvtf
                     : form == inl::fcvtzs_p ? op_fcvtzs
                                             : op_fcvtzu;
    emit_native_vector_unary(three(floating_word(op, size), d, n, 0), false);
    mark_native_fp();
    return true;
  }
  return false;
}

bool JitCompiler::emit_sve_integer(std::uint16_t form,
                                   std::uint32_t instruction,
                                   std::uint64_t pc) {
  const auto &host = host_capabilities();
  const unsigned size = field(instruction, 22, 2);
  const unsigned d = field(instruction, 0, 5);
  const unsigned n = field(instruction, 5, 5);
  const unsigned m = field(instruction, 16, 5);
  const unsigned g = field(instruction, 10, 3);

  /* SQINC, SQDEC, UQINC and UQDEC of Xdn or Wdn: bits 15-12 are 1111. */
  if ((instruction & 0xff20'f000U) == 0x0420'f000U)
    return emit_sve_saturating_count(instruction);

  if (size == 3U && emit_sve_doubleword(form, instruction, pc))
    return true;

  for (const auto &op : destructive_ops) {
    if (op.form != form || op.floating)
      continue;
    if (!op.doubleword && size == 3U)
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    /* Zdn is bits 0-4 and Zm bits 5-9. */
    const auto opcode = op.sized ? sized(op.opcode, size) : op.opcode;
    emit_native_vector_binary(
        three(opcode, d, op.reversed ? n : d, op.reversed ? d : n), false);
    return true;
  }

  switch (form) {
  case jit::sve_forms::orr_immediate:
  case jit::sve_forms::eor_immediate:
  case jit::sve_forms::and_immediate: {
    /* Zdn = Zdn op #bitmask: the 64-bit pattern, whatever the element. */
    const auto value = detail::decode_logical_immediate(
        true, field(instruction, 17, 1), field(instruction, 11, 6),
        field(instruction, 5, 6));
    if (!value)
      return false;
    begin_sve_word();
    emit_sve_vector_constant(scratch_b, *value, *value);
    const auto first = read_native_vector(d, 0U);
    const auto out = write_native_vector(d, false, 2U);
    emitter_.emit(three(form == jit::sve_forms::orr_immediate   ? vector_orr
                        : form == jit::sve_forms::eor_immediate ? vector_eor
                                                                : vector_and,
                        out.host, first.host, scratch_b));
    commit_native_vector(out, d);
    return true;
  }
  case inl::add_u:
  case inl::sub_u: {
    begin_sve_word();
    emit_native_vector_binary(
        three(sized(form == inl::add_u ? 0x4e20'8400U : 0x6e20'8400U, size), d,
              n, m),
        false);
    return true;
  }
  case inl::uqadd_u:
  case inl::uqsub_u: {
    /* The saturating NEON forms set FPSR.QC, which SVE's do not: the
     * wrapped result, then every lane that wrapped replaced. */
    begin_sve_word();
    const bool subtract = form == inl::uqsub_u;
    const auto first = read_native_vector(n, 0U);
    const auto second = m == n ? first : read_native_vector(m, 1U);
    emitter_.emit(vector_add(subtract, size, scratch_a, first.host,
                             second.host));
    emitter_.emit(three(sized(vector_cmhi, size), scratch_b, first.host,
                        subtract ? second.host : scratch_a));
    const auto out = write_native_vector(d, false, 2U);
    emitter_.emit(three(subtract ? vector_and : vector_orr, out.host,
                        scratch_a, scratch_b));
    commit_native_vector(out, d);
    return true;
  }
  case inl::mla_p:
  case inl::mls_p: {
    if (size == 3U)
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    emit_native_vector_accumulate(
        three(sized(form == inl::mla_p ? 0x4e20'9400U : 0x6e20'9400U, size), d,
              n, m),
        true);
    return true;
  }
  case inl::mad_p:
  case inl::msb_p: {
    /* Zdn = Zdn * Zm + Za (MSB: Za - Zdn * Zm); Zm is bits 16-20, Za 5-9. */
    if (size == 3U)
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    const auto accumulator = read_native_vector(n, 0U);
    const auto multiplicand = read_native_vector(d, 1U);
    const auto multiplier = m == d   ? multiplicand
                            : m == n ? accumulator
                                     : read_native_vector(m, 2U);
    emitter_.emit(vector_move(scratch_a, accumulator.host));
    emitter_.emit(three(sized(form == inl::mad_p ? 0x4e20'9400U : 0x6e20'9400U,
                              size),
                        scratch_a, multiplicand.host, multiplier.host));
    const auto out = write_native_vector(d, false, 3U);
    emitter_.emit(vector_move(out.host, scratch_a));
    commit_native_vector(out, d);
    return true;
  }
  case inl::lsl_i:
  case inl::lsr_i:
  case inl::asr_i:
  case inl::sri:
  case inl::usra:
  case inl::ssra: {
    /* tsz:imm3 at bits 22-23, 19-20 and 16-18 is NEON's immh:immb. */
    const unsigned code = shift_code(instruction, 19, 16);
    if (shift_log_size(code) > 3U)
      return false;
    begin_sve_word();
    const std::uint32_t opcode = form == inl::lsl_i   ? 0x4f00'5400U
                                 : form == inl::lsr_i ? 0x6f00'0400U
                                 : form == inl::asr_i ? 0x4f00'0400U
                                 : form == inl::sri   ? 0x6f00'4400U
                                 : form == inl::usra  ? 0x6f00'1400U
                                                      : 0x4f00'1400U;
    emit_native_vector_unary(opcode | (code << 16) | (n << 5) | d,
                             form == inl::sri || form == inl::usra ||
                                 form == inl::ssra);
    return true;
  }
  case inl::urshr_p: {
    /* Zdn, Pg/M, Zdn, #shift: tszl is bits 8-9, imm3 bits 5-7. */
    const unsigned code = shift_code(instruction, 8, 5);
    if (shift_log_size(code) > 3U)
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << shift_log_size(code), instruction, pc);
    emit_native_vector_unary(0x6f00'2400U | (code << 16) | (d << 5) | d,
                             false);
    return true;
  }
  case inl::uqshl_p: {
    /* The NEON form sets FPSR.QC, which SVE's does not: shift left, shift
     * back, and every lane that lost a bit becomes all ones. */
    const unsigned code = shift_code(instruction, 8, 5);
    const unsigned log_size = shift_log_size(code);
    if (log_size > 3U)
      return false;
    const unsigned shift = code - (8U << log_size);
    begin_sve_word();
    emit_sve_guard(g, 1U << log_size, instruction, pc);
    if (shift == 0U)
      return true;
    const auto source = read_native_vector(d, 0U);
    emitter_.emit(0x4f00'5400U | ((8U << log_size) + shift) << 16 |
                  (source.host << 5) | scratch_a); // shl v4, source, #shift
    emitter_.emit(0x6f00'0400U | (((16U << log_size) - shift) << 16) |
                  (scratch_a << 5) | scratch_b); // ushr v5, v4, #shift
    emitter_.emit(three(sized(vector_cmeq, log_size), scratch_b, scratch_b,
                        source.host));
    const auto out = write_native_vector(d, false, 2U);
    emitter_.emit(three(0x4ee0'1c00U, out.host, scratch_a, scratch_b)); // orn
    commit_native_vector(out, d);
    return true;
  }
  case inl::xar: {
    /* Zdn = ROR(Zdn EOR Zm, #shift); Zm is bits 5-9. */
    const unsigned code = shift_code(instruction, 19, 16);
    const unsigned log_size = shift_log_size(code);
    if (log_size > 3U)
      return false;
    begin_sve_word();
    const unsigned bits = 8U << log_size;
    const unsigned rotate = ((2U * bits) - code) % bits;
    const auto first = read_native_vector(d, 0U);
    const auto second = n == d ? first : read_native_vector(n, 1U);
    if (rotate == 0U) {
      const auto out = write_native_vector(d, false, 2U);
      emitter_.emit(three(vector_eor, out.host, first.host, second.host));
      commit_native_vector(out, d);
      return true;
    }
    emitter_.emit(three(vector_eor, scratch_a, first.host, second.host));
    const auto out = write_native_vector(d, false, 2U);
    emitter_.emit(0x4f00'5400U | ((bits + bits - rotate) << 16) |
                  (scratch_a << 5) | out.host); // shl out, v4, #(bits - r)
    emitter_.emit(0x6f00'1400U | (((2U * bits) - rotate) << 16) |
                  (scratch_a << 5) | out.host); // usra out, v4, #r
    commit_native_vector(out, d);
    return true;
  }
  case inl::eor3:
  case inl::bcax: {
    /* Zdn = Zdn EOR Zm EOR Zk, or Zdn EOR (Zm AND NOT Zk); Zm is bits
     * 16-20, Zk bits 5-9. */
    begin_sve_word();
    const auto first = read_native_vector(d, 0U);
    const auto second = m == d ? first : read_native_vector(m, 1U);
    const auto third = n == d   ? first
                       : n == m ? second
                                : read_native_vector(n, 2U);
    const auto out = write_native_vector(d, false, 3U);
    if (host.sha3) {
      emitter_.emit((form == inl::eor3 ? 0xce00'0000U : 0xce20'0000U) |
                    (second.host << 16) | (third.host << 10) |
                    (first.host << 5) | out.host);
    } else {
      if (form == inl::eor3) {
        emitter_.emit(three(vector_eor, scratch_a, second.host, third.host));
      } else {
        emitter_.emit(three(vector_bic, scratch_a, second.host, third.host));
      }
      emitter_.emit(three(vector_eor, out.host, first.host, scratch_a));
    }
    commit_native_vector(out, d);
    return true;
  }
  case inl::bsl: {
    /* Zdn = (Zdn AND Zk) OR (Zm AND NOT Zk): BIF with Zk as the select. */
    begin_sve_word();
    const auto select = read_native_vector(n, 1U);
    const auto other = m == n ? select : read_native_vector(m, 2U);
    const auto out = write_native_vector(d, true, 0U);
    emitter_.emit(three(vector_bif, out.host, other.host, select.host));
    commit_native_vector(out, d);
    return true;
  }
  case inl::addp_p: {
    /* Zdn, Pg/M, Zdn, Zm: element 2i of the result adds Zdn's pair i, and
     * 2i+1 Zm's. AdvSIMD's ADDP puts the first operand's pairs in the low
     * half and the second's in the high one, which ZIP1 of the halves
     * interleaves. */
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    const auto first = read_native_vector(d, 0U);
    const auto second = n == d ? first : read_native_vector(n, 1U);
    const auto out = write_native_vector(d, false, 2U);
    if (size == 3U) {
      emitter_.emit(three(sized(0x4e20'bc00U, size), out.host, first.host,
                          second.host));
    } else {
      emitter_.emit(three(sized(0x4e20'bc00U, size), scratch_a, first.host,
                          second.host));
      emitter_.emit(ext_half(scratch_b, scratch_a, scratch_a));
      emitter_.emit(three(sized(0x4e00'3800U, size), out.host, scratch_a,
                          scratch_b));
    }
    commit_native_vector(out, d);
    return true;
  }
  case inl::usqadd_p: {
    /* Zdn (unsigned) plus Zm (signed), saturating without FPSR.QC: the
     * wrapped sum, then the lanes that overflowed upward (Zm >= 0) set and
     * those that underflowed (Zm < 0) cleared. */
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    const auto x = read_native_vector(d, 0U);
    const auto y = n == d ? x : read_native_vector(n, 1U);
    emitter_.emit(vector_add(false, size, scratch_a, x.host, y.host));
    emitter_.emit(three(sized(vector_cmhi, size), scratch_b, x.host, scratch_a));
    emitter_.emit(three(sized(vector_cmhi, size), 3U, scratch_a, x.host));
    emitter_.emit(sized(0x4e20'a800U, size) | (y.host << 5) | 2U); // cmlt #0
    emitter_.emit(three(vector_bic, scratch_b, scratch_b, 2U));
    emitter_.emit(three(vector_and, 3U, 3U, 2U));
    emitter_.emit(three(vector_orr, scratch_a, scratch_a, scratch_b));
    const auto out = write_native_vector(d, false, 2U);
    emitter_.emit(three(vector_bic, out.host, scratch_a, 3U));
    commit_native_vector(out, d);
    return true;
  }
  case inl::asr_v:
  case inl::lsr_v:
  case inl::lsl_v:
  case inl::asrr_v:
  case inl::lsrr_v:
  case inl::lslr_v: {
    /* The count is the whole element, unsigned; AdvSIMD's is a signed byte.
     * Clamping it to 64 keeps any larger one shifting everything out, and a
     * right shift is a left shift by the negated count. */
    const bool reversed =
        form == inl::asrr_v || form == inl::lsrr_v || form == inl::lslr_v;
    const bool right = form != inl::lsl_v && form != inl::lslr_v;
    const bool arithmetic = form == inl::asr_v || form == inl::asrr_v;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    const unsigned value_register = reversed ? n : d;
    const unsigned count_register = reversed ? d : n;
    const auto value = read_native_vector(value_register, 0U);
    const auto count = count_register == value_register
                           ? value
                           : read_native_vector(count_register, 1U);
    emit_sve_element_constant(scratch_b, 64U, 1U << size);
    unsigned amount = scratch_a;
    if (size == 3U) {
      emitter_.emit(three(sized(vector_cmhi, 3U), 3U, count.host, scratch_b));
      emitter_.emit(three(0x6e60'1c00U, 3U, scratch_b, count.host)); // bsl
      amount = 3U;
    } else {
      emitter_.emit(three(sized(vector_umin, size), scratch_a, count.host,
                          scratch_b));
    }
    if (right)
      emitter_.emit(sized(0x6e20'b800U, size) | (amount << 5) | amount); // neg
    const auto out = write_native_vector(d, false, 2U);
    emitter_.emit(three(sized(arithmetic ? 0x4e20'4400U : 0x6e20'4400U, size),
                        out.host, value.host, amount));
    commit_native_vector(out, d);
    return true;
  }
  case inl::asr_pi:
  case inl::lsr_pi:
  case inl::lsl_pi:
  case inl::asrd_pi: {
    /* Zdn, Pg/M, Zdn, #shift: tszl is bits 8-9, imm3 bits 5-7. */
    const unsigned code = shift_code(instruction, 8, 5);
    const unsigned log_size = shift_log_size(code);
    if (log_size > 3U)
      return false;
    const unsigned bits = 8U << log_size;
    const bool left = form == inl::lsl_pi;
    const unsigned shift = left ? code - bits : 2U * bits - code;
    if (form == inl::asrd_pi && shift == bits)
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << log_size, instruction, pc);
    if (form != inl::asrd_pi) {
      const std::uint32_t opcode = left                 ? 0x4f00'5400U
                                   : form == inl::lsr_pi ? 0x6f00'0400U
                                                        : 0x4f00'0400U;
      emit_native_vector_unary(opcode | (code << 16) | (d << 5) | d, false);
      return true;
    }
    /* Round towards zero: add 2^shift - 1 to a negative value first. */
    const auto source = read_native_vector(d, 0U);
    emitter_.emit(0x4f00'0400U | ((2U * bits - (bits - 1U)) << 16) |
                  (source.host << 5) | scratch_a); // sshr #(bits - 1)
    emitter_.emit(0x6f00'0400U | ((2U * bits - (bits - shift)) << 16) |
                  (scratch_a << 5) | scratch_a); // ushr #(bits - shift)
    emitter_.emit(vector_add(false, log_size, scratch_a, source.host, scratch_a));
    const auto out = write_native_vector(d, false, 2U);
    emitter_.emit(0x4f00'0400U | (code << 16) | (scratch_a << 5) | out.host);
    commit_native_vector(out, d);
    return true;
  }
  case inl::mul_u: {
    if (size == 3U)
      return false;
    begin_sve_word();
    emit_native_vector_binary(three(sized(0x4e20'9c00U, size), d, n, m), false);
    return true;
  }
  case inl::umulh:
  case inl::smulh: {
    /* The high half of each product: the widened products of the two
     * halves, then the odd (high) narrow elements of both. */
    if (size == 3U)
      return false;
    begin_sve_word();
    const bool is_unsigned = form == inl::umulh;
    const auto first = read_native_vector(n, 0U);
    const auto second = m == n ? first : read_native_vector(m, 1U);
    const std::uint32_t low = is_unsigned ? 0x2e20'c000U : 0x0e20'c000U;
    const std::uint32_t high = is_unsigned ? 0x6e20'c000U : 0x4e20'c000U;
    emitter_.emit(three(sized(low, size), scratch_a, first.host, second.host));
    emitter_.emit(three(sized(high, size), scratch_b, first.host,
                        second.host));
    const auto out = write_native_vector(d, false, 2U);
    emitter_.emit(three(sized(0x4e00'5800U, size), out.host, scratch_a,
                        scratch_b)); // uzp2
    commit_native_vector(out, d);
    return true;
  }
  case inl::smullb:
  case inl::smullt:
  case inl::umullb:
  case inl::umullt:
  case inl::smlalb:
  case inl::smlalt:
  case inl::umlalb:
  case inl::umlalt:
  case inl::saddlb:
  case inl::saddlt:
  case inl::uaddlb:
  case inl::uaddlt: {
    /* Widening on the even (bottom) or odd (top) elements: UZP1 or UZP2
     * of each input narrows them to the low half, the NEON long form
     * widens. */
    if (size == 0U)
      return false;
    begin_sve_word();
    const bool top = form == inl::smullt || form == inl::umullt ||
                     form == inl::smlalt || form == inl::umlalt ||
                     form == inl::saddlt || form == inl::uaddlt;
    const bool accumulate = form == inl::smlalb || form == inl::smlalt ||
                            form == inl::umlalb || form == inl::umlalt;
    const bool is_unsigned = form == inl::umullb || form == inl::umullt ||
                             form == inl::umlalb || form == inl::umlalt ||
                             form == inl::uaddlb || form == inl::uaddlt;
    const bool add = form == inl::saddlb || form == inl::saddlt ||
                     form == inl::uaddlb || form == inl::uaddlt;
    std::uint32_t opcode;
    if (add)
      opcode = is_unsigned ? 0x2e20'0000U : 0x0e20'0000U;
    else if (accumulate)
      opcode = is_unsigned ? 0x2e20'8000U : 0x0e20'8000U;
    else
      opcode = is_unsigned ? 0x2e20'c000U : 0x0e20'c000U;
    const std::uint32_t unzip = top ? 0x4e00'5800U : 0x4e00'1800U;
    const auto first = read_native_vector(n, 0U);
    const auto second = m == n ? first : read_native_vector(m, 1U);
    emitter_.emit(three(sized(unzip, size - 1U), scratch_a, first.host,
                        first.host));
    unsigned right = scratch_a;
    if (m != n) {
      emitter_.emit(three(sized(unzip, size - 1U), scratch_b, second.host,
                          second.host));
      right = scratch_b;
    }
    const auto out = write_native_vector(d, accumulate, 2U);
    emitter_.emit(three(sized(opcode, size - 1U), out.host, scratch_a, right));
    commit_native_vector(out, d);
    return true;
  }
  case inl::addhnb:
  case inl::addhnt:
  case inl::uqshrnb:
  case inl::uqshrnt: {
    /* The narrowed value (ADDHN, or a logical shift right clamped to the
     * narrow range, as UQSHRN would set FPSR.QC) goes to the even (B) or
     * odd (T) narrow elements; B zeroes the others, T keeps them. */
    const bool top = form == inl::addhnt || form == inl::uqshrnt;
    const bool add = form == inl::addhnb || form == inl::addhnt;
    /* log2 of the wide source element: the SVE size field for ADDHN, one
     * above the tsz's narrow element for UQSHRN. */
    unsigned wide;
    unsigned shift = 0;
    if (add) {
      wide = size;
    } else {
      const unsigned code = shift_code(instruction, 19, 16);
      const unsigned narrow_size = shift_log_size(code);
      if (narrow_size > 1U)
        return false; // a doubleword source has no NEON UMIN
      wide = narrow_size + 1U;
      shift = (16U << narrow_size) - code;
    }
    if (wide == 0U || wide > 3U || (!add && wide == 3U))
      return false;
    begin_sve_word();
    const unsigned narrow_size = wide - 1U;
    const auto source = read_native_vector(n, 0U);
    if (add) {
      const auto second = m == n ? source : read_native_vector(m, 1U);
      emitter_.emit(three(sized(0x0e20'4000U, narrow_size), scratch_a,
                          source.host, second.host));
    } else {
      emitter_.emit(0x6f00'0400U | (((16U << wide) - shift) << 16) |
                    (source.host << 5) | scratch_a); // ushr v4, source, #shift
      /* movi v5, #0xff or #0xffff: the narrow element's maximum. */
      emitter_.emit(wide == 1U ? 0x4f07'87e5U : 0x4f07'c7e5U);
      emitter_.emit(three(sized(vector_umin, wide), scratch_a, scratch_a,
                          scratch_b));
      emitter_.emit(narrow(narrow_size, scratch_a, scratch_a));
    }
    if (!top) {
      const auto out = write_native_vector(d, false, 2U);
      emitter_.emit(extend_long(false, narrow_size, out.host, scratch_a));
      commit_native_vector(out, d);
      return true;
    }
    const auto previous = write_native_vector(d, true, 2U);
    emitter_.emit(three(sized(0x4e00'1800U, narrow_size), scratch_b,
                        previous.host, previous.host)); // uzp1: even elements
    emitter_.emit(three(sized(0x4e00'3800U, narrow_size), previous.host,
                        scratch_b, scratch_a)); // zip1: even, new
    commit_native_vector(previous, d);
    return true;
  }
  case inl::sdot:
  case inl::udot:
  case inl::sdot_i:
  case inl::udot_i: {
    const bool is_unsigned = form == inl::udot || form == inl::udot_i;
    const bool indexed = form == inl::sdot_i || form == inl::udot_i;
    if (size < 2U)
      return false;
    if (size == 2U) {
      /* Four bytes into each word: the dot-product instruction. The
       * indexed form's Zm is 3 bits and its index (L:H of NEON's) the 2
       * bits above. */
      if (!host.has(ArmFeature::dotprod))
        return false;
      begin_sve_word();
      if (!indexed) {
        emit_native_vector_accumulate(
            three(is_unsigned ? 0x6e80'9400U : 0x4e80'9400U, d, n, m), true);
        return true;
      }
      const unsigned index = field(instruction, 19, 2);
      emit_native_vector_by_element(
          (is_unsigned ? 0x6f80'e000U : 0x4f80'e000U) | ((index & 1U) << 21) |
          ((index >> 1) << 11) | (field(instruction, 16, 3) << 16) | (n << 5) |
          d);
      return true;
    }
    /* Four halfwords into each doubleword, which AdvSIMD lacks: the products
     * of each half of the vector, pairs added into doublewords, and the two
     * halves' sums added. */
    begin_sve_word();
    const auto first = read_native_vector(n, 0U);
    unsigned second;
    if (indexed) {
      /* The 4 halfwords at index (bit 20) of Zm (4 bits) for every group. */
      const auto selected = read_native_vector(field(instruction, 16, 4), 1U);
      emitter_.emit(dup_element(3U, field(instruction, 20, 1), 3U,
                                selected.host));
      second = 3U;
    } else {
      second = (m == n ? first : read_native_vector(m, 1U)).host;
    }
    const std::uint32_t low = is_unsigned ? 0x2e60'c000U : 0x0e60'c000U;
    const std::uint32_t high = is_unsigned ? 0x6e60'c000U : 0x4e60'c000U;
    const std::uint32_t pairs = is_unsigned ? 0x6ea0'2800U : 0x4ea0'2800U;
    emitter_.emit(three(low, scratch_a, first.host, second));
    emitter_.emit(three(high, scratch_b, first.host, second));
    emitter_.emit(pairs | (scratch_a << 5) | scratch_a);
    emitter_.emit(pairs | (scratch_b << 5) | scratch_b);
    emitter_.emit(three(0x4ee0'bc00U, scratch_a, scratch_a, scratch_b)); // addp
    const auto out = write_native_vector(d, true, 2U);
    emitter_.emit(vector_add(false, 3U, out.host, out.host, scratch_a));
    commit_native_vector(out, d);
    return true;
  }
  default:
    return false;
  }
}

bool JitCompiler::emit_sve_permute(std::uint16_t form,
                                   std::uint32_t instruction,
                                   std::uint64_t pc) {
  const unsigned size = field(instruction, 22, 2);
  const unsigned d = field(instruction, 0, 5);
  const unsigned n = field(instruction, 5, 5);
  const unsigned m = field(instruction, 16, 5);
  const unsigned g = field(instruction, 10, 3);

  switch (form) {
  case inl::zip:
  case inl::uzp:
  case inl::trn: {
    begin_sve_word();
    const std::uint32_t second = field(instruction, 10, 1) << 14;
    const std::uint32_t base = form == inl::zip   ? 0x4e00'3800U
                               : form == inl::uzp ? 0x4e00'1800U
                                                  : 0x4e00'2800U;
    /* ZIP2 and TRN2 differ from ZIP1 and TRN1 in bit 14 (UZP2 in bit 14
     * too): 0x3800 -> 0x7800, 0x2800 -> 0x6800, 0x1800 -> 0x5800. */
    emit_native_vector_binary(three(sized(base | second, size), d, n, m),
                              false);
    return true;
  }
  case inl::tbl: {
    /* TBL Zd.B, {Zn.B}, Zm.B; the SVE2 TBL with two registers, and TBX,
     * which keeps Zd for an index out of range. */
    const unsigned operation = field(instruction, 10, 6);
    if (size != 0U) {
      /* AdvSIMD indexes bytes: an element's index becomes the indices of
       * its bytes (index * element size + 0, 1, ...) and an index past the
       * last element, which reads zero, becomes bytes AdvSIMD also reads
       * as zero. */
      if (operation != 0x0cU)
        return false;
      begin_sve_word();
      const unsigned elements = sve_vector_length >> size;
      const auto table = read_native_vector(n, 0U);
      const auto index = read_native_vector(m, 1U);
      std::uint64_t offsets = 0;
      for (unsigned byte = 0; byte < (1U << size); ++byte)
        offsets |= static_cast<std::uint64_t>(byte) << (8U * byte);
      if (size == 3U) {
        /* No 64-bit minimum or multiply: an index past the last element
         * becomes all ones, the low byte of the index is repeated over
         * its element's bytes, and each byte is scaled by eight. */
        emit_sve_element_constant(scratch_a, elements - 1U, 8U);
        emitter_.emit(three(sized(vector_cmhi, 3U), scratch_a, index.host,
                            scratch_a));
        emitter_.emit(three(vector_orr, scratch_a, scratch_a, index.host));
        emit_sve_vector_constant(scratch_b, 0U, 0x0808'0808'0808'0808ULL);
        emitter_.emit(three(0x4e00'0000U, scratch_a, scratch_a, scratch_b));
        emitter_.emit(0x4f00'5400U | ((8U + 3U) << 16) | (scratch_a << 5) |
                      scratch_a); // shl #3
      } else {
        /* The index, clamped to one past the last element, times the
         * element size in every byte of its element. */
        std::uint64_t scale = 0;
        for (unsigned byte = 0; byte < (1U << size); ++byte)
          scale = (scale << 8) | (1U << size);
        emit_sve_element_constant(scratch_b, elements, 1U << size);
        emitter_.emit(three(sized(vector_umin, size), scratch_a, index.host,
                            scratch_b));
        emit_sve_element_constant(scratch_b, scale, 1U << size);
        emitter_.emit(three(sized(0x4e20'9c00U, size), scratch_a, scratch_a,
                            scratch_b)); // mul
      }
      emit_sve_element_constant(scratch_b, offsets, 1U << size);
      emitter_.emit(vector_add(false, 0U, scratch_a, scratch_a, scratch_b));
      const auto out = write_native_vector(d, false, 2U);
      emitter_.emit(three(0x4e00'0000U, out.host, table.host, scratch_a));
      commit_native_vector(out, d);
      return true;
    }
    if (operation == 0x0cU) {
      begin_sve_word();
      emit_native_vector_binary(three(0x4e00'0000U, d, n, m), false);
      return true;
    }
    if (operation == 0x0bU) {
      begin_sve_word();
      emit_native_vector_binary(three(0x4e00'1000U, d, n, m), true);
      return true;
    }
    if (operation != 0x0aU)
      return false;
    begin_sve_word();
    const auto low = read_native_vector(n, 0U);
    const auto high = read_native_vector((n + 1U) & 31U, 1U);
    emitter_.emit(vector_move(scratch_a, low.host));
    emitter_.emit(vector_move(scratch_b, high.host));
    const auto index = read_native_vector(m, 2U);
    const auto out = write_native_vector(d, false, 3U);
    emitter_.emit(0x4e00'2000U | (index.host << 16) | (scratch_a << 5) |
                  out.host);
    commit_native_vector(out, d);
    return true;
  }
  case inl::ext: {
    /* EXT Zdn, Zdn, Zm, #imm (bit 22 clear), or the SVE2 EXT Zd, {Zn1,
     * Zn2}, #imm: a position of 16 or more selects 0. */
    const unsigned position =
        (field(instruction, 16, 5) << 3) | field(instruction, 10, 3);
    const std::uint32_t opcode =
        0x6e00'0000U | ((position >= 16U ? 0U : position) << 11);
    begin_sve_word();
    if (field(instruction, 22, 1) == 0U) {
      emit_native_vector_binary(opcode | (n << 16) | (d << 5) | d, false);
      return true;
    }
    const auto low = read_native_vector(n, 0U);
    const auto high = read_native_vector((n + 1U) & 31U, 1U);
    const auto out = write_native_vector(d, false, 2U);
    emitter_.emit(opcode | (high.host << 16) | (low.host << 5) | out.host);
    commit_native_vector(out, d);
    return true;
  }
  case inl::rev: {
    /* REVB, REVH, REVW and RBIT (bits 16-17), Zd, Pg/M, Zn. */
    const unsigned operation = field(instruction, 16, 2);
    if (operation == 3U || size <= operation)
      return false;
    begin_sve_word();
    emit_sve_guard(g, 1U << size, instruction, pc);
    /* REVB reverses bytes within size-wide elements (REV16, 32, 64 on
     * bytes), REVH halfwords within words or doublewords (on halves), REVW
     * words within doublewords (on words). */
    std::uint32_t word;
    if (operation == 0U) {
      word = size == 1U   ? 0x4e20'1800U
             : size == 2U ? 0x6e20'0800U
                          : 0x4e20'0800U;
    } else if (operation == 1U) {
      word = size == 2U ? 0x6e60'0800U : 0x4e60'0800U;
    } else {
      word = 0x4ea0'0800U;
    }
    emit_native_vector_unary(word | (n << 5) | d, false);
    return true;
  }
  case inl::index_ii:
  case inl::index_ir:
  case inl::index_ri:
  case inl::index_rr:
    return emit_sve_index(form, instruction);
  default:
    return false;
  }
}

bool JitCompiler::emit_sve_index(std::uint16_t form,
                                 std::uint32_t instruction) {
  /* INDEX Zd.T, <base>, <step>: lane i is base + i * step, each an
   * immediate (bits 5-9 and 16-20, signed) or a register (Wn or Xn). */
  const unsigned size = field(instruction, 22, 2);
  const unsigned esize = 1U << size;
  const unsigned elements = sve_vector_length / esize;
  const unsigned d = field(instruction, 0, 5);
  const bool base_immediate = form == inl::index_ii || form == inl::index_ir;
  const bool step_immediate = form == inl::index_ii || form == inl::index_ri;
  const auto base_value = sign_extend(field(instruction, 5, 5), 5);
  const auto step_value = sign_extend(field(instruction, 16, 5), 5);
  begin_sve_word();
  const auto element_mask =
      esize == 8U ? ~std::uint64_t{0}
                  : (std::uint64_t{1} << (esize * 8U)) - 1U;
  const auto lane = [&](unsigned index) {
    return (static_cast<std::uint64_t>(base_value) +
            index * static_cast<std::uint64_t>(step_value)) &
           element_mask;
  };
  if (base_immediate && step_immediate) {
    std::uint64_t half[2]{};
    for (unsigned index = 0; index < elements; ++index)
      half[(index * esize) / 8U] |= lane(index) << (((index * esize) % 8U) * 8U);
    const auto out = write_native_vector(d, false, 0U);
    emit_sve_vector_constant(out.host, half[0], half[1]);
    commit_native_vector(out, d);
    return true;
  }
  const unsigned base_register = field(instruction, 5, 5);
  const unsigned step_register = field(instruction, 16, 5);
  if (esize == 8U) {
    /* Lane 0 is the base, lane 1 base + step, in x registers. */
    unsigned base_host = 8U;
    unsigned step_host = 9U;
    if (base_immediate)
      emitter_.load_constant(8, static_cast<std::uint64_t>(base_value));
    else
      base_host = read_native_gpr(base_register, false, 8U).host;
    if (step_immediate)
      emitter_.load_constant(9, static_cast<std::uint64_t>(step_value));
    else
      step_host = read_native_gpr(step_register, false, 9U).host;
    emitter_.emit(add_x(10, base_host, step_host));
    emitter_.emit(fmov_d_x(scratch_a, base_host));
    emitter_.emit(ins_high(scratch_a, 10));
    const auto out = write_native_vector(d, false, 0U);
    emitter_.emit(vector_move(out.host, scratch_a));
    commit_native_vector(out, d);
    return true;
  }
  /* B, H and S: the lane numbers times the step, plus the base. */
  std::uint64_t iota[2]{};
  for (unsigned index = 0; index < elements; ++index)
    iota[(index * esize) / 8U] |= std::uint64_t{index}
                                  << (((index * esize) % 8U) * 8U);
  if (step_immediate) {
    std::uint64_t scaled[2]{};
    for (unsigned index = 0; index < elements; ++index)
      scaled[(index * esize) / 8U] |=
          ((index * static_cast<std::uint64_t>(step_value)) & element_mask)
          << (((index * esize) % 8U) * 8U);
    emit_sve_vector_constant(scratch_a, scaled[0], scaled[1]);
  } else {
    const auto step = read_native_gpr(step_register, false, 9U);
    emitter_.emit(dup_general(size, scratch_b, step.host));
    emit_sve_vector_constant(scratch_a, iota[0], iota[1]);
    emitter_.emit(three(sized(0x4e20'9c00U, size), scratch_a, scratch_a,
                        scratch_b)); // mul
  }
  if (base_immediate) {
    if (base_value != 0) {
      emit_sve_element_constant(scratch_b,
                                static_cast<std::uint64_t>(base_value) &
                                    element_mask,
                                esize);
      const auto out = write_native_vector(d, false, 0U);
      emitter_.emit(vector_add(false, size, out.host, scratch_a, scratch_b));
      commit_native_vector(out, d);
      return true;
    }
  } else {
    const auto base = read_native_gpr(base_register, false, 8U);
    emitter_.emit(dup_general(size, scratch_b, base.host));
    const auto out = write_native_vector(d, false, 0U);
    emitter_.emit(vector_add(false, size, out.host, scratch_a, scratch_b));
    commit_native_vector(out, d);
    return true;
  }
  const auto out = write_native_vector(d, false, 0U);
  emitter_.emit(vector_move(out.host, scratch_a));
  commit_native_vector(out, d);
  return true;
}

bool JitCompiler::emit_sve_compare(std::uint16_t form,
                                   std::uint32_t instruction) {
  /*
   * CMP<cc> Pd.T, Pg/Z, Zn.T, <Zm.T | #imm>, flags from PredTest(Pg, Pd).
   * The NEON compare gives all-ones lanes; the predicate keeps only each
   * element's lowest byte (the significant one), under Pg, and the flags
   * are those of the same two canonical images. No lane is read that the
   * result depends on beyond Pg's, and an integer compare raises nothing,
   * so no governing-predicate check is needed.
   */
  enum class Condition { eq, ne, ge, gt, hs, hi, lt, le, lo, ls };
  const unsigned size = field(instruction, 22, 2);
  const unsigned esize = 1U << size;
  const unsigned d = field(instruction, 0, 4);
  const unsigned g = field(instruction, 10, 3);
  const unsigned n = field(instruction, 5, 5);
  const bool immediate = form == inl::cmp_immediate;
  Condition condition;
  std::uint64_t value = 0;
  const unsigned operation =
      (field(instruction, 13, 3) << 1) | field(instruction, 4, 1);
  if (!immediate) {
    switch (operation) {
    case 0: condition = Condition::hs; break;
    case 1: condition = Condition::hi; break;
    case 8: condition = Condition::ge; break;
    case 9: condition = Condition::gt; break;
    case 10: condition = Condition::eq; break;
    case 11: condition = Condition::ne; break;
    default: return false;
    }
  } else if (field(instruction, 24, 1) != 0U) {
    /* A signed 5-bit immediate. */
    switch (operation) {
    case 0: condition = Condition::ge; break;
    case 1: condition = Condition::gt; break;
    case 2: condition = Condition::lt; break;
    case 3: condition = Condition::le; break;
    case 8: condition = Condition::eq; break;
    case 9: condition = Condition::ne; break;
    default: return false;
    }
    value = static_cast<std::uint64_t>(sign_extend(field(instruction, 16, 5), 5));
  } else {
    /* An unsigned 7-bit immediate: bit 13 and bit 4 select the condition. */
    switch (field(instruction, 13, 1) * 2U + field(instruction, 4, 1)) {
    case 0: condition = Condition::hs; break;
    case 1: condition = Condition::hi; break;
    case 2: condition = Condition::lo; break;
    default: condition = Condition::ls; break;
    }
    value = field(instruction, 14, 7);
  }
  begin_sve_word();
  const auto element_mask = esize == 8U
                                ? ~std::uint64_t{0}
                                : (std::uint64_t{1} << (esize * 8U)) - 1U;
  const auto first = read_native_vector(n, 0U);
  unsigned second;
  if (immediate) {
    emit_sve_element_constant(scratch_b, value & element_mask, esize);
    second = scratch_b;
  } else {
    second = field(instruction, 16, 5) == n
                 ? first.host
                 : read_native_vector(field(instruction, 16, 5), 1U).host;
  }
  const bool swapped = condition == Condition::lt ||
                       condition == Condition::le ||
                       condition == Condition::lo || condition == Condition::ls;
  std::uint32_t opcode;
  switch (condition) {
  case Condition::eq:
  case Condition::ne: opcode = 0x6e20'8c00U; break;
  case Condition::ge:
  case Condition::le: opcode = 0x4e20'3c00U; break;
  case Condition::gt:
  case Condition::lt: opcode = 0x4e20'3400U; break;
  case Condition::hs:
  case Condition::ls: opcode = 0x6e20'3c00U; break;
  default: opcode = 0x6e20'3400U; break;
  }
  emitter_.emit(three(sized(opcode, size), scratch_a,
                      swapped ? second : first.host,
                      swapped ? first.host : second));
  if (condition == Condition::ne)
    emitter_.emit(0x6e20'5800U | (scratch_a << 5) | scratch_a); // not
  const auto governing = read_native_predicate(g, 2U);
  emitter_.emit(three(vector_and, scratch_a, scratch_a, governing.host));
  const auto out = write_native_predicate(d, false, 3U);
  unsigned mask = governing.host;
  if (esize != 1U) {
    emit_sve_element_constant(scratch_b, 0xffU, esize);
    emitter_.emit(three(vector_and, scratch_a, scratch_a, scratch_b));
    emitter_.emit(three(vector_and, out.host, governing.host, scratch_b));
    mask = out.host;
  }
  emit_sve_predicate_test(mask, scratch_a);
  emitter_.emit(vector_move(out.host, scratch_a));
  commit_native_predicate(out, d);
  return true;
}

bool JitCompiler::emit_sve_memory(const detail::SmeEncoding &row,
                                  std::uint32_t instruction,
                                  std::uint64_t pc) {
  using Shape = detail::SmeMemoryShape;
  const auto &memory = *detail::sme_encoding_memory_of(row);
  if (memory.shape == Shape::prefetch ||
      memory.shape == Shape::gather_prefetch) {
    /* A prefetch moves nothing and cannot fault. */
    begin_sve_word();
    return true;
  }
  if (memory.shape == Shape::gather)
    return emit_sve_gather(row, instruction, pc);
  if (memory.shape == Shape::fill_p)
    return emit_sve_predicate_fill(row, instruction, pc);
  if (memory.shape != Shape::contiguous && memory.shape != Shape::structure &&
      memory.shape != Shape::replicate &&
      memory.shape != Shape::replicate_quad && memory.shape != Shape::fill_z)
    return false;
  if (memory.shape == Shape::contiguous && memory.registers != 1U)
    return false;
  const auto *operands = detail::sme_encoding_operands_of(row);
  const bool store = memory.direction == detail::SmeMemoryDirection::store;
  const unsigned esize = jit::sme_memory_element_bytes(memory, instruction);
  const unsigned msize = memory.msize / 8U;
  const bool structure = memory.shape == Shape::structure;
  if (msize > esize || (structure && msize != esize) ||
      (store && msize != esize && memory.shape != Shape::contiguous))
    return false;

  /* The register operands: the transfer Z (the first of a structure's) and
   * the governing predicate, if the form has one. */
  unsigned transfer = 0;
  unsigned governing = 0;
  bool predicated = false;
  for (unsigned index = 0; index < row.operand_count; ++index) {
    if (index == memory.base_operand || index == memory.index_operand)
      continue;
    if (operands[index].kind == detail::SmeOperandKind::z)
      transfer = detail::sme_operand_number(operands[index], instruction);
    else if (operands[index].kind == detail::SmeOperandKind::p) {
      governing = detail::sme_operand_number(operands[index], instruction);
      predicated = true;
    }
  }
  const unsigned base =
      detail::sme_operand_number(operands[memory.base_operand], instruction);
  const bool indexed = memory.offset == detail::SmeAddressOffset::register_element;
  const unsigned index_register =
      indexed ? detail::sme_operand_number(operands[memory.index_operand],
                                           instruction)
              : 0U;
  const auto bytes = static_cast<std::size_t>(
      jit::sme_memory_span(memory, instruction, sve_vector_length));
  const auto access = store ? Access::write : Access::read;
  const unsigned log_esize = static_cast<unsigned>(std::countr_zero(esize));
  const unsigned log_msize = static_cast<unsigned>(std::countr_zero(msize));

  if (store)
    mark_direct_memory_write();
  const auto slow = cold_exit(instruction, pc, ColdPath::Action::sve_slow);
  begin_sve_word();
  if (predicated)
    emit_sve_full_guard(governing, esize, slow);
  load_gpr(8, base, true);
  /* A misaligned SP base and a tagged base take the library, which reports
   * the alignment fault or untags the address. */
  if (base == 31U) {
    emit_misalignment_check(8, 16, slow);
  } else {
    emitter_.load_b(0, 15, offsetof(CpuState, tbi_enabled));
    emitter_.compare_zero(slow, 0, false, true);
  }
  if (indexed) {
    const auto index = read_native_gpr(index_register, false, 9U);
    emitter_.add_shifted(8, 8, index.host, log_msize);
  } else {
    const auto offset = static_cast<std::int64_t>(
        jit::sme_memory_offset(memory, instruction, sve_vector_length, 0));
    if (offset != 0) {
      const auto magnitude =
          static_cast<std::uint64_t>(offset < 0 ? -offset : offset);
      if (magnitude <= 0xfffU) {
        if (offset < 0)
          emitter_.subtract_immediate(8, 8, static_cast<unsigned>(magnitude));
        else
          emitter_.add_immediate(8, 8, static_cast<unsigned>(magnitude));
      } else {
        emitter_.load_constant(9, magnitude);
        emitter_.emit(offset < 0 ? sub_x(8, 8, 9) : add_x(8, 8, 9));
      }
    }
  }
  emit_translate(8, access, slow, slow, bytes);
  if (store)
    track_direct_store(8, bytes);

  switch (memory.shape) {
  case Shape::replicate: {
    const auto out = write_native_vector(transfer, false, 0U);
    if (msize == esize) {
      emitter_.emit(ld1r(log_esize, out.host, 8));
    } else {
      emitter_.emit(load_scalar(log_msize, memory.sign_extend, esize == 8U, 9, 8));
      emitter_.emit(dup_general(log_esize, out.host, 9));
    }
    commit_native_vector(out, transfer);
    break;
  }
  case Shape::structure: {
    const unsigned registers = memory.registers;
    if (!store) {
      emitter_.emit(neon_structure(true, registers, log_esize, 0, 8));
      for (unsigned part = 0; part < registers; ++part) {
        const unsigned guest = (transfer + part) & 31U;
        const auto out = write_native_vector(guest, false, scratch_a);
        emitter_.emit(vector_move(out.host, part));
        commit_native_vector(out, guest);
      }
    } else {
      for (unsigned part = 0; part < registers; ++part) {
        const auto source = read_native_vector((transfer + part) & 31U,
                                               scratch_a);
        emitter_.emit(vector_move(part, source.host));
      }
      emitter_.emit(neon_structure(false, registers, log_esize, 0, 8));
    }
    break;
  }
  default: {
    /* A contiguous load or store of one register, LD1RQ's quadword and
     * LDR and STR of a vector: 16 bytes, or 16 / 2^steps when the elements
     * widen on a load or narrow on a store. */
    const unsigned steps = log_esize - log_msize;
    const unsigned width = 4U - steps;
    if (!store) {
      const auto out = write_native_vector(transfer, false, 0U);
      emitter_.emit(ldr_simd(width, out.host, 8));
      for (unsigned step = 0; step < steps; ++step)
        emitter_.emit(extend_long(memory.sign_extend, log_msize + step,
                                  out.host, out.host));
      commit_native_vector(out, transfer);
    } else {
      const auto source = read_native_vector(transfer, 0U);
      unsigned from = source.host;
      for (unsigned step = 0; step < steps; ++step) {
        emitter_.emit(narrow(log_esize - 1U - step, scratch_a, from));
        from = scratch_a;
      }
      emitter_.emit(str_simd(width, from, 8));
    }
    break;
  }
  }
  if (store)
    clear_local_exclusive_monitor();
  return true;
}

bool JitCompiler::emit_sve_gather(const detail::SmeEncoding &row,
                                  std::uint32_t instruction,
                                  std::uint64_t pc) {
  using Offset = detail::SmeAddressOffset;
  const auto &memory = *detail::sme_encoding_memory_of(row);
  const bool store = memory.direction == detail::SmeMemoryDirection::store;
  /* A tracked block records one store range per instruction, and a scatter
   * writes up to four; the library, which invalidates each, runs it. The
   * word still counts as a direct store, so that the tracked translation
   * and the untracked one agree on which instructions store. */
  if (store && track_direct_stores_) {
    mark_direct_memory_write();
    return false;
  }
  const auto *operands = detail::sme_encoding_operands_of(row);
  const unsigned esize = jit::sme_memory_element_bytes(memory, instruction);
  const unsigned msize = memory.msize / 8U;
  if (msize > esize)
    return false;
  const unsigned elements = sve_vector_length / esize;

  unsigned transfer = 0;
  unsigned governing = 0;
  for (unsigned index = 0; index < row.operand_count; ++index) {
    if (index == memory.base_operand || index == memory.index_operand)
      continue;
    if (operands[index].kind == detail::SmeOperandKind::z)
      transfer = detail::sme_operand_number(operands[index], instruction);
    else if (operands[index].kind == detail::SmeOperandKind::p)
      governing = detail::sme_operand_number(operands[index], instruction);
  }
  const auto &base_operand = operands[memory.base_operand];
  const bool vector_base = base_operand.kind == detail::SmeOperandKind::z;
  const unsigned base_number =
      detail::sme_operand_number(base_operand, instruction);
  const bool sp_base = !vector_base && base_number == 31U;
  const bool vector_index = memory.offset == Offset::vector_index;
  const unsigned index_number =
      memory.index_operand == detail::sme_no_operand
          ? 0U
          : detail::sme_operand_number(operands[memory.index_operand],
                                       instruction);
  const unsigned log_esize = static_cast<unsigned>(std::countr_zero(esize));
  const unsigned log_msize = static_cast<unsigned>(std::countr_zero(msize));
  const unsigned steps = log_esize - log_msize;
  const unsigned index_bits = memory.index_bits;
  const bool index_signed =
      vector_index && index_bits < 64U &&
      detail::sme_segment_value(memory.index_sign, instruction) != 0U;
  const auto immediate = static_cast<std::int64_t>(
      jit::sme_memory_offset(memory, instruction, sve_vector_length, 0));
  const auto access = store ? Access::write : Access::read;

  if (store)
    mark_direct_memory_write();
  const auto slow = cold_exit(instruction, pc, ColdPath::Action::sve_slow);
  begin_sve_word();
  emit_sve_full_guard(governing, esize, slow);

  /* The registers an address is built from stay put through every element:
   * a translation here never calls the bridge, so it clobbers only x0-x2 and
   * x9, and a miss leaves the block for the library. */
  unsigned base_host = 0;
  if (!vector_base) {
    base_host = read_native_gpr(base_number, true, 10U).host;
    if (sp_base) {
      emit_misalignment_check(base_host, 16, slow);
    } else {
      emitter_.load_b(0, 15, offsetof(CpuState, tbi_enabled));
      emitter_.compare_zero(slow, 0, false, true);
    }
  } else {
    emitter_.load_b(0, 15, offsetof(CpuState, tbi_enabled));
    emitter_.compare_zero(slow, 0, false, true);
  }
  unsigned vector_host = 0;
  if (vector_base)
    vector_host = read_native_vector(base_number, 1U).host;
  else if (vector_index)
    vector_host = read_native_vector(index_number, 1U).host;
  unsigned offset_host = 31;
  if (memory.offset == Offset::register_byte && index_number != 31U)
    offset_host = read_native_gpr(index_number, false, 11U).host;
  unsigned source = scratch_a;
  if (store) {
    source = read_native_vector(transfer, 0U).host;
    for (unsigned step = 0; step < steps; ++step) {
      emitter_.emit(narrow(log_esize - 1U - step, scratch_a, source));
      source = scratch_a;
    }
  }

  for (unsigned element = 0; element < elements; ++element) {
    if (vector_base) {
      emitter_.emit(esize == 8U ? umov_d(8, vector_host, element)
                                : umov_s(8, vector_host, element));
      if (memory.offset == Offset::immediate_element && immediate != 0)
        emitter_.add_immediate(8, 8, static_cast<unsigned>(immediate));
      else if (offset_host != 31U)
        emitter_.emit(add_x(8, 8, offset_host));
    } else {
      /* A 32-bit offset is the low word of its element. */
      if (index_bits == 64U) {
        emitter_.emit(umov_d(9, vector_host, element));
        emitter_.add_shifted(8, base_host, 9, memory.index_shift);
      } else {
        emitter_.emit(umov_s(9, vector_host, esize == 8U ? 2U * element
                                                          : element));
        emitter_.emit(add_extended_word(index_signed, 8, base_host, 9,
                                        memory.index_shift));
      }
    }
    emit_translate(8, access, slow, slow, msize, false);
    emitter_.emit(lane_memory(!store, log_msize, element, source, 8));
  }

  if (store) {
    clear_local_exclusive_monitor();
    return true;
  }
  const auto out = write_native_vector(transfer, false, 0U);
  if (steps == 0U) {
    emitter_.emit(vector_move(out.host, scratch_a));
  } else {
    emitter_.emit(
        extend_long(memory.sign_extend, log_msize, out.host, scratch_a));
    for (unsigned step = 1; step < steps; ++step)
      emitter_.emit(extend_long(memory.sign_extend, log_msize + step, out.host,
                                out.host));
  }
  commit_native_vector(out, transfer);
  return true;
}

bool JitCompiler::emit_sve_predicate_fill(const detail::SmeEncoding &row,
                                          std::uint32_t instruction,
                                          std::uint64_t pc) {
  const auto &memory = *detail::sme_encoding_memory_of(row);
  const auto *operands = detail::sme_encoding_operands_of(row);
  const bool store = memory.direction == detail::SmeMemoryDirection::store;
  /* The 16 bits of the predicate, one per byte of the vector; the base is
   * a register, the offset a multiple of the two bytes. */
  constexpr std::size_t bytes = sve_vector_length / 8U;
  unsigned predicate = 0;
  for (unsigned index = 0; index < row.operand_count; ++index)
    if (index != memory.base_operand &&
        operands[index].kind == detail::SmeOperandKind::p)
      predicate = detail::sme_operand_number(operands[index], instruction);
  const unsigned base =
      detail::sme_operand_number(operands[memory.base_operand], instruction);
  const auto offset = static_cast<std::int64_t>(
      jit::sme_memory_offset(memory, instruction, sve_vector_length, 0));

  if (store)
    mark_direct_memory_write();
  const auto slow = cold_exit(instruction, pc, ColdPath::Action::sve_slow);
  begin_sve_word();
  load_gpr(8, base, true);
  if (base == 31U) {
    emit_misalignment_check(8, 16, slow);
  } else {
    emitter_.load_b(0, 15, offsetof(CpuState, tbi_enabled));
    emitter_.compare_zero(slow, 0, false, true);
  }
  if (offset != 0) {
    const auto magnitude =
        static_cast<std::uint64_t>(offset < 0 ? -offset : offset);
    if (offset < 0)
      emitter_.subtract_immediate(8, 8, static_cast<unsigned>(magnitude));
    else
      emitter_.add_immediate(8, 8, static_cast<unsigned>(magnitude));
  }
  emit_translate(8, store ? Access::write : Access::read, slow, slow, bytes);
  if (store)
    track_direct_store(8, bytes);

  /* Byte i of the mask form is all ones when bit i of the 16 bits is set:
   * the bits spread to the bytes and test against each byte's own. */
  constexpr std::uint64_t weights = 0x8040'2010'0804'0201ULL;
  if (!store) {
    emitter_.emit(load_scalar(1, false, false, 9, 8));
    emitter_.emit(dup_general(1, scratch_a, 9));
    emitter_.emit(movi_bytes_one(scratch_b));
    emitter_.emit(ins_low_zero(scratch_b));
    emitter_.emit(tbl_one(scratch_a, scratch_a, scratch_b));
    emit_sve_vector_constant(scratch_b, weights, weights);
    const auto out = write_native_predicate(predicate, false, 3U);
    emitter_.emit(cmtst_bytes(out.host, scratch_a, scratch_b));
    commit_native_predicate(out, predicate);
    return true;
  }
  const auto source = read_native_predicate(predicate, 0U);
  emit_sve_vector_constant(scratch_b, weights, weights);
  emitter_.emit(three(vector_and, scratch_a, source.host, scratch_b));
  emitter_.emit(ext_half(scratch_b, scratch_a, scratch_a));
  emitter_.emit(zip1_bytes(scratch_a, scratch_a, scratch_b));
  emitter_.emit(addv_halves(scratch_a, scratch_a));
  emitter_.emit(umov_h0(9, scratch_a));
  emitter_.emit(store_half(9, 8));
  clear_local_exclusive_monitor();
  return true;
}

bool JitCompiler::emit_sve_saturating_count(std::uint32_t instruction) {
  const unsigned size = field(instruction, 22, 2);
  const unsigned d = field(instruction, 0, 5);
  const bool wide = field(instruction, 20, 1) != 0U;
  const bool decrement = field(instruction, 11, 1) != 0U;
  const bool is_unsigned = field(instruction, 10, 1) != 0U;
  const unsigned count =
      predicate_pattern_count(field(instruction, 5, 5),
                              sve_vector_length >> size) *
      (field(instruction, 16, 4) + 1U);
  begin_sve_word();
  if (d == 31U || (wide && count == 0U))
    return true; // XZR, or X plus nothing
  const auto out = write_native_gpr(d, false, true, 9U);
  const unsigned x = out.host;
  /* The saturation tests the flags, which guest code never sees. */
  spill_flags_for_internal_clobber();
  if (is_unsigned) {
    /* The carry: a decrement saturates at zero when it borrowed, an
     * increment at all ones when it carried. */
    emitter_.emit(add_immediate_flags(wide, decrement, x, x, count));
    emitter_.emit(decrement ? (wide ? csel_x(x, x, 31, cond_hs)
                                    : csel_w(x, x, 31, cond_hs))
                            : csinv(wide, x, x, 31, cond_lo));
  } else if (wide) {
    /* The overflow: a signed 64-bit decrement can only reach the least
     * value, an increment the greatest. */
    emitter_.emit(add_immediate_flags(true, decrement, x, x, count));
    emitter_.load_constant(10, decrement ? 0x8000'0000'0000'0000ULL
                                         : 0x7fff'ffff'ffff'ffffULL);
    emitter_.emit(csel_x(x, 10, x, cond_vs));
  } else {
    /* Wdn is a signed 32-bit value; the result is sign-extended. */
    emitter_.emit(sxtw(x, x));
    if (decrement)
      emitter_.subtract_immediate(x, x, count);
    else
      emitter_.add_immediate(x, x, count);
    emitter_.load_constant(10, decrement ? 0xffff'ffff'8000'0000ULL
                                         : 0x7fff'ffffULL);
    emitter_.emit(cmp_x(x, 10));
    emitter_.emit(csel_x(x, 10, x, decrement ? cond_lt : cond_gt));
  }
  commit_native_gpr(out, d, false);
  return true;
}

bool JitCompiler::emit_sve_narrow(std::uint16_t form,
                                  std::uint32_t instruction) {
  const bool shift = form == inl::shrnb || form == inl::shrnt ||
                     form == inl::rshrnb || form == inl::rshrnt;
  const bool arithmetic =
      form == inl::raddhnb || form == inl::raddhnt || form == inl::subhnb ||
      form == inl::subhnt || form == inl::rsubhnb || form == inl::rsubhnt;
  if (!shift && !arithmetic)
    return false;
  const unsigned d = field(instruction, 0, 5);
  const unsigned n = field(instruction, 5, 5);
  const unsigned m = field(instruction, 16, 5);
  /* Bit 10 picks the top half; bit 11 of the shifts and bit 11 and 12 of
   * the others pick the rounding and the subtraction. */
  const bool top = (instruction & 0x400U) != 0U;
  std::uint32_t narrowing;
  unsigned log_size;
  if (shift) {
    /* The destination element: tszh:tszl (bit 22, bits 20:19). */
    const unsigned code = (field(instruction, 22, 1) << 5) |
                          (field(instruction, 19, 2) << 3) |
                          field(instruction, 16, 3);
    const unsigned tsz = code >> 3;
    if (tsz == 0U)
      return false;
    log_size = tsz == 1U ? 0U : tsz < 4U ? 1U : 2U;
    const bool rounding = (instruction & 0x800U) != 0U;
    narrowing = (rounding ? 0x0f00'8c00U : 0x0f00'8400U) | (code << 16);
  } else {
    const unsigned source = field(instruction, 22, 2);
    if (source == 0U)
      return false;
    log_size = source - 1U;
    const bool rounding = (instruction & 0x800U) != 0U;
    const bool subtract = (instruction & 0x1000U) != 0U;
    narrowing = (subtract ? 0x0e20'6000U : 0x0e20'4000U) |
                (rounding ? 0x2000'0000U : 0U) | (log_size << 22);
  }
  begin_sve_word();
  const auto first = read_native_vector(n, 0U);
  if (!shift) {
    const auto second = m == n ? first : read_native_vector(m, 1U);
    narrowing |= second.host << 16;
  }
  /* The narrowed elements fill the low half of scratch_a. */
  emitter_.emit(narrowing | (first.host << 5) | scratch_a);
  if (!top) {
    emitter_.emit(movi_bytes(true, scratch_b, 0U));
    const auto out = write_native_vector(d, false, 2U);
    emitter_.emit(three(sized(0x4e00'3800U, log_size), out.host, scratch_a,
                        scratch_b));
    commit_native_vector(out, d);
    return true;
  }
  /* The old even elements, packed, then interleaved with the narrowed. */
  const auto old = read_native_vector(d, 2U);
  emitter_.emit(three(sized(0x4e00'1800U, log_size), scratch_b, old.host,
                      old.host));
  const auto out = write_native_vector(d, false, 3U);
  emitter_.emit(three(sized(0x4e00'3800U, log_size), out.host, scratch_b,
                      scratch_a));
  commit_native_vector(out, d);
  return true;
}

bool JitCompiler::emit_sve_doubleword(std::uint16_t form,
                                      std::uint32_t instruction,
                                      std::uint64_t pc) {
  const unsigned d = field(instruction, 0, 5);
  const unsigned n = field(instruction, 5, 5);
  const unsigned m = field(instruction, 16, 5);
  const unsigned g = field(instruction, 10, 3);
  const bool maximum = form == inl::smax_p || form == inl::umax_p;
  const bool minimum = form == inl::smin_p || form == inl::umin_p;
  if (maximum || minimum) {
    /* Zdn, Pg/M, Zdn, Zm (Zm is bits 5-9): the compare's mask selects. */
    begin_sve_word();
    emit_sve_guard(g, 8U, instruction, pc);
    const auto first = read_native_vector(d, 0U);
    const auto second = n == d ? first : read_native_vector(n, 1U);
    const bool is_unsigned = form == inl::umax_p || form == inl::umin_p;
    /* The mask is set where the first operand is the result. */
    emitter_.emit(three(sized(is_unsigned ? vector_cmhi : 0x4e20'3400U, 3U),
                        scratch_a, maximum ? first.host : second.host,
                        maximum ? second.host : first.host));
    emitter_.emit(three(0x6e60'1c00U, scratch_a, first.host, second.host));
    const auto out = write_native_vector(d, false, 2U);
    emitter_.emit(vector_move(out.host, scratch_a));
    commit_native_vector(out, d);
    return true;
  }
  if (form != inl::mul_p && form != inl::mla_p && form != inl::mls_p &&
      form != inl::mad_p && form != inl::msb_p && form != inl::mul_u)
    return false;

  /* A product modulo 2^64, with no 64-bit AdvSIMD multiply: the low halves'
   * full product plus the cross products' low words shifted up. */
  begin_sve_word();
  if (form != inl::mul_u)
    emit_sve_guard(g, 8U, instruction, pc);
  unsigned multiplicand;
  unsigned multiplier;
  unsigned addend = 0;
  bool has_addend = true;
  bool subtract = form == inl::mls_p || form == inl::msb_p;
  NativeVector first;
  NativeVector second;
  NativeVector third;
  if (form == inl::mul_p) {
    /* Zdn, Pg/M, Zdn, Zm: Zm is bits 5-9. */
    first = read_native_vector(d, 0U);
    second = n == d ? first : read_native_vector(n, 1U);
    has_addend = false;
  } else if (form == inl::mul_u) {
    first = read_native_vector(n, 0U);
    second = m == n ? first : read_native_vector(m, 1U);
    has_addend = false;
  } else if (form == inl::mla_p || form == inl::mls_p) {
    /* Zda, Pg/M, Zn, Zm: Zn is bits 5-9, Zm bits 16-20. */
    first = read_native_vector(n, 0U);
    second = m == n ? first : read_native_vector(m, 1U);
    third = d == n ? first : d == m ? second : read_native_vector(d, 2U);
  } else {
    /* MAD, MSB: Zdn, Pg/M, Zm, Za with Zm bits 16-20 and Za bits 5-9. */
    first = read_native_vector(d, 0U);
    second = m == d ? first : read_native_vector(m, 1U);
    third = n == d ? first : n == m ? second : read_native_vector(n, 2U);
  }
  multiplicand = first.host;
  multiplier = second.host;
  if (has_addend)
    addend = third.host;
  constexpr unsigned extra = 3U;
  emitter_.emit(0x4ea0'0800U | (multiplier << 5) | scratch_a); // rev64 .4s
  emitter_.emit(three(0x4ea0'9c00U, scratch_a, scratch_a, multiplicand));
  emitter_.emit(0x6ea0'2800U | (scratch_a << 5) | scratch_a); // uaddlp .2d
  emitter_.emit(0x4f60'5400U | (scratch_a << 5) | scratch_a); // shl #32
  emitter_.emit(narrow(2U, scratch_b, multiplicand));
  emitter_.emit(narrow(2U, extra, multiplier));
  emitter_.emit(three(0x2ea0'8000U, scratch_a, scratch_b, extra)); // umlal
  if (has_addend)
    emitter_.emit(vector_add(subtract, 3U, scratch_a, addend, scratch_a));
  const auto out = write_native_vector(d, false, 3U);
  emitter_.emit(vector_move(out.host, scratch_a));
  commit_native_vector(out, d);
  return true;
}

} // namespace uml::cpu::arm64
