// SPDX-License-Identifier: GPL-2.0-or-later
//
// VFP and Advanced SIMD: the register views and the floating-point
// environment, and the dispatch to the families.
//
// Q0-Q15 are CpuState vectors 0-15, which the A64 compiler's vector
// residency binds to host registers V16-V31: a Q operand is its resident
// host register. D n is the low (even n) or high (odd n) 64 bits of Q n/2 and
// S n is the 32-bit lane n%4 of Q n/4, so an operand narrower than a Q is
// read by moving its lane into a scratch register and written by inserting
// the result into the lane: a scalar or 64-bit A64 operation zeroes the rest
// of its destination, which here is another register or lane.
//
// Floating point. A VFP instruction runs under the guest's FPCR, which the
// executor installs in the host before the first block (the native floating
// point environment), and an Advanced SIMD floating-point instruction under
// the Standard FPSCR value: FZ and DN set, round to nearest, AHP and FZ16 the
// guest's. The host FPCR keeps the mode it has across exits and links, as the
// gadget engine's does: CpuState::a32_fp_standard records it, a translation
// switches only where the instruction needs the other mode, and tests the
// byte at the first such instruction (set_fp_mode). fp_mode_ is what the
// translation knows of the host at the point being translated.

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_operations.hpp"
#include "a32_vector_encoders.hpp"

namespace uml::cpu::arm64::jit::a32 {

namespace {

constexpr std::uint32_t fpcr_standard_bits = 0x0300'0000U; // DN and FZ
constexpr std::uint32_t fpcr_guest_kept_bits = 0x0408'0000U; // AHP and FZ16

} // namespace

// ---------------------------------------------------------------------------
// The register views.

unsigned A32Compiler::q_use(unsigned q, unsigned fallback) {
  return read_native_vector(q, fallback).host;
}

JitCompilerFamilies::NativeVector A32Compiler::q_def(unsigned q,
                                                     bool reads_old,
                                                     unsigned fallback) {
  return write_native_vector(q, reads_old, fallback);
}

void A32Compiler::q_done(NativeVector value, unsigned q) {
  commit_native_vector(value, q);
}

unsigned A32Compiler::d_use(unsigned d, unsigned scratch) {
  const unsigned host = read_native_vector(d >> 1, scratch).host;
  if ((d & 1U) == 0U)
    return host;
  // DUP Dscratch, Vhost.D[1] (the scalar form clears the upper half).
  emit(enc::vec::dup_scalar(3, scratch, host, 1));
  return scratch;
}

unsigned A32Compiler::d_use_twice(unsigned d, unsigned scratch) {
  const unsigned host = read_native_vector(d >> 1, scratch).host;
  emit(enc::vec::dup_element(true, 3, scratch, host, d & 1U));
  return scratch;
}

void A32Compiler::d_write(unsigned d, unsigned result) {
  const auto destination = q_def(d >> 1, true, 2);
  emit(enc::vec::ins_element(3, destination.host, d & 1U, result, 0));
  q_done(destination, d >> 1);
}

unsigned A32Compiler::s_use(unsigned s, unsigned scratch) {
  const unsigned host = read_native_vector(s >> 2, scratch).host;
  if ((s & 3U) == 0U)
    return host;
  emit(enc::vec::dup_scalar(2, scratch, host, s & 3U));
  return scratch;
}

void A32Compiler::s_write(unsigned s, unsigned result) {
  const auto destination = q_def(s >> 2, true, 2);
  emit(enc::vec::ins_element(2, destination.host, s & 3U, result, 0));
  q_done(destination, s >> 2);
}

// ---------------------------------------------------------------------------
// The floating-point environment.

void A32Compiler::touch_fp_environment() {
  touch_vectors();
  mark_native_fp();
}

/* The Standard FPSCR value is the guest's AHP and FZ16 with FZ and DN set.
 * CpuState::a32_fp_standard says which one the host FPCR holds while the
 * native environment is active, as it does for the gadget engine, and the
 * executor clears it where it activates the environment. Where the
 * translation does not know the mode it tests the byte and writes the FPCR
 * only if the mode differs; an exit leaves the FPCR as it is. */
void A32Compiler::set_fp_mode(bool standard) {
  const auto wanted = standard ? FpMode::standard : FpMode::guest;
  if (fp_mode_ == wanted)
    return;
  Arm64Emitter::Label done{};
  const bool tested = fp_mode_ == FpMode::unknown;
  if (tested) {
    done = make_label();
    emitter_.load_b(0, 15, offsetof(CpuState, a32_fp_standard));
    emitter_.compare_zero(done, 0, false, standard);
  }
  emitter_.load_w(0, 15, offsetof(CpuState, fpcr));
  if (standard) {
    emit(enc::movz(1, fpcr_guest_kept_bits >> 16, 1));
    emit(enc::and_(0, 0, 1));
    emit(enc::orr_i(0, 0, *enc::logical_immediate_field(fpcr_standard_bits)));
  }
  emit(0xd51b'4400U); // msr fpcr, x0
  if (standard) {
    emitter_.load_constant(1, 1);
    emitter_.store_b(1, 15, offsetof(CpuState, a32_fp_standard));
  } else {
    emitter_.store_b(31, 15, offsetof(CpuState, a32_fp_standard));
  }
  if (tested)
    bind(done);
  fp_mode_ = wanted;
}

/* The word of a Neon data-processing or structure instruction in the A32
 * layout (a Thumb word has its U bit apart from the rest). */
std::uint32_t A32Compiler::neon_word() const noexcept {
  const auto word = cur().inst.word;
  if (!thumb_)
    return word;
  if ((word & 0xff00'0000U) == 0xf900'0000U)
    return 0xf400'0000U | (word & 0x00ff'ffffU);
  return 0xf200'0000U | (((word >> 28) & 1U) << 24) | (word & 0x00ff'ffffU);
}

// ---------------------------------------------------------------------------
// The dispatch.

A32Compiler::Flow A32Compiler::emit_vector(const OpInfo &info) {
  touch_vectors();
  const auto &v = vector_info(cur().inst.form);
  if (v.mnemonic == VM::none) {
    exit_illegal();
    return Flow::end;
  }
  if (vector_memory(v.mnemonic))
    return emit_vector_memory(v);
  if (v.vfp)
    return emit_vfp(info, v);
  return emit_neon(v);
}

} // namespace uml::cpu::arm64::jit::a32
