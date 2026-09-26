// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_compiler.hpp"

#include "cpu/arm64/detail/atomic_floating.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace uml::cpu::arm64 {

void JitCompiler::emit_translate(unsigned address, Access access,
                      Arm64Emitter::Label fallback, Arm64Emitter::Label fault,
                      std::size_t bytes) {
    if (bytes > 1) {
      emitter_.ubfm(0, address, 0, page_shift - 1);
      emitter_.add_immediate(0, 0, static_cast<unsigned>(bytes - 1));
      /*
       * bytes is translation-time bounded below one page, so the sum is less
       * than two pages. Its page bit is therefore the complete crossing test.
       */
      emitter_.test_zero(fallback, 0, page_shift, true);
    }

    /*
     * The TLB tag is the guest page number, as the shared
     * `uml::memory::TlbEntry` contract and the gadget engine both store it.
     * One shift produces it, and the backing delta the hit path applies is
     * still relative to the page-aligned address.
     */
    emitter_.ubfm(1, address, page_shift, 63U); // lsr x1, Xn, #page_shift
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
    emitter_.ubfm(0, address, page_shift,
                  page_shift + ::uml::memory::tlb_index_bits - 1U);
    emitter_.add_shifted(0, 14, 0, 6);
#else
    emitter_.ubfm(0, address, page_shift,
                  page_shift + ::uml::memory::tlb_index_bits - 1U);
    emitter_.emit(0xca40'0000U | ((page_shift + 13U) << 10) |
                  ((address & 31U) << 16)); // eor x0, x0, address, lsr #25
    emitter_.ubfm(0, 0, 0, ::uml::memory::tlb_index_bits - 1U);
    emitter_.add_shifted(0, 14, 0, 5);
#endif
    const unsigned page_offset = access == Access::write ? 16U : 0U;
    const auto slow = emitter_.make_label();
    const auto complete = emitter_.make_label();
    /*
     * The access-specific tag and backing delta are adjacent immutable fields.
     * Load both before matching the tag. The overwhelmingly common first-way
     * hit falls through one normally-not-taken CBNZ and directly applies the
     * host-owned delta. The second way and preserving miss bridge are emitted
     * after the complete hot block and branch back to the same continuation.
     */
    emitter_.load_pair_x(2, 9, 0, page_offset);
    emitter_.emit(0xca01'0042U); // eor x2, x2, x1
    emitter_.compare_zero(slow, 2, true, true);
    emitter_.add_shifted(address, address, 9, 0);
    emitter_.bind(complete);
    translation_cold_.push_back({
        .label = slow,
        .continuation = complete,
        .fault = fault,
        .address = address,
        .access = access,
    });
  }

void JitCompiler::emit_translation_cold(const TranslationColdPath &path) {
    emitter_.bind(path.label);
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
    const unsigned page_offset =
        path.access == Access::write ? 16U : 0U;
    const auto miss = emitter_.make_label();
    emitter_.load_pair_x(2, 9, 0, page_offset + 32U);
    emitter_.emit(0xca01'0042U); // eor x2, x2, x1
    emitter_.compare_zero(miss, 2, true, true);
    emitter_.add_shifted(path.address, path.address, 9, 0);
    emitter_.branch(path.continuation);
    emitter_.bind(miss);
#endif
    emitter_.move_x(0, 14);
    emitter_.move_x(1, path.address);
    emitter_.load_constant(
        2, path.access == Access::write ? 1U : 0U);
    /*
     * Select the complete preservation contract while translating. Hosts
     * without FPMR never execute its system-register encodings, and generated
     * code pays no runtime feature branch.
     */
    const auto *miss_bridge =
        host_capabilities().has(ArmFeature::fp8)
            ? reinterpret_cast<const void *>(
                  &uml_arm64_jit_tlb_translate_preserving_fpmr)
            : reinterpret_cast<const void *>(
                  &uml_arm64_jit_tlb_translate_preserving);
    emitter_.call_preserving_flags(miss_bridge);
    emitter_.move_x(path.address, 0);
    emitter_.compare_zero(path.fault, path.address, true, false);
    emitter_.branch(path.continuation);
  }

void JitCompiler::emit_misalignment_check(unsigned address, std::size_t alignment,
                               Arm64Emitter::Label misaligned) {
    if (alignment <= 1U)
      return;
    if (!std::has_single_bit(alignment) || alignment > 64U) {
      /* A malformed translation request must take the existing cold semantic
       * path for every execution; do not manufacture an invalid mask or abort
       * the emulator. All production callers use constants in [2, 64]. */
      emitter_.branch(misaligned);
      return;
    }
    emitter_.ubfm(
        0, address, 0,
        static_cast<unsigned>(std::countr_zero(alignment) - 1U));
    emitter_.compare_zero(misaligned, 0, true, true);
  }

void JitCompiler::clear_local_exclusive_monitor() {
    /*
     * An ordinary store is permitted to clear the local monitor
     * unconditionally.  Keep this as one straight-line instruction after the
     * store: no opcode or monitor-state checks belong in the JIT hot path.
     */
    emitter_.store_b(31, 15, offsetof(CpuState, exclusive_size));
  }

[[nodiscard]] std::size_t
  JitCompiler::try_emit_native_exclusive_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    if (!emitter_.persistent_linked_block_abi() ||
        count_ != 0U || !gpr_slots_unbound() || instructions.size() < 2U)
      return 0;

    const auto load_instruction = instructions[0];
    const auto scalar_exclusive = [](std::uint32_t instruction) {
      return (instruction & 0x3fa0'0000U) == 0x0800'0000U;
    };
    if (!scalar_exclusive(load_instruction) ||
        (load_instruction & 0x0040'0000U) == 0U)
      return 0;

    std::size_t store_index = 1U;
    bool compare_retry = false;
    if (!scalar_exclusive(instructions[store_index]) ||
        (instructions[store_index] & 0x0040'0000U) != 0U) {
      if (instructions.size() < 3U)
        return 0;
      const auto middle = instructions[1];
      const bool add_sub_immediate =
          (middle & 0x1f80'0000U) == 0x1100'0000U;
      const bool add_sub_shifted =
          (middle & 0x1f20'0000U) == 0x0b00'0000U &&
          valid_shifted_register(middle, true);
      const bool logical_shifted =
          (middle & 0x1f00'0000U) == 0x0a00'0000U &&
          valid_shifted_register(middle, false);
      if (!add_sub_immediate && !add_sub_shifted && !logical_shifted)
        return 0;
      store_index = 2U;
      if (instructions.size() >= 5U &&
          (middle & 0x7fe0'fc1fU) == 0x6b00'001fU &&
          (instructions[2] & 0xff00'001fU) == 0x5400'0001U) {
        compare_retry = true;
        store_index = 3U;
      }
    }

    const auto store_instruction = instructions[store_index];
    if (!scalar_exclusive(store_instruction) ||
        (store_instruction & 0x0040'0000U) != 0U)
      return 0;

    const unsigned size = load_instruction >> 30;
    const unsigned base = (load_instruction >> 5) & 31U;
    const unsigned load_destination = load_instruction & 31U;
    const unsigned store_source = store_instruction & 31U;
    const unsigned status = (store_instruction >> 16) & 31U;
    if ((store_instruction >> 30) != size ||
        ((store_instruction >> 5) & 31U) != base ||
        ((load_instruction >> 10) & 31U) != 31U ||
        ((load_instruction >> 16) & 31U) != 31U ||
        ((store_instruction >> 10) & 31U) != 31U ||
        load_destination == base)
      return 0;

    if (compare_retry) {
      const auto compare = instructions[1];
      const auto mismatch_offset =
          sign_extend((instructions[2] >> 5) & 0x7ffffU, 19) * 4;
      const auto retry = instructions[4];
      const auto retry_offset = sign_extend((retry >> 5) & 0x7ffffU, 19) * 4;
      if (load_destination == 31U || status == load_destination ||
          ((compare >> 5) & 31U) != load_destination ||
          (compare >> 31) != (size == 3U ? 1U : 0U) ||
          mismatch_offset < 12 ||
          (retry & 0xff00'001fU) != (0x3500'0000U | status) ||
          retry_offset != -16)
        return 0;
    }

    const bool has_middle = store_index == 2U || compare_retry;
    const auto middle_instruction =
        has_middle ? instructions[1] : std::uint32_t{};
    if (has_middle && (middle_instruction & 31U) == base)
      return 0;

    /* This fusion needs at most six distinct resident architectural values.
     * Prove capacity before emitting any code so guest-selected register
     * aliases can only decline the optimization, never exhaust the allocator
     * after the native exclusive interval has been constructed. */
    std::array<bool, 32> required_gprs{};
    std::size_t required_gpr_count = 0;
    const auto require_gpr = [&](unsigned guest, bool stack_pointer) {
      if (guest == 31U && !stack_pointer)
        return;
      if (!required_gprs[guest]) {
        required_gprs[guest] = true;
        ++required_gpr_count;
      }
    };
    if (has_middle) {
      const auto left = (middle_instruction >> 5) & 31U;
      const auto right = (middle_instruction >> 16) & 31U;
      const bool immediate =
          (middle_instruction & 0x1f80'0000U) == 0x1100'0000U;
      const bool writes_flags =
          (middle_instruction & 0x2000'0000U) != 0U;
      if (left != load_destination)
        require_gpr(left, immediate);
      if (!immediate && right != load_destination)
        require_gpr(right, false);
      require_gpr(middle_instruction & 31U,
                  immediate && !writes_flags);
    }
    if (store_source != load_destination &&
        (!has_middle || store_source != (middle_instruction & 31U)))
      require_gpr(store_source, false);
    require_gpr(status, false);
    require_gpr(load_destination, false);
    if (required_gpr_count > gpr_slot_capacity())
      return 0;

    const std::size_t bytes = std::size_t{1} << size;
    const auto slow = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {slow, load_instruction, pc, count_ + 1,
         ColdPath::Action::deferred_exclusive_slow});
    cold_.push_back(
        {fault, load_instruction, pc, count_ + 1,
         ColdPath::Action::memory_fault});

    load_gpr(8, base, true);
    if (base == 31U) {
      emit_misalignment_check(8, 16, slow);
    } else {
      emitter_.load_h(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
    }
    emit_misalignment_check(8, bytes, slow);

    /*
     * Resolve both permissions before establishing the host monitor. x21 is
     * host-owned chain state, so it can retain the translated write address
     * across the one register-only middle instruction. The selector is
     * restored after the store, before the terminating branch can use it.
     */
    emit_translate(8, Access::write, slow, slow, bytes);
    emitter_.move_x(21, 8);
    /*
     * A native conditional store still participates in the publication
     * protocol. Record its possible range before establishing the host
     * monitor; invalidation on a failed attempt is conservative and avoids
     * putting helper work or a status branch inside the native interval.
     */
    track_direct_store(21, bytes, count_ + store_index);
    load_gpr(8, base, true);
    emit_translate(8, Access::read, slow, fault, bytes);
    emitter_.compare_register(8, 21);
    emitter_.branch_condition(slow, 1); // NE

    /*
     * Bind every register needed between the host exclusive load/store before
     * establishing the monitor. The emitted interval then contains only the
     * native load, an optional translation-selected integer operation, and
     * the native store; no CpuState access or helper call can intervene.
     */
    const auto preload = [&](unsigned guest, bool stack_pointer) {
      (void)read_native_gpr(guest, stack_pointer, 10);
    };
    if (has_middle) {
      const auto left = (middle_instruction >> 5) & 31U;
      const auto right = (middle_instruction >> 16) & 31U;
      const bool immediate =
          (middle_instruction & 0x1f80'0000U) == 0x1100'0000U;
      if (left != load_destination)
        preload(left, immediate);
      if (!immediate && right != load_destination)
        preload(right, false);
      const bool writes_flags =
          (middle_instruction & 0x2000'0000U) != 0U;
      const auto destination =
          write_native_gpr(middle_instruction & 31U,
                           immediate && !writes_flags, false, 10);
      (void)destination;
    }
    if (store_source != load_destination &&
        (!has_middle || store_source != (middle_instruction & 31U)))
      preload(store_source, false);
    const auto preallocated_status =
        write_native_gpr(status, false, compare_retry, 9);

    const auto native_destination =
        write_native_gpr(load_destination, false, false, 9);
    auto native_load =
        load_instruction &
        ~((31U << 16) | (31U << 10) | (31U << 5) | 31U);
    native_load |= (31U << 16) | (31U << 10) | (21U << 5) |
                   native_destination.host;
    emitter_.emit(native_load);
    commit_native_gpr(native_destination, load_destination, false);

    ++count_;
    if (has_middle) {
      const auto middle_action =
          emit_instruction(middle_instruction, pc + sizeof(std::uint32_t));
      /* The predecoder above accepts only baseline add/sub or logical forms;
       * each is a nonterminal native lowering. */
      (void)middle_action;
      ++count_;
    }
    if (compare_retry) {
      const auto mismatch = emitter_.make_label();
      const auto target = pc + 8U +
          sign_extend((instructions[2] >> 5) & 0x7ffffU, 19) * 4;
      cold_.push_back({mismatch, instructions[2], target, count_ + 1U,
                      ColdPath::Action::exclusive_compare_mismatch});
      load_flags();
      emitter_.branch_condition(mismatch, 1); // NE
      ++count_;
    }
    mark_direct_memory_write();
    auto native_source = read_native_gpr(store_source, false, 10);
    const auto native_status = preallocated_status;
    if (status != 31U && status == store_source) {
      emitter_.move_x(10, native_source.host);
      native_source = {10U, false};
    }
    auto native_store =
        store_instruction &
        ~((31U << 16) | (31U << 10) | (31U << 5) | 31U);
    native_store |= (native_status.host << 16) | (31U << 10) |
                    (21U << 5) | native_source.host;
    emitter_.emit(native_store);
    commit_native_gpr(native_status, status, false);
    clear_local_exclusive_monitor();
    emitter_.load_b(21, 19, offsetof(JitChainState, link_selector));

    ++count_;
    return store_index + 1U;
  }

void JitCompiler::emit_ls64(std::uint32_t instruction, std::uint64_t pc, bool load,
                 bool status, bool accdata) {
    const unsigned first = instruction & 31U;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned status_register = (instruction >> 16) & 31U;
    if (!load)
      mark_direct_memory_write();

    load_gpr(8, base, true);
    if (base != 31U) {
      const auto tag_slow = emitter_.make_label();
      cold_.push_back(
          {tag_slow, instruction, pc, count_ + 1,
           ColdPath::Action::memory_slow});
      emitter_.load_h(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(tag_slow, 0, false, true);
    }
    const auto alignment = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {alignment, instruction, pc, count_ + 1, ColdPath::Action::memory_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    /* A 64-byte-aligned transfer cannot cross any supported guest page.
     * Resolve alignment once, then use the single-page TLB fast path.
     */
    emitter_.ubfm(0, 8, 0, 5);
    emitter_.compare_zero(alignment, 0, true, true);
    emit_translate(8, load ? Access::read : Access::write, fault, fault, 1);
    if (!load)
      track_direct_store(8, 64);


    /*
     * The native 64-byte forms name x0-x7 implicitly. Some of those registers
     * are ordinary resident-cache hosts, so publish and invalidate their
     * values before entering this constrained-register sequence.
     */
    flush_and_invalidate_gprs();
    if (!load)
      for (unsigned index = 0; index < 8; ++index)
        emitter_.load_x(index, 15,
                        register_offset(first + index, false));
    if (accdata) {
      emitter_.load_w(9, 15, offsetof(CpuState, accdata));
      /* bfxil x0, x9, #0, #32: substitute ACCDATA_EL1[31:0] while
       * preserving the upper half of the first guest register.
       */
      emitter_.emit(0xb340'7d20U);
    }

    if (load) {
      emitter_.emit(0xf83f'd100U); // ld64b x0, [x8]
      for (unsigned index = 0; index < 8; ++index)
        emitter_.store_x(index, 15,
                         register_offset(first + index, false));
    } else if (status || accdata) {
      /* The accumulator form has already materialized its guest-specific
       * first word, so the native status-returning store is equivalent while
       * avoiding dependence on host privileged accumulator state.
       */
      emitter_.emit(0xf829'b100U); // st64bv x9, x0, [x8]
    } else {
      emitter_.emit(0xf83f'9100U); // st64b x0, [x8]
    }
    if (!load && (status || accdata) && status_register != 31U) {
      /* Normal mapped emulator memory defines its successful status as zero,
       * independent of the host memory system's implementation-defined value.
       */
      store_gpr(31, status_register, false);
    }
    if (!load)
      clear_local_exclusive_monitor();
  }

[[nodiscard]] JitCompiler::Action JitCompiler::emit_exclusive_memory(std::uint32_t instruction,
                                             std::uint64_t pc) {
    const bool load = (instruction & 0x0040'0000U) != 0;
    const bool pair = (instruction & 0x0020'0000U) != 0;
    const bool exclusive = (instruction & 0x0080'0000U) == 0;
    const unsigned status = (instruction >> 16) & 31U;
    const unsigned second = (instruction >> 10) & 31U;
    const unsigned size = instruction >> 30;
    const bool unprivileged = (instruction & 0x0100'0000U) != 0;
    const bool pair_compare_swap =
        (instruction & 0xbfa0'0000U) == 0x0820'0000U ||
        (instruction & 0xbfa0'0000U) == 0x0980'0000U;
    const bool scalar_compare_swap =
        (instruction & 0x3fa0'0000U) == 0x08a0'0000U ||
        (instruction & 0xbfa0'0000U) == 0x8980'0000U;
    const bool pair_exclusive =
        (instruction & 0xbfa0'0000U) == 0x8820'0000U;
    const bool scalar_exclusive =
        (instruction & 0x3fa0'0000U) == 0x0800'0000U ||
        (instruction & 0xbfa0'0000U) == 0x8900'0000U;

    bool valid = false;
    if (pair_compare_swap) {
      const unsigned replacement = instruction & 31U;
      valid = second == 31U && (status & 1U) == 0U &&
              (replacement & 1U) == 0U && size <= 1U &&
              (!unprivileged || size == 1U);
    } else if (scalar_compare_swap) {
      valid = second == 31U && (!unprivileged || size == 3U);
    } else if (pair_exclusive) {
      const unsigned first = instruction & 31U;
      valid = size >= 2U &&
              (!load || (status == 31U && first != second));
    } else if (scalar_exclusive) {
      valid = second == 31U && (!load || status == 31U);
    } else {
      /* The remaining allocation is the non-exclusive ordered scalar
       * load/store family. Preserve its dedicated helper. */
      valid = !pair && second == 31U && status == 31U && !exclusive;
      if (!valid)
        return Action::unsupported;
      const auto slow = emitter_.make_label();
      cold_.push_back(
          {slow, instruction, pc, count_ + 1, ColdPath::Action::ordered_slow});
      emitter_.branch(slow);
      return Action::terminate;
    }
    if (!valid)
      return Action::unsupported;
    if (scalar_compare_swap && !unprivileged &&
        host_capabilities().has(ArmFeature::lse)) {
      emit_native_scalar_compare_swap(instruction, pc);
      return Action::keep_going;
    }
    if (pair_compare_swap || scalar_compare_swap || !load)
      mark_memory_write();
    const auto slow = emitter_.make_label();
    cold_.push_back(
        {slow, instruction, pc, count_ + 1,
         ColdPath::Action::exclusive_slow});
    emitter_.branch(slow);
    return Action::terminate;
  }

[[nodiscard]] JitCompiler::Action JitCompiler::emit_atomic_memory(std::uint32_t instruction,
                                          std::uint64_t pc) {
    const bool unprivileged = (instruction & 0x0100'0000U) != 0;
    const unsigned size = instruction >> 30;
    const unsigned o3 = (instruction >> 15) & 1U;
    const unsigned opcode = (instruction >> 12) & 7U;
    const bool valid =
        unprivileged
            ? size <= 1U &&
                  ((!o3 && (opcode == 0U || opcode == 1U || opcode == 3U)) ||
                   (o3 && opcode == 0U))
            : !o3 || opcode == 0U;
    if (!valid)
      return Action::unsupported;
    if (!unprivileged &&
        host_capabilities().has(ArmFeature::lse)) {
      emit_native_scalar_atomic(instruction, pc);
      return Action::keep_going;
    }
    mark_memory_write();
    const auto slow = emitter_.make_label();
    cold_.push_back(
        {slow, instruction, pc, count_ + 1, ColdPath::Action::exclusive_slow});
    emitter_.branch(slow);
    return Action::terminate;
  }

void JitCompiler::emit_native_scalar_atomic(std::uint32_t instruction,
                                 std::uint64_t pc) {
    mark_direct_memory_write();
    const unsigned size = instruction >> 30;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned source = (instruction >> 16) & 31U;
    const unsigned destination = instruction & 31U;
    const std::size_t bytes = std::size_t{1} << size;
    load_gpr(8, base, true);

    const auto slow = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {slow, instruction, pc, count_ + 1,
         ColdPath::Action::exclusive_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1,
         ColdPath::Action::memory_fault});
    if (base == 31U) {
      emit_misalignment_check(8, 16, slow);
    } else {
      emitter_.load_h(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
      emit_misalignment_check(8, bytes, slow);
    }
    emit_translate(8, Access::write, slow, fault, 1);
    track_direct_store(8, bytes);

    /*
     * Rs is consumed before Rt is defined, so a guest register may legally
     * name both operands. Translation also keeps the guest base value away
     * from the host pointer in x8.
     */
    const auto native_source = read_native_gpr(source, false, 9);
    const auto native_destination =
        write_native_gpr(destination, false, false, 10);
    auto native =
        instruction & ~((31U << 16) | (31U << 5) | 31U);
    native |= (native_source.host << 16) | (8U << 5) |
              native_destination.host;
    emitter_.emit(native);
    commit_native_gpr(native_destination, destination, false);
    clear_local_exclusive_monitor();
  }

void JitCompiler::emit_native_scalar_compare_swap(std::uint32_t instruction,
                                       std::uint64_t pc) {
    mark_direct_memory_write();
    const unsigned size = instruction >> 30;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned compare = (instruction >> 16) & 31U;
    const unsigned replacement = instruction & 31U;
    const std::size_t bytes = std::size_t{1} << size;
    load_gpr(8, base, true);

    const auto slow = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {slow, instruction, pc, count_ + 1,
         ColdPath::Action::exclusive_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1,
         ColdPath::Action::memory_fault});
    if (base == 31U) {
      emit_misalignment_check(8, 16, slow);
    } else {
      emitter_.load_h(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
      emit_misalignment_check(8, bytes, slow);
    }
    emit_translate(8, Access::write, slow, fault, 1);
    track_direct_store(8, bytes);

    /*
     * CAS reads both guest operands before publishing the old memory value
     * through the compare register. Keep distinct nonresident scratch
     * registers, while preserving the valid compare==replacement case.
     */
    const auto native_compare =
        read_native_gpr(compare, false, 9);
    const auto native_replacement =
        replacement == compare
            ? native_compare
            : read_native_gpr(replacement, false, 10);
    const auto native_result =
        write_native_gpr(compare, false, false,
                         native_compare.host);
    auto native =
        instruction & ~((31U << 16) | (31U << 5) | 31U);
    native |= (native_result.host << 16) | (8U << 5) |
              native_replacement.host;
    emitter_.emit(native);
    commit_native_gpr(native_result, compare, false);
    clear_local_exclusive_monitor();
  }

void JitCompiler::emit_atomic_floating_memory(
      std::uint32_t instruction, std::uint64_t pc,
      const arm64::detail::AtomicFloating &decoded) {
    mark_direct_memory_write();
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned source = (instruction >> 16) & 31U;
    const unsigned destination = instruction & 31U;
    const std::size_t bytes = decoded.width / 8U;
    load_gpr(8, base, true);

    /*
     * Every dynamic exceptional condition terminates through the exact
     * one-instruction semantic bridge. The normal untagged path performs no
     * opcode decode and reaches one native FEAT_LSFE instruction after a
     * write-TLB lookup.
     */
    const auto slow = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {slow, instruction, pc, count_ + 1, ColdPath::Action::memory_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    if (base == 31U) {
      emit_misalignment_check(8, 16, slow);
    } else {
      emitter_.load_h(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
      emit_misalignment_check(8, bytes, slow);
    }
    /* Natural alignment and a page size divisible by eight prove that this
     * transfer cannot cross a guest-page boundary. */
    emit_translate(8, Access::write, slow, fault, 1);
    track_direct_store(8, bytes);
    /*
     * Resolve vector bindings only after every exceptional edge and its
     * architectural-state snapshot have been recorded. In particular, a
     * fault must not publish an unexecuted result register.
     *
     * Import the source before defining the destination: Rs==Rt is valid and
     * the native instruction must consume the old resident value before
     * replacing it with the returned memory value.
     */
    const auto native_source = read_native_vector(source, 0);
    const auto native_destination =
        decoded.return_old
            ? write_native_vector(destination, false, 1)
            : NativeVector{31U, true};
    auto native =
        instruction & ~((31U << 16) | (31U << 5) | 31U);
    native |= (native_source.host << 16) | (8U << 5) |
              native_destination.host;
    emitter_.emit(native);
    if (decoded.return_old)
      commit_native_vector(native_destination, destination);
    clear_local_exclusive_monitor();
    native_fp_ = true;
  }

[[nodiscard]] bool JitCompiler::emit_pair_memory(std::uint32_t instruction,
                                      std::uint64_t pc) {
    const bool vector = (instruction & 0x0400'0000U) != 0;
    const bool load = (instruction & 0x0040'0000U) != 0;
    const unsigned operation = instruction >> 30;
    if (vector && !host_capabilities().floating_point)
      return false;
    if (vector && operation == 3U &&
        !host_capabilities().has(ArmFeature::lsui))
      return false;
    if (!load)
      mark_direct_memory_write();

    const unsigned mode = (instruction >> 23) & 3U;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned first = instruction & 31U;
    const unsigned second = (instruction >> 10) & 31U;
    const std::size_t bytes =
        vector            ? (operation >= 2U ? 16U
                                             : std::size_t{4} << operation)
        : operation >= 2U ? 8U
                          : 4U;
    const auto displacement =
        sign_extend((instruction >> 15) & 0x7fU, 7) *
        static_cast<std::int64_t>(bytes);
    const auto emit_displacement = [&](unsigned destination,
                                       unsigned source) {
      if (displacement == 0)
        return;
      const auto magnitude = static_cast<unsigned>(
          displacement < 0 ? -displacement : displacement);
      emitter_.emit((displacement < 0 ? 0xd100'0000U : 0x9100'0000U) |
                    (magnitude << 10) | (source << 5) | destination);
    };

    const auto native_base = read_native_gpr(base, true, 8);
    if (native_base.host != 8U)
      emitter_.move_x(8, native_base.host);
    const auto slow = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {slow, instruction, pc, count_ + 1, ColdPath::Action::pair_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    if (base == 31U) {
      /* Pair accesses through SP require the architectural stack alignment
       * check before either memory or writeback can change. */
      emit_misalignment_check(8, 16, slow);
    }
    const bool tagchecked = mode == 1U || mode == 3U || base != 31U;
    if (tagchecked) {
      /* Allocation-tag validation stays on the exact semantic bridge.
       * Untagged programs take the straight native path. */
      emitter_.load_h(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
    }
    if (mode != 1U)
      emit_displacement(8, 8);

    /* The helper remains responsible for cross-page atomicity. The common
     * same-page case needs one TLB lookup and one native pair instruction. */
    emit_translate(8, load ? Access::read : Access::write, slow, fault,
                   bytes * 2U);
    if (!load)
      track_direct_store(8, bytes * 2U);
    NativeGpr native_first_gpr{};
    NativeGpr native_second_gpr{};
    NativeVector native_first_vector{};
    NativeVector native_second_vector{};
    if (vector) {
      if (load) {
        native_first_vector = write_native_vector(first, false, 0);
        native_second_vector =
            second == first
                ? native_first_vector
                : write_native_vector(second, false, 1);
      } else {
        native_first_vector = read_native_vector(first, 0);
        native_second_vector =
            second == first ? native_first_vector
                            : read_native_vector(second, 1);
      }
    } else {
      if (load) {
        native_first_gpr = write_native_gpr(first, false, false, 9);
        native_second_gpr =
            second == first
                ? native_first_gpr
                : write_native_gpr(second, false, false, 10);
      } else {
        native_first_gpr = read_native_gpr(first, false, 9);
        native_second_gpr =
            second == first ? native_first_gpr
                            : read_native_gpr(second, false, 10);
      }
    }

    /* Address generation and writeback were resolved while translating.
     * Preserve the pair operation/mode, replace its immediate with zero, and
     * name the operands' resident host bindings directly. A full cache falls
     * back to the same dedicated scratch registers used by the old path. */
    auto native =
        instruction & ~((0x7fU << 15) | (31U << 10) | (31U << 5) | 31U);
    native |=
        (8U << 5) |
        ((vector ? native_second_vector.host : native_second_gpr.host) << 10) |
        (vector ? native_first_vector.host : native_first_gpr.host);
    emitter_.emit(native);
    if (load) {
      if (vector) {
        commit_native_vector(native_first_vector, first);
        if (second != first)
          commit_native_vector(native_second_vector, second);
      } else {
        commit_native_gpr(native_first_gpr, first, false);
        if (second != first)
          commit_native_gpr(native_second_gpr, second, false);
      }
    } else {
      clear_local_exclusive_monitor();
    }

    const bool writeback =
        (mode == 1U || mode == 3U) &&
        !(load && !vector && base != 31U &&
          (base == first || base == second));
    if (writeback && displacement != 0) {
      const auto native_writeback =
          write_native_gpr(base, true, true, 9);
      emit_displacement(native_writeback.host, native_writeback.host);
      commit_native_gpr(native_writeback, base, true);
    }
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_structure_memory(std::uint32_t instruction,
                                                std::uint64_t pc, bool single) {
    if (!host_capabilities().advanced_simd)
      return false;
    const bool load = (instruction & 0x0040'0000U) != 0;
    const bool post_index = (instruction & 0x0080'0000U) != 0;
    const unsigned offset_register = (instruction >> 16) & 31U;
    if (!post_index && offset_register != 0U)
      return false;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned first = instruction & 31U;
    const std::size_t vector_size =
        (instruction & 0x4000'0000U) != 0 ? 16U : 8U;
    unsigned register_count = 0;
    std::size_t element_size = 0;
    bool load_preserves_destination = false;
    if (!single) {
      const unsigned opcode = (instruction >> 12) & 15U;
      const bool interleaved = opcode == 8U || opcode == 4U || opcode == 0U;
      register_count = opcode == 7U    ? 1U
                       : opcode == 10U ? 2U
                       : opcode == 6U  ? 3U
                       : opcode == 2U  ? 4U
                       : opcode == 8U  ? 2U
                       : opcode == 4U  ? 3U
                       : opcode == 0U  ? 4U
                                       : 0U;
      const unsigned size = (instruction >> 10) & 3U;
      if (register_count == 0U ||
          (interleaved && vector_size == 8U && size == 3U))
        return false;
      element_size = std::size_t{1} << size;
    } else {
      const unsigned extra_register = (instruction >> 21) & 1U;
      const unsigned opcode = (instruction >> 13) & 7U;
      const unsigned scale_bit = (instruction >> 12) & 1U;
      const unsigned size = (instruction >> 10) & 3U;
      const unsigned scale = opcode >> 1;
      if (scale == 3U) {
        if (!load || scale_bit != 0U)
          return false;
        register_count =
            opcode == 6U ? 1U + extra_register : 3U + extra_register;
        element_size = std::size_t{1} << size;
      } else {
        load_preserves_destination = load;
        register_count =
            (opcode & 1U) != 0U ? 3U + extra_register : 1U + extra_register;
        if (scale == 0U) {
          element_size = 1U;
        } else if (scale == 1U) {
          if ((size & 1U) != 0U)
            return false;
          element_size = 2U;
        } else {
          if ((size & 2U) != 0U || ((size & 1U) != 0U && scale_bit != 0U))
            return false;
          element_size = (size & 1U) != 0U ? 8U : 4U;
        }
      }
    }
    const auto transfer_size =
        single ? element_size * register_count : vector_size * register_count;
    if (!load)
      mark_direct_memory_write();

    load_gpr(8, base, true);
    const auto slow = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back({slow, instruction, pc, count_ + 1,
                     ColdPath::Action::simd_structure_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    if (base == 31U) {
      emit_misalignment_check(8, 16, slow);
    }
    if (post_index || base != 31U) {
      /*
       * Untagged programs retain the direct native transfer. Effective MTE
       * state selects the checked cold bridge, which strips the logical tag
       * and validates every touched allocation granule before any transfer.
       */
      emitter_.load_h(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
    }
    emit_translate(8, load ? Access::read : Access::write, slow, fault,
                   transfer_size);
    if (!load)
      track_direct_store(8, transfer_size);
    NativeVector native_vector{};
    if (register_count == 1U) {
      /*
       * A one-register structure operation has an unrestricted Rt field.
       * Keep the guest vector in its resident v16-v31 binding and name that
       * binding in the original instruction.  Selection and validity were
       * resolved above while translating; the generated memory hot path
       * therefore gains no opcode checks or dispatch branches.
       *
       * Lane loads read the untouched lanes, while full and replicate loads
       * overwrite the architectural destination.  A full vector cache uses
       * v0 and the existing commit path exactly as the staged lowering did.
       */
      native_vector =
          load ? write_native_vector(first, load_preserves_destination, 0)
               : read_native_vector(first, 0);
    } else if (!load || load_preserves_destination) {
      for (unsigned index = 0; index < register_count; ++index)
        load_q(index, 15,
               offsetof(CpuState, vectors) +
                   ((first + index) & 31U) *
                       sizeof(arm64::VectorRegister));
    }
    const auto native =
        (instruction & ~(0x0080'0000U | (31U << 16) | (31U << 5) | 31U)) |
        (8U << 5) |
        (register_count == 1U ? native_vector.host : 0U);
    emitter_.emit(native);
    if (load) {
      if (register_count == 1U) {
        commit_native_vector(native_vector, first);
      } else {
        for (unsigned index = 0; index < register_count; ++index)
          store_q(index, 15,
                  offsetof(CpuState, vectors) +
                      ((first + index) & 31U) *
                          sizeof(arm64::VectorRegister));
      }
    } else {
      clear_local_exclusive_monitor();
    }
    if (post_index) {
      load_gpr(8, base, true);
      if (offset_register == 31U)
        emitter_.load_constant(9, transfer_size);
      else
        load_gpr(9, offset_register, false);
      emitter_.add_shifted(8, 8, 9, 0);
      store_gpr(8, base, true);
    }
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_other_memory(std::uint32_t instruction,
                                       std::uint64_t pc) {
    const bool vector = (instruction & 0x0400'0000U) != 0;
    if (vector && !host_capabilities().floating_point)
      return false;
    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    const bool register_offset = (instruction & 0x0020'0000U) != 0;
    const unsigned mode = (instruction >> 10) & 3U;
    const unsigned option = (instruction >> 13) & 7U;
    if (!vector && size_shift == 3U && opc == 2U)
      /* PRFUM/register-offset PRFM are architecturally
       * non-faulting hints. All address work disappears at
       * translation time after validating their encoding. */
      return register_offset ? mode == 2U && (option & 2U) != 0U : mode == 0U;
    if (vector && opc >= 2U && size_shift != 0U)
      return false;
    const std::size_t bytes =
        vector && opc >= 2U ? 16U : std::size_t{1} << size_shift;
    if (!vector && ((bytes == 8U && opc >= 2U) || (bytes == 4U && opc == 3U)))
      return false;
    if ((register_offset && (mode != 2U || (option & 2U) == 0U)) ||
        (!register_offset && vector && mode == 2U))
      return false;
    const bool load = vector ? (opc & 1U) != 0U : opc != 0U;
    if (!load)
      mark_direct_memory_write();
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned transfer = instruction & 31U;
    const auto displacement = sign_extend((instruction >> 12) & 0x1ffU, 9);

    load_gpr(8, base, true);
    if (base == 31U) {
      const auto alignment = emitter_.make_label();
      cold_.push_back({alignment, instruction, pc, count_ + 1,
                       ColdPath::Action::alignment_fault});
      /* Only the translation-selected SP path pays this architectural
       * alignment branch. */
      emit_misalignment_check(8, 16, alignment);
    }
    if (register_offset) {
      const unsigned offset_register = (instruction >> 16) & 31U;
      const unsigned shift =
          (instruction & 0x1000U) != 0U
              ? static_cast<unsigned>(vector && opc >= 2U ? 4U : size_shift)
              : 0U;
      load_gpr(9, offset_register, false);
      emitter_.emit(0x8b20'0000U | (option << 13) | (shift << 10) | (9U << 16) |
                    (8U << 5) | 8U);
    } else if (mode != 1U && displacement != 0) {
      emitter_.load_constant(9, static_cast<std::uint64_t>(displacement));
      emitter_.add_shifted(8, 8, 9, 0);
    }

    const auto fallback = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {fallback, instruction, pc, count_ + 1, ColdPath::Action::memory_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    const bool tagchecked =
        register_offset || mode == 1U || mode == 3U || base != 31U;
    if (tagchecked) {
      /* Tagged-state handling stays on the exact semantic cold path. The
       * ordinary untagged hot path pays one load and one predicted branch. */
      emitter_.load_h(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(fallback, 0, false, true);
    }
    emit_translate(8, load ? Access::read : Access::write, fallback, fault,
                   bytes);
    if (!load)
      track_direct_store(8, bytes);
    const auto native_vector =
        vector
            ? (load ? write_native_vector(transfer, false, 0)
                    : read_native_vector(transfer, 0))
            : NativeVector{};
    const auto native_gpr =
        !vector
            ? (load ? write_native_gpr(transfer, false, false, 9)
                    : read_native_gpr(transfer, false, 9))
            : NativeGpr{};
    const auto native =
        0x3900'0000U |
        (instruction & (0xc000'0000U | 0x0400'0000U | 0x00c0'0000U)) |
        (8U << 5) | (vector ? native_vector.host : native_gpr.host);
    emitter_.emit(native);
    if (load) {
      if (vector)
        commit_native_vector(native_vector, transfer);
      else
        commit_native_gpr(native_gpr, transfer, false);
    } else {
      clear_local_exclusive_monitor();
    }

    const bool writeback =
        !register_offset && (mode == 1U || mode == 3U) &&
        !(load && !vector && base == transfer && base != 31U);
    if (writeback) {
      load_gpr(8, base, true);
      if (displacement != 0) {
        emitter_.load_constant(9, static_cast<std::uint64_t>(displacement));
        emitter_.add_shifted(8, 8, 9, 0);
      }
      store_gpr(8, base, true);
    }
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_literal_memory(std::uint32_t instruction,
                                         std::uint64_t pc) {
    if ((instruction & 0xff00'0000U) == 0xd800'0000U)
      /* Literal PRFM is an architecturally non-faulting hint. */
      return true;
    if ((instruction & 0x3b00'0000U) != 0x1800'0000U)
      return false;
    const bool vector = (instruction & 0x0400'0000U) != 0;
    const unsigned operation = instruction >> 30;
    if ((vector && !host_capabilities().floating_point) ||
        (vector && operation == 3U))
      return false;
    const std::size_t bytes = vector            ? std::size_t{4} << operation
                              : operation == 1U ? 8U
                                                : 4U;
    const auto displacement =
        sign_extend((instruction >> 5) & 0x7ffffU, 19) * 4;
    const auto address = pc + static_cast<std::uint64_t>(displacement);
    const unsigned destination = instruction & 31U;
    emitter_.load_constant(8, address);
    const auto fallback = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {fallback, instruction, pc, count_ + 1, ColdPath::Action::memory_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    emit_translate(8, Access::read, fallback, fault, bytes);
    if (vector) {
      static constexpr std::array<std::uint32_t, 3> loads{
          0xbd40'0100U, // ldr s0, [x8]
          0xfd40'0100U, // ldr d0, [x8]
          0x3dc0'0100U, // ldr q0, [x8]
      };
      const auto native_destination =
          write_native_vector(destination, false, 0);
      emitter_.emit(loads[operation] | native_destination.host);
      commit_native_vector(native_destination, destination);
    } else {
      static constexpr std::array<std::uint32_t, 3> loads{
          0xb940'0109U, // ldr w9, [x8]
          0xf940'0109U, // ldr x9, [x8]
          0xb980'0109U, // ldrsw x9, [x8]
      };
      const auto native_destination =
          write_native_gpr(destination, false, false, 9);
      emitter_.emit((loads[operation] & ~31U) | native_destination.host);
      commit_native_gpr(native_destination, destination, false);
    }
    return true;
  }

[[nodiscard]] bool JitCompiler::decode_simd_unsigned_memory(
      std::uint32_t instruction,
      SimdUnsignedMemoryOperation &operation) noexcept {
    if ((instruction & 0x3b00'0000U) != 0x3900'0000U ||
        (instruction & 0x0400'0000U) == 0U)
      return false;

    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    if (opc >= 2U && size_shift != 0U)
      return false;

    const std::size_t bytes =
        opc >= 2U ? 16U : std::size_t{1} << size_shift;
    operation = {
        .instruction = instruction,
        .offset = static_cast<std::uint64_t>(
                      (instruction >> 10) & 0xfffU) *
                  bytes,
        .bytes = bytes,
        .base = (instruction >> 5) & 31U,
        .transfer = instruction & 31U,
        .load = (opc & 1U) != 0U,
    };
    return true;
  }

[[nodiscard]] std::size_t JitCompiler::try_emit_simd_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    constexpr std::size_t maximum_run = 8;
    if (track_direct_stores_ || !host_capabilities().floating_point ||
        instructions.size() < 2)
      return 0;

    std::array<SimdUnsignedMemoryOperation, maximum_run> operations{};
    std::array<std::uint64_t, maximum_run> maximum_ends{};
    std::size_t candidates = 0;
    unsigned base = 0;
    bool load = false;
    std::uint64_t maximum_end = 0;
    while (candidates < operations.size() &&
           candidates < instructions.size()) {
      SimdUnsignedMemoryOperation operation{};
      if (!decode_simd_unsigned_memory(instructions[candidates], operation))
        break;
      if (candidates == 0) {
        base = operation.base;
        load = operation.load;
      } else if (operation.base != base || operation.load != load) {
        break;
      }
      maximum_end =
          std::max(maximum_end, operation.offset + operation.bytes);
      operations[candidates] = operation;
      maximum_ends[candidates] = maximum_end;
      ++candidates;
    }

    std::size_t length = candidates;
    while (length >= 2 && maximum_ends[length - 1] > page_size)
      --length;
    if (length < 2)
      return 0;

    /*
     * Every guard and the permission lookup precedes the first architectural
     * memory operation.  A rejected proof therefore enters the ordinary
     * semantic path for operation zero with the complete dirty prefix still
     * visible, preserving its effective fault address and retirement count.
     */
    const auto slow = emitter_.make_label();
    cold_.push_back({slow, operations[0].instruction, pc, count_ + 1,
                     ColdPath::Action::memory_slow});

    Arm64Emitter::Label alignment{};
    if (base == 31U) {
      alignment = emitter_.make_label();
      cold_.push_back({alignment, operations[0].instruction, pc, count_ + 1,
                       ColdPath::Action::alignment_fault});
    }

    load_gpr(8, base, true);
    if (base == 31U) {
      /* All retained operations name SP, so its architectural alignment is
       * checked once before any destination or memory becomes observable. */
      emit_misalignment_check(8, 16, alignment);
    } else {
      /*
       * Tagged-address handling remains on the existing exact semantic path.
       * Consecutive operations share the immutable effective state, so one
       * translation-selected check is sufficient for the complete run.
       */
      emitter_.load_h(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
    }

    maximum_end = maximum_ends[length - 1];
    if (base == 31U) {
      const auto last_unit =
          static_cast<unsigned>((maximum_end - 1U) >> 4U);
      if (last_unit != 0U) {
        /*
         * SP is 16-byte aligned.  Prove the furthest retained byte remains in
         * the page before translating SP itself once.
         */
        emitter_.ubfm(0, 8, 4, page_shift - 1);
        emitter_.add_immediate(0, 0, last_unit);
        emitter_.test_zero(slow, 0, page_shift - 4U, true);
      }
    } else {
      const auto last_delta = maximum_end - 1U;
      if (last_delta != 0U) {
        if (last_delta <= 0xfffU) {
          emitter_.add_immediate(0, 8,
                                 static_cast<unsigned>(last_delta));
        } else {
          emitter_.load_constant(0, last_delta);
          emitter_.add_shifted(0, 8, 0, 0);
        }
        emitter_.emit(0xca08'0000U); // eor x0, x0, x8
        emitter_.test_zero(slow, 0, page_shift,
                           true); // tbnz x0, #page_shift
      }
    }

    emit_translate(8, load ? Access::read : Access::write, slow, slow, 1);
    for (std::size_t index = 0; index < length; ++index) {
      const auto &operation = operations[index];
      const auto native_vector =
          load ? write_native_vector(operation.transfer, false, 0)
               : read_native_vector(operation.transfer, 0);
      const auto native =
          (operation.instruction & ~((31U << 5) | 31U)) | (8U << 5) |
          native_vector.host;
      emitter_.emit(native);
      if (load)
        commit_native_vector(native_vector, operation.transfer);
    }

    if (!load) {
      for (std::size_t index = 0; index < length; ++index) {
        const auto instruction_index = count_ + index;
        if (instruction_index >= maximum_block_instructions)
          break;
        const auto bit = std::uint64_t{1} << instruction_index;
        memory_write_mask_ |= bit;
        direct_memory_write_mask_ |= bit;
      }
      clear_local_exclusive_monitor();
    }
    return length;
  }

[[nodiscard]] bool JitCompiler::decode_sp_unsigned_memory(
      std::uint32_t instruction,
      SpUnsignedMemoryOperation &operation) noexcept {
    if ((instruction & 0x3b00'0000U) != 0x3900'0000U ||
        (instruction & 0x0400'0000U) != 0U ||
        ((instruction >> 5) & 31U) != 31U)
      return false;

    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    if ((size_shift == 3U && opc >= 2U) ||
        (size_shift == 2U && opc == 3U))
      return false;

    operation = {
        .instruction = instruction,
        .offset = static_cast<std::uint64_t>(
                      (instruction >> 10) & 0xfffU)
                  << size_shift,
        .bytes = std::size_t{1} << size_shift,
        .transfer = instruction & 31U,
        .load = opc != 0U,
    };
    return true;
  }

[[nodiscard]] std::size_t JitCompiler::try_emit_sp_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    constexpr std::size_t maximum_run = 4;
    if (track_direct_stores_ || instructions.size() < 2)
      return 0;

    std::array<SpUnsignedMemoryOperation, maximum_run> operations{};
    std::array<std::uint64_t, maximum_run> maximum_ends{};
    std::size_t candidates = 0;
    bool load = false;
    std::uint64_t maximum_end = 0;
    while (candidates < operations.size() &&
           candidates < instructions.size()) {
      SpUnsignedMemoryOperation operation{};
      if (!decode_sp_unsigned_memory(instructions[candidates], operation))
        break;
      if (candidates == 0)
        load = operation.load;
      else if (operation.load != load)
        break;
      maximum_end =
          std::max(maximum_end, operation.offset + operation.bytes);
      operations[candidates] = operation;
      maximum_ends[candidates] = maximum_end;
      ++candidates;
    }

    std::size_t length = candidates;
    while (length >= 2 && maximum_ends[length - 1] > page_size)
      --length;
    if (length < 2)
      return 0;

    const auto alignment = emitter_.make_label();
    const auto slow = emitter_.make_label();
    cold_.push_back({alignment, operations[0].instruction, pc, count_ + 1,
                     ColdPath::Action::alignment_fault});
    cold_.push_back({slow, operations[0].instruction, pc, count_ + 1,
                     ColdPath::Action::memory_slow});

    load_gpr(8, 31, true);
    /* Every selected instruction names SP, so check its architectural
     * alignment once before observing any memory or destination register. */
    emit_misalignment_check(8, 16, alignment);

    maximum_end = maximum_ends[length - 1];
    const auto last_unit =
        static_cast<unsigned>((maximum_end - 1U) >> 4U);
    if (last_unit != 0U) {
      /*
       * SP is known to be 16-byte aligned. q+last_unit is below twice the
       * number of units in a page, so its page bit is the complete crossing
       * test.
       */
      emitter_.ubfm(0, 8, 4, page_shift - 1);
      emitter_.add_immediate(0, 0, last_unit);
      emitter_.test_zero(slow, 0, page_shift - 4U, true);
    }

    emit_translate(8, load ? Access::read : Access::write, slow, slow, 1);
    for (std::size_t index = 0; index < length; ++index) {
      const auto &operation = operations[index];
      const auto native_gpr =
          load ? write_native_gpr(operation.transfer, false, false, 9)
               : read_native_gpr(operation.transfer, false, 9);
      const auto native =
          (operation.instruction & ~((31U << 5) | 31U)) | (8U << 5) |
          native_gpr.host;
      emitter_.emit(native);
      if (load)
        commit_native_gpr(native_gpr, operation.transfer, false);
    }

    if (!load) {
      for (std::size_t index = 0; index < length; ++index) {
        const auto instruction_index = count_ + index;
        if (instruction_index >= maximum_block_instructions)
          break;
        const auto bit = std::uint64_t{1} << instruction_index;
        memory_write_mask_ |= bit;
        direct_memory_write_mask_ |= bit;
      }
      clear_local_exclusive_monitor();
    }
    return length;
  }

[[nodiscard]] bool JitCompiler::decode_gpr_unsigned_memory(
      std::uint32_t instruction,
      GprUnsignedMemoryOperation &operation) noexcept {
    if ((instruction & 0x3b00'0000U) != 0x3900'0000U ||
        (instruction & 0x0400'0000U) != 0U)
      return false;

    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    const unsigned base = (instruction >> 5) & 31U;
    if (base == 31U || (size_shift == 3U && opc >= 2U) ||
        (size_shift == 2U && opc == 3U))
      return false;

    operation = {
        .instruction = instruction,
        .offset = static_cast<std::uint64_t>(
                      (instruction >> 10) & 0xfffU)
                  << size_shift,
        .bytes = std::size_t{1} << size_shift,
        .base = base,
        .transfer = instruction & 31U,
        .load = opc != 0U,
    };
    return true;
  }

[[nodiscard]] std::size_t JitCompiler::try_emit_gpr_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    constexpr std::size_t maximum_run = 4;
    if (track_direct_stores_ || instructions.size() < 2)
      return 0;

    std::array<GprUnsignedMemoryOperation, maximum_run> operations{};
    std::array<std::uint64_t, maximum_run> maximum_ends{};
    std::size_t candidates = 0;
    unsigned base = 0;
    bool load = false;
    std::uint64_t maximum_end = 0;
    while (candidates < operations.size() &&
           candidates < instructions.size()) {
      GprUnsignedMemoryOperation operation{};
      if (!decode_gpr_unsigned_memory(instructions[candidates], operation))
        break;
      if (candidates == 0) {
        base = operation.base;
        load = operation.load;
      } else if (operation.base != base || operation.load != load) {
        break;
      }
      /*
       * A load into Rn changes the architectural base observed by every
       * following instruction. Keeping the translated host base in x8 would
       * therefore be stale. Store Rt==Rn is safe because stores do not change
       * the source register.
       */
      if (load && operation.transfer == base)
        break;
      maximum_end =
          std::max(maximum_end, operation.offset + operation.bytes);
      operations[candidates] = operation;
      maximum_ends[candidates] = maximum_end;
      ++candidates;
    }

    std::size_t length = candidates;
    while (length >= 2 && maximum_ends[length - 1] > page_size)
      --length;
    if (length < 2)
      return 0;

    /*
     * Record the semantic first-instruction edge before loading or allocating
     * any run register. If the page proof or permission translation fails,
     * the ordinary slow helper executes exactly operation zero with the
     * architectural prefix state and precise fault address.
     */
    const auto slow = emitter_.make_label();
    cold_.push_back({slow, operations[0].instruction, pc, count_ + 1,
                     ColdPath::Action::memory_slow});

    load_gpr(8, base, true);
    emitter_.load_h(0, 15, offsetof(CpuState, mte_enabled));
    emitter_.compare_zero(slow, 0, false, true);
    maximum_end = maximum_ends[length - 1];
    const auto last_delta = maximum_end - 1U;
    if (last_delta != 0U) {
      if (last_delta <= 0xfffU) {
        emitter_.add_immediate(0, 8,
                               static_cast<unsigned>(last_delta));
      } else {
        emitter_.load_constant(0, last_delta);
        emitter_.add_shifted(0, 8, 0, 0);
      }
      /*
       * last_delta is translation-time bounded below one guest page. The page
       * bit changes iff base..base+last_delta crosses a page boundary.
       */
      emitter_.emit(0xca08'0000U); // eor x0, x0, x8
      emitter_.test_zero(slow, 0, page_shift, true); // tbnz x0, #page_shift
    }

    emit_translate(8, load ? Access::read : Access::write, slow, slow, 1);
    for (std::size_t index = 0; index < length; ++index) {
      const auto &operation = operations[index];
      const auto native_gpr =
          load ? write_native_gpr(operation.transfer, false, false, 9)
               : read_native_gpr(operation.transfer, false, 9);
      const auto native =
          (operation.instruction & ~((31U << 5) | 31U)) | (8U << 5) |
          native_gpr.host;
      emitter_.emit(native);
      if (load)
        commit_native_gpr(native_gpr, operation.transfer, false);
    }

    if (!load) {
      for (std::size_t index = 0; index < length; ++index) {
        const auto instruction_index = count_ + index;
        if (instruction_index >= maximum_block_instructions)
          break;
        const auto bit = std::uint64_t{1} << instruction_index;
        memory_write_mask_ |= bit;
        direct_memory_write_mask_ |= bit;
      }
      clear_local_exclusive_monitor();
    }
    return length;
  }

[[nodiscard]] bool JitCompiler::emit_unsigned_memory(std::uint32_t instruction,
                                          std::uint64_t pc) {
    const bool vector = (instruction & 0x0400'0000U) != 0;
    if (vector && !host_capabilities().floating_point)
      return false;
    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    /*
     * PRFM is an architecturally non-faulting hint.  Its Rt field is a
     * prefetch operation, not a destination register.  Treating opc=2
     * uniformly as a signed load corrupts that register and can also
     * introduce a spurious memory fault.
     */
    if (!vector && size_shift == 3U && opc == 2U)
      return true;
    if (vector && opc >= 2U && size_shift != 0U)
      return false;
    const std::size_t bytes =
        vector && opc >= 2U ? 16U : std::size_t{1} << size_shift;
    if (!vector && ((bytes == 8U && opc >= 2U) || (bytes == 4U && opc == 3U)))
      return false;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned transfer = instruction & 31U;
    const auto offset =
        static_cast<std::uint64_t>((instruction >> 10) & 0xfffU) * bytes;
    /* opc==0 is the only scalar store allocation. Signed loads use
     * opc==2/3, so testing only the ordinary-load bit would silently
     * discard every LDRSB/LDRSH/LDRSW result.
     */
    const bool load = vector ? (opc & 1U) != 0 : opc != 0;
    if (!load)
      mark_direct_memory_write();
    load_gpr(8, base, true);
    if (base == 31U) {
      const auto alignment = emitter_.make_label();
      cold_.push_back({alignment, instruction, pc, count_ + 1,
                       ColdPath::Action::alignment_fault});
      emit_misalignment_check(8, 16, alignment);
    }
    if (offset != 0) {
      emitter_.load_constant(9, offset);
      emitter_.add_shifted(8, 8, 9, 0);
    }
    const auto fallback = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {fallback, instruction, pc, count_ + 1, ColdPath::Action::memory_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    if (base != 31U) {
      emitter_.load_h(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(fallback, 0, false, true);
    }
    emit_translate(8, load ? Access::read : Access::write, fallback, fault,
                   bytes);
    if (!load)
      track_direct_store(8, bytes);
    const auto native_vector =
        vector
            ? (load ? write_native_vector(transfer, false, 0)
                    : read_native_vector(transfer, 0))
            : NativeVector{};
    const auto native_gpr =
        !vector
            ? (load ? write_native_gpr(transfer, false, false, 9)
                    : read_native_gpr(transfer, false, 9))
            : NativeGpr{};
    const auto native = (instruction & ~((0xfffU << 10) | (31U << 5) | 31U)) |
                        (8U << 5) |
                        (vector ? native_vector.host : native_gpr.host);
    emitter_.emit(native);
    if (load) {
      if (vector)
        commit_native_vector(native_vector, transfer);
      else
        commit_native_gpr(native_gpr, transfer, false);
    } else {
      clear_local_exclusive_monitor();
    }
    return true;
  }

} // namespace uml::cpu::arm64
