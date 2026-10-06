// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SME in the arm64 JIT (design 4.1 and 4.2): the control core (S4 milestone
 * M1) and the register-only forms (M2). Every size and layout that depends
 * on the streaming vector length is read at run time, so a translation is
 * the same for every SVL: with RDSVL and ADDSVL in SM=1 translations, from
 * CpuState::sme_svl in the others.
 *
 * SM=1 translations execute the ZA and ZT0 forms and the register-only
 * forms natively on the guest's resident state, with only their general
 * register fields re-encoded. SM=0 translations run with host SME off and
 * contain no SVE or SME instruction: they copy between guest memory and the
 * register file with GPRs (design 4.1.2).
 */
#include "jit_compiler.hpp"
#include "jit_sme_support.hpp"

#include "cpu/arm64/detail/sme_decode.hpp"

#include <algorithm>
#include <optional>
#include <utility>

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

constexpr std::uint32_t clear_fpsr = 0xd51b'443fU;    // msr fpsr, xzr
constexpr std::uint32_t read_fpsr_x11 = 0xd53b'442bU; // mrs x11, fpsr
constexpr std::uint32_t save_flags_x16 = 0xd53b'4210U;    // mrs x16, nzcv
constexpr std::uint32_t restore_flags_x16 = 0xd51b'4210U; // msr nzcv, x16
constexpr unsigned fpsr_qc_bit = 27;

/* LDR or STR Zt or Pt, [Xbase, #imm, MUL VL]. */
[[nodiscard]] constexpr std::uint32_t register_fill(bool load, bool predicate,
                                                    unsigned base,
                                                    unsigned imm,
                                                    unsigned reg) noexcept {
  return (load ? 0x8580'0000U : 0xe580'0000U) | (predicate ? 0U : 0x4000U) |
         ((imm >> 3) << 16) | ((imm & 7U) << 10) | (base << 5) | reg;
}

/* The register file holds Z_n at n * SVL and P_n at 32 * SVL + n * SVL / 8,
 * which is 16 * SVL further on plus (128 + n) predicate lengths: the
 * immediate of register_fill() for a base that ADDSVL #16 moved. */
constexpr unsigned predicate_base_svl = 16;
constexpr unsigned predicate_fill_bias = 128;
static_assert(sme_p_offset_vl == 32U);
static_assert((sme_p_offset_vl - predicate_base_svl) * 8U ==
              predicate_fill_bias);
static_assert(register_fill(false, false, 16, 5, 5) == 0xe580'5605U);
static_assert(register_fill(true, false, 11, 31, 31) == 0x8583'5d7fU);
static_assert(register_fill(false, true, 16, 131, 3) == 0xe590'0e03U);
static_assert(register_fill(true, true, 11, 143, 15) == 0x8591'1d6fU);

/* The single field of a register operand numbered by one field. */
[[nodiscard]] constexpr bool one_field(const detail::SmeOperand &operand,
                                       unsigned width) noexcept {
  return operand.segment_count == 1U && operand.segments[0].field &&
         operand.segments[0].width == width;
}

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
  if (row == nullptr || !jit::implements_sme_row(*row) ||
      !arm64::detail::sme_mode_permits(*row, (mode & svcr_sm) != 0U,
                                       (mode & svcr_za) != 0U))
    return Action::unsupported;
  const bool streaming = streaming_translation();
  const auto form = row->form;
  if (!jit::implements_sme_form(form)) {
    /* A register-only form: every one needs streaming mode. */
    if (!streaming)
      return Action::unsupported;
    if (form == forms::movprfx || form == forms::movprfx_predicated) {
      emit_movprfx(instruction, form == forms::movprfx_predicated);
      return Action::keep_going;
    }
    const auto plan = plan_sme_row(*row, instruction);
    if (!plan)
      return Action::unsupported;
    emit_streaming_word(*plan);
    return Action::keep_going;
  }
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

std::optional<JitCompiler::StreamingWord>
JitCompiler::plan_sme_row(const detail::SmeEncoding &row,
                          std::uint32_t instruction) {
  using Saved = StreamingWord::Saved;
  if (jit::sme_word_reserved(row, instruction))
    return std::nullopt;
  StreamingWord plan{.word = instruction};
  const auto *operands = detail::sme_encoding_operands_of(row);
  unsigned destinations = 0;
  std::optional<detail::SmeOperandKind> destination_kind;
  unsigned destination = 0;
  unsigned destination_count = 1;
  /* Bit p set: P_p is a source. */
  unsigned predicate_sources = 0;
  for (unsigned index = 0; index < row.operand_count; ++index) {
    const auto &operand = operands[index];
    const auto number = detail::sme_operand_number(operand, instruction);
    const bool at_zero = std::any_of(
        operand.segments.begin(),
        operand.segments.begin() + operand.segment_count,
        [](const detail::SmeSegment &segment) {
          return segment.field && segment.low == 0U;
        });
    switch (operand.kind) {
    case detail::SmeOperandKind::za_tile:
      continue;
    case detail::SmeOperandKind::gpr:
    case detail::SmeOperandKind::gpr_or_sp:
      /* A re-encoded field must stay inside the row: the row may fix no
       * bit of it. */
      if (one_field(operand, 5)) {
        if (plan.gpr_count == plan.gprs.size() ||
            ((row.mask >> operand.segments[0].low) & 31U) != 0U)
          return std::nullopt;
        destinations += at_zero ? 1U : 0U;
        plan.gprs[plan.gpr_count++] = {
            .low = operand.segments[0].low,
            .guest = static_cast<std::uint8_t>(number),
            .stack_pointer =
                operand.kind == detail::SmeOperandKind::gpr_or_sp,
            .destination = at_zero,
        };
        continue;
      }
      /* A slice index: a constant prefix and a field, naming W12-W15 in
       * the SME1 forms, which run with the index in W12. */
      if (operand.segment_count == 2U && !operand.segments[0].field &&
          operand.segments[1].field && !plan.slice) {
        const unsigned width = operand.segments[1].width;
        if ((12U >> width) != operand.segments[0].constant ||
            (12U & ((1U << width) - 1U)) != 0U ||
            ((row.mask >> operand.segments[1].low) &
             ((1U << width) - 1U)) != 0U)
          return std::nullopt;
        plan.slice = true;
        plan.slice_low = operand.segments[1].low;
        plan.slice_width = static_cast<std::uint8_t>(width);
        plan.slice_guest = static_cast<std::uint8_t>(number);
        continue;
      }
      return std::nullopt;
    case detail::SmeOperandKind::z:
    case detail::SmeOperandKind::v:
    case detail::SmeOperandKind::p:
      if (at_zero) {
        ++destinations;
        destination_kind = operand.kind;
        destination = number;
        destination_count = operand.count;
      } else if (operand.kind == detail::SmeOperandKind::p) {
        for (unsigned reg = 0; reg < operand.count; ++reg)
          predicate_sources |= 1U << ((number + reg * operand.stride) & 15U);
      }
      continue;
    }
  }
  if (destinations > 1U)
    return std::nullopt;
  const bool predicate_destination =
      destination_kind == detail::SmeOperandKind::p;
  plan.reads_flags = plan.writes_flags =
      jit::sme_row_uses_flags(row, predicate_destination);
  plan.raises = (row.form & detail::sme_form_sme_section) == 0U &&
                jit::sve_raises_fp_exceptions(instruction);
  if (plan.raises && destination_kind) {
    /* A rerun must see the sources the first run saw: a Z destination is
     * read by every merging and destructive form, a P destination only
     * when it is also a source. */
    if (destination_count != 1U)
      return std::nullopt;
    if (!predicate_destination)
      plan.saved = Saved::z;
    else if ((predicate_sources & (1U << destination)) != 0U)
      plan.saved = Saved::p;
    plan.saved_register = static_cast<std::uint8_t>(destination);
  }
  return plan;
}

void JitCompiler::emit_streaming_word(const StreamingWord &plan) {
  using Saved = StreamingWord::Saved;
  auto word = plan.word;
  const auto place = [&word](unsigned low, unsigned width, unsigned value) {
    const auto mask = ((1U << width) - 1U) << low;
    word = (word & ~mask) | ((value << low) & mask);
  };
  /* Sources first, in x8 and x9 when not resident; the destination, which
   * may also be read, in x10. */
  unsigned fallback = 8;
  for (unsigned index = 0; index < plan.gpr_count; ++index) {
    const auto &gpr = plan.gprs[index];
    if (gpr.destination)
      continue;
    const auto native = read_native_gpr(gpr.guest, gpr.stack_pointer,
                                        fallback++);
    place(gpr.low, 5, native.host);
  }
  std::optional<std::pair<NativeGpr, StreamingWord::Gpr>> destination;
  for (unsigned index = 0; index < plan.gpr_count; ++index) {
    const auto &gpr = plan.gprs[index];
    if (!gpr.destination)
      continue;
    const auto native =
        write_native_gpr(gpr.guest, gpr.stack_pointer, true, 10);
    place(gpr.low, 5, native.host);
    destination.emplace(native, gpr);
  }
  if (plan.slice) {
    /* x12 holds no guest register in a streaming translation. */
    load_gpr(12, plan.slice_guest, false);
    place(plan.slice_low, plan.slice_width, 0);
  }
  if (plan.reads_flags)
    load_flags();
  if (!plan.raises) {
    emitter_.emit(word);
  } else {
    /*
     * XNU resets FPSR to 0x0800009F on every kernel entry while a thread
     * is in streaming mode, so the guest's FPSR stays in CpuState: run the
     * word with FPSR clear and OR what it raised into the guest's. No
     * streaming-mode instruction sets QC, so a QC bit means a kernel entry
     * came between, and the word runs again on its saved inputs.
     */
    const bool predicate = plan.saved == Saved::p;
    const unsigned offset =
        plan.saved_register + (predicate ? predicate_fill_bias : 0U);
    if (plan.saved != Saved::none) {
      emitter_.load_x(16, 15, offsetof(CpuState, sme_registers));
      if (predicate)
        emitter_.emit(addsvl(16, 16, predicate_base_svl));
      emitter_.emit(
          register_fill(false, predicate, 16, offset, plan.saved_register));
    }
    if (plan.reads_flags)
      emitter_.emit(save_flags_x16);
    const auto retry = emitter_.make_label();
    const auto done = emitter_.make_label();
    emitter_.bind(retry);
    emitter_.emit(clear_fpsr);
    emitter_.emit(word);
    emitter_.emit(read_fpsr_x11);
    emitter_.test_zero(done, 11, fpsr_qc_bit, false);
    if (plan.saved != Saved::none) {
      emitter_.load_x(11, 15, offsetof(CpuState, sme_registers));
      if (predicate)
        emitter_.emit(addsvl(11, 11, predicate_base_svl));
      emitter_.emit(
          register_fill(true, predicate, 11, offset, plan.saved_register));
    }
    if (plan.reads_flags)
      emitter_.emit(restore_flags_x16);
    emitter_.branch(retry);
    emitter_.bind(done);
    emitter_.load_w(16, 15, offsetof(CpuState, fpsr));
    emitter_.emit(0x2a0b'0210U); // orr w16, w16, w11
    emitter_.store_w(16, 15, offsetof(CpuState, fpsr));
  }
  if (plan.writes_flags)
    store_flags();
  if (destination)
    commit_native_gpr(destination->first, destination->second.guest,
                      destination->second.stack_pointer);
}

void JitCompiler::emit_movprfx(std::uint32_t instruction, bool predicated) {
  /*
   * MOVPRFX runs as the move it is architecturally, never paired with the
   * guest's next word: nothing the translation puts around that word can
   * then come between a native pair (design 1.4).
   */
  const unsigned destination = instruction & 31U;
  const unsigned source = (instruction >> 5) & 31U;
  if (!predicated) {
    emitter_.emit(0x0460'3000U | (source << 16) | (source << 5) |
                  destination); // orr zd.d, zn.d, zn.d
    return;
  }
  const unsigned size = (instruction >> 22) & 3U;
  const unsigned governing = (instruction >> 10) & 7U;
  if ((instruction & 0x0001'0000U) != 0U) {
    emitter_.emit(0x0520'c000U | (size << 22) | (destination << 16) |
                  (governing << 10) | (source << 5) |
                  destination); // sel zd.t, pg, zn.t, zd.t
    return;
  }
  /* Zeroing: the MOVPRFX itself, completed by LSL #0, a prefixed form
   * with the same predicate and element size. */
  const unsigned shift_size = 1U << size;
  emitter_.emit(instruction);
  emitter_.emit(0x0403'8000U | ((shift_size >> 2) << 22) | (governing << 10) |
                ((shift_size & 3U) << 8) | destination);
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
