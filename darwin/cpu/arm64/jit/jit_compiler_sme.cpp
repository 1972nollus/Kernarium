// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The SME control core of the arm64 JIT (design 4.1 and 4.2, S4 milestone
 * M1). Every size and layout that depends on the streaming vector length is
 * read at run time, so a translation is the same for every SVL: with RDSVL
 * and ADDSVL in SM=1 translations, from CpuState::sme_svl in the others.
 *
 * SM=1 translations execute the ZA and ZT0 forms natively on the guest's
 * resident state. SM=0 translations run with host SME off and contain no
 * SVE or SME instruction: they copy between guest memory and the register
 * file with GPRs (design 4.1.2).
 */
#include "jit_compiler.hpp"
#include "jit_sme_support.hpp"

#include "cpu/arm64/detail/sme_decode.hpp"

namespace uml::cpu::arm64 {

namespace {

namespace forms = jit::sme_forms;

[[nodiscard]] constexpr std::uint32_t rdsvl(unsigned destination,
                                            unsigned multiple) noexcept {
  return 0x04bf'5800U | ((multiple & 63U) << 5) | destination;
}

[[nodiscard]] constexpr std::uint32_t addsvl(unsigned destination,
                                             unsigned source,
                                             unsigned multiple) noexcept {
  return 0x0420'5800U | (source << 16) | ((multiple & 63U) << 5) |
         destination;
}

/* LDR or STR ZA[W12, 0], [Xbase]. */
[[nodiscard]] constexpr std::uint32_t za_vector_transfer(bool load,
                                                         unsigned base) noexcept {
  return (load ? 0xe100'0000U : 0xe120'0000U) | (base << 5);
}

/* LDR or STR ZT0, [Xbase]. */
[[nodiscard]] constexpr std::uint32_t zt0_transfer(bool load,
                                                   unsigned base) noexcept {
  return (load ? 0xe11f'8000U : 0xe13f'8000U) | (base << 5);
}

/* LDP or STP of X0 and X1 at [Xbase], #16 (post-index). */
[[nodiscard]] constexpr std::uint32_t pair_post_index(bool load,
                                                      unsigned base) noexcept {
  return (load ? 0xa8c1'0400U : 0xa881'0400U) | (base << 5);
}

/* MADD Xd, Xn, Xm, Xa. */
[[nodiscard]] constexpr std::uint32_t multiply_add(unsigned destination,
                                                   unsigned left,
                                                   unsigned right,
                                                   unsigned addend) noexcept {
  return 0x9b00'0000U | (right << 16) | (addend << 10) | (left << 5) |
         destination;
}

/* Xd = SVL, from RDSVL in a streaming translation and from CpuState in the
 * others, where the host's SME is off. */
void load_vector_length(Arm64Emitter &emitter, bool streaming,
                        unsigned destination) {
  if (streaming)
    emitter.emit(rdsvl(destination, 1));
  else
    emitter.load_w(destination, 15, offsetof(CpuState, sme_svl));
}

static_assert(offsetof(CpuState, sme_svl) % 4U == 0U);
static_assert(detail::find_sme_encoding(rdsvl(1, 1))->form == forms::rdsvl);
static_assert(detail::find_sme_encoding(addsvl(8, 8, 15))->form ==
              forms::addsvl);
static_assert(detail::find_sme_encoding(za_vector_transfer(true, 8))->form ==
              forms::ldr_za);
static_assert(detail::find_sme_encoding(za_vector_transfer(false, 8))->form ==
              forms::str_za);
static_assert(detail::find_sme_encoding(zt0_transfer(true, 8))->form ==
              forms::ldr_zt0);
static_assert(detail::find_sme_encoding(zt0_transfer(false, 8))->form ==
              forms::str_zt0);

} // namespace

std::optional<JitCompiler::Action>
JitCompiler::emit_sme_system(std::uint32_t instruction, std::uint64_t pc) {
  /* MSR SVCRSM, SVCRZA or SVCRSMZA, #imm is MSR (immediate) with op1 3 and
   * op2 3; the others are MRS and MSR (register). */
  const bool svcr_immediate = (instruction & 0xffff'f0ffU) == 0xd503'407fU;
  const auto system = instruction & 0xffff'ffe0U;
  if (!svcr_immediate && system != 0xd53b'4240U && system != 0xd51b'4240U &&
      system != 0xd53b'd0a0U && system != 0xd51b'd0a0U)
    return std::nullopt;
  if (!implements_sme(host_capabilities()))
    return std::nullopt;
  const unsigned transfer = instruction & 31U;
  const auto write_destination = [&](auto emit_value) {
    if (transfer == 31U)
      return;
    const auto destination = write_native_gpr(transfer, false, false, 8);
    emit_value(destination.host);
    commit_native_gpr(destination, transfer, false);
  };
  if (system == 0xd53b'4240U) {
    /* MRS SVCR: the translation's mode is the guest's. */
    write_destination([&](unsigned destination) {
      emitter_.load_constant(destination, sme_mode());
    });
    return Action::keep_going;
  }
  /* TPIDR2_EL0 is the guest's own, never the host register, which belongs
   * to the host's SME ABI. */
  if (system == 0xd53b'd0a0U) {
    write_destination([&](unsigned destination) {
      emitter_.load_x(destination, 15, offsetof(CpuState, tpidr2));
    });
    return Action::keep_going;
  }
  if (system == 0xd51b'd0a0U) {
    const auto source = read_native_gpr(transfer, false, 8);
    emitter_.store_x(source.host, 15, offsetof(CpuState, tpidr2));
    return Action::keep_going;
  }
  if (svcr_immediate) {
    /* CRm 001x, 010x or 011x selects SM, ZA or both; the rest are
     * unallocated. */
    const unsigned field = (instruction >> 8) & 15U;
    if (field < 2U || field > 7U)
      return Action::unsupported;
  }
  /* A write of SVCR ends the block: its successor runs in the new mode. */
  emit_terminal_helper(
      reinterpret_cast<const void *>(&uml_arm64_jit_write_svcr), instruction,
      pc, count_ + 1U);
  return Action::terminate;
}

JitCompiler::Action JitCompiler::emit_sme_instruction(std::uint32_t instruction,
                                                      std::uint64_t pc) {
  const auto &host = host_capabilities();
  const auto *row = arm64::detail::decode_sme(
      instruction, implemented_sme_features(host), host.sme_svl_bytes);
  const auto mode = sme_mode();
  if (row == nullptr || !jit::implements_sme_form(row->form) ||
      !arm64::detail::sme_mode_permits(*row, (mode & svcr_sm) != 0U,
                                       (mode & svcr_za) != 0U))
    return Action::unsupported;
  const bool streaming = streaming_translation();
  const auto form = row->form;
  if (form == forms::rdsvl || form == forms::addsvl ||
      form == forms::addspl) {
    emit_sme_vector_length(instruction, form == forms::rdsvl);
    return Action::keep_going;
  }
  if (form == forms::ldr_za || form == forms::str_za) {
    emit_sme_array_vector(instruction, pc, form == forms::ldr_za);
    return Action::keep_going;
  }
  if (form == forms::ldr_zt0 || form == forms::str_zt0) {
    emit_sme_zt0_transfer(instruction, pc, form == forms::ldr_zt0);
    return Action::keep_going;
  }
  if (streaming) {
    /* ZERO ZA and ZERO ZT0 name no register; ZA is resident. */
    emitter_.emit(instruction);
    return Action::keep_going;
  }
  if (form == forms::zero_zt0) {
    emit_sme_zt0_address();
    for (unsigned offset = 0; offset < 64U; offset += 16U)
      emitter_.emit(0xa900'0000U | ((offset / 8U) << 15) | (31U << 10) |
                    (9U << 5) | 31U); // stp xzr, xzr, [x9, #offset]
    return Action::keep_going;
  }
  /* ZERO ZA in an SM=0 translation: rare (ZA was just enabled), so the
   * register file's tiles are cleared by the helper. */
  emit_terminal_helper(
      reinterpret_cast<const void *>(&uml_arm64_jit_execute_sme), instruction,
      pc, count_ + 1U);
  return Action::terminate;
}

void JitCompiler::emit_sme_vector_length(std::uint32_t instruction,
                                         bool read) {
  const unsigned destination = instruction & 31U;
  const bool streaming = streaming_translation();
  if (read && destination == 31U)
    return; // RDSVL Xd, #imm: Rd 31 is XZR.
  if (!streaming) {
    /* x10 = SVL, or SVL / 8 for ADDSPL; x11 = the signed multiple. */
    load_vector_length(emitter_, false, 10);
    if ((instruction & 0x0040'0000U) != 0U)
      emitter_.ubfm(10, 10, 3, 63); // lsr x10, x10, #3
    emitter_.load_constant(
        11, static_cast<std::uint64_t>(sign_extend((instruction >> 5) & 63U, 6)));
  }
  if (read) {
    const auto native = write_native_gpr(destination, false, false, 9);
    if (streaming)
      emitter_.emit((instruction & ~31U) | native.host);
    else
      emitter_.emit(multiply_add(native.host, 10, 11, 31));
    commit_native_gpr(native, destination, false);
    return;
  }
  /* ADDSVL and ADDSPL Xd|SP, Xn|SP, #imm. */
  const unsigned source = (instruction >> 16) & 31U;
  const auto native_source = read_native_gpr(source, true, 8);
  const auto native_destination =
      write_native_gpr(destination, true, source == destination, 9);
  if (streaming)
    emitter_.emit((instruction & ~((31U << 16) | 31U)) |
                  (native_source.host << 16) | native_destination.host);
  else
    emitter_.emit(
        multiply_add(native_destination.host, 10, 11, native_source.host));
  commit_native_gpr(native_destination, destination, true);
}

void JitCompiler::emit_sme_array_vector(std::uint32_t instruction,
                                        std::uint64_t pc, bool load) {
  /* LDR and STR ZA[Wv, #off4], [Xn|SP, #off4, MUL VL]: ZA vector
   * (Wv + off4) MOD SVL, at Xn + off4 * SVL, with Wv one of W12-W15. */
  const unsigned base = (instruction >> 5) & 31U;
  const unsigned offset = instruction & 15U;
  const unsigned index = 12U + ((instruction >> 13) & 3U);
  if (!load)
    mark_direct_memory_write();
  const auto slow = cold_exit(instruction, pc, ColdPath::Action::sme_slow);
  const auto fault =
      cold_exit(instruction, pc, ColdPath::Action::memory_fault);
  load_gpr(8, base, true);
  /* A misaligned SP and a tagged address take the helper, which reports
   * the alignment fault or untags the address. */
  if (base == 31U) {
    emit_misalignment_check(8, 16, slow);
  } else {
    emitter_.load_b(0, 15, offsetof(CpuState, tbi_enabled));
    emitter_.compare_zero(slow, 0, false, true);
  }
  const bool streaming = streaming_translation();
  load_vector_length(emitter_, streaming, 1);
  if (offset != 0U) {
    if (streaming) {
      emitter_.emit(addsvl(8, 8, offset));
    } else {
      emitter_.load_constant(2, offset);
      emitter_.emit(multiply_add(8, 1, 2, 8));
    }
  }
  /* The vector's SVL bytes must lie in one page: SVL is below a page, so
   * the page bit of (address & page_mask) + SVL - 1 is the test. A
   * crossing vector takes the helper, which translates both pages first. */
  emitter_.ubfm(0, 8, 0, page_shift - 1U);
  emitter_.add_shifted(0, 0, 1, 0);
  emitter_.subtract_immediate(0, 0, 1);
  emitter_.test_zero(slow, 0, page_shift, true);
  emit_translate(8, load ? Access::read : Access::write, slow, fault, 1);
  if (!load) {
    load_vector_length(emitter_, streaming, 10);
    track_direct_store_register(8, 10, count_);
  }
  if (streaming) {
    /* x12 holds no guest register in a streaming translation. */
    load_gpr(12, index, false);
    if (offset != 0U)
      emitter_.emit(0x1100'0000U | (offset << 10) | (12U << 5) |
                    12U); // add w12, w12, #offset
    emitter_.emit(za_vector_transfer(load, 8));
  } else {
    /* x9 = the register file's ZA vector (34 + vector) * SVL. */
    emitter_.load_x(9, 15, offsetof(CpuState, sme_registers));
    load_gpr(11, index, false);
    if (offset != 0U)
      emitter_.add_immediate(11, 11, offset);
    load_vector_length(emitter_, false, 10);
    emitter_.subtract_immediate(16, 10, 1);
    emitter_.emit(0x8a10'016bU); // and x11, x11, x16
    emitter_.add_immediate(11, 11, static_cast<unsigned>(sme_za_offset_vl));
    emitter_.emit(multiply_add(9, 11, 10, 9));
    /* SVL bytes, 16 at a time, from x8 to x9 or back. */
    const auto loop = emitter_.make_label();
    emitter_.bind(loop);
    emitter_.emit(pair_post_index(true, load ? 8U : 9U));
    emitter_.emit(pair_post_index(false, load ? 9U : 8U));
    emitter_.subtract_immediate(10, 10, 16);
    emitter_.compare_zero(loop, 10, true, true);
  }
  if (!load)
    clear_local_exclusive_monitor();
}

void JitCompiler::emit_sme_zt0_address() {
  /* ZT0 follows the SVL ZA vectors: 34 * SVL + SVL * SVL. */
  emitter_.load_x(9, 15, offsetof(CpuState, sme_registers));
  load_vector_length(emitter_, false, 10);
  emitter_.add_immediate(11, 10, static_cast<unsigned>(sme_za_offset_vl));
  emitter_.emit(multiply_add(9, 11, 10, 9));
}

void JitCompiler::emit_sme_zt0_transfer(std::uint32_t instruction,
                                        std::uint64_t pc, bool load) {
  /* LDR and STR ZT0, [Xn|SP]: 64 bytes. */
  const unsigned base = (instruction >> 5) & 31U;
  if (!load)
    mark_direct_memory_write();
  const auto slow = cold_exit(instruction, pc, ColdPath::Action::sme_slow);
  const auto fault =
      cold_exit(instruction, pc, ColdPath::Action::memory_fault);
  load_gpr(8, base, true);
  if (base == 31U) {
    emit_misalignment_check(8, 16, slow);
  } else {
    emitter_.load_b(0, 15, offsetof(CpuState, tbi_enabled));
    emitter_.compare_zero(slow, 0, false, true);
  }
  emit_translate(8, load ? Access::read : Access::write, slow, fault,
                 sme_zt0_bytes);
  if (!load)
    track_direct_store(8, sme_zt0_bytes);
  if (streaming_translation()) {
    emitter_.emit(zt0_transfer(load, 8));
  } else {
    emit_sme_zt0_address();
    for (unsigned pair = 0; pair < sme_zt0_bytes / 16U; ++pair) {
      emitter_.emit(pair_post_index(true, load ? 8U : 9U));
      emitter_.emit(pair_post_index(false, load ? 9U : 8U));
    }
  }
  if (!load)
    clear_local_exclusive_monitor();
}

void JitCompiler::emit_terminal_helper(const void *helper,
                                       std::uint32_t instruction,
                                       std::uint64_t pc, std::size_t count) {
  store_pc(pc);
  spill_gprs_for_terminal_call();
  emitter_.move_x(0, 15);
  emitter_.move_x(1, 14);
  emitter_.load_constant(2, instruction);
  emitter_.call(helper);
  emitter_.return_dynamic(count);
}

} // namespace uml::cpu::arm64
