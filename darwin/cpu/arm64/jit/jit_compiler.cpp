// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_compiler.hpp"

#include "cpu/arm64/code_synchronization.hpp"
#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/detail/fp_state_access.hpp"
#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/memory_copy_set.hpp"
#include "cpu/arm64/detail/read_check_write.hpp"
#include "cpu/arm64/id_registers.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace uml::cpu::arm64 {

JitCompiler::JitCompiler(void *link_context,
                       LinkResolver link_resolver,
                       bool track_direct_stores,
                       IndirectLinkResolver indirect_link_resolver) noexcept
      : emitter_{std::size_t{1} << 18U},
        cold_{this},
        configured_link_context_{link_context},
        configured_link_resolver_{link_resolver},
        configured_indirect_link_resolver_{indirect_link_resolver},
        track_direct_stores_{track_direct_stores} {
    attach_family_emitter(&emitter_, &native_fp_, &native_fpmr_,
                          !track_direct_stores_);
    attach_link_resolver(link_context, link_resolver);
    attach_indirect_link_resolver(link_context, indirect_link_resolver);
    emitter_.attach_exit_flush(this, [](void *context) {
      static_cast<JitCompiler *>(context)->flush_exit_gprs();
    });
    emitter_.attach_flag_clobber(this, [](void *context) {
      static_cast<JitCompiler *>(context)
          ->spill_flags_for_internal_clobber();
    });
  }

JitCompiler::JitCompiler(
              const ArmFeatureSet &translation_features) noexcept
      : JitCompiler{} {
    translation_features_ = &translation_features;
  }

void JitCompiler::set_table_list_planning_enabled(bool enabled) noexcept {
    table_list_planning_enabled_ = enabled;
  }

JitCompiler::Result JitCompiler::compile(std::uint64_t guest_pc,
                 std::span<const std::uint32_t> instructions) {
    return compile_once(guest_pc, instructions);
  }

JitCompiler::Result JitCompiler::compile_once(std::uint64_t guest_pc,
                      std::span<const std::uint32_t> instructions) {
    begin_link_observation();
    select_constrained_vector_pins(instructions);
    if (table_list_planning_enabled_)
      select_consecutive_vector_lists(instructions);
    emitter_.prologue();
    const auto internal_entry_word = emitter_.size();
    while (count_ < instructions.size()) {
      const auto pc = guest_pc + count_ * sizeof(std::uint32_t);
      if (const auto run = try_emit_simd_unsigned_run(
              instructions.subspan(count_), pc);
          run != 0) {
        accesses_fp_state_ = true;
        count_ += run;
        continue;
      }
      if (const auto run = try_emit_sp_unsigned_run(
              instructions.subspan(count_), pc);
          run != 0) {
        count_ += run;
        continue;
      }
      if (const auto run = try_emit_gpr_unsigned_run(
              instructions.subspan(count_), pc);
          run != 0) {
        count_ += run;
        continue;
      }
      if (try_emit_native_exclusive_run(instructions.subspan(count_), pc))
        continue;
      if (const auto run = try_emit_memory_copy_set_sequence(
              instructions.subspan(count_), pc);
          run != 0) {
        count_ += run;
        terminated_ = true;
        break;
      }
      const auto instruction = instructions[count_];
      if (reservation_sensitive_allocation(instruction)) {
        /*
         * A reservation-establishing helper waits for every other vCPU's
         * published direct-store block.  It therefore cannot execute inside
         * a publication inherited from an earlier instruction or linked
         * predecessor: two such vCPUs could otherwise wait on one another.
         *
         * Split before the allocation and mark its one-instruction block as
         * an ineligible chain target.  Both decisions happen while
         * translating, leaving ordinary generated blocks branch-free.
         */
        if (count_ != 0) {
          store_pc(pc);
          emitter_.return_constant(Event::continued, count_);
          terminated_ = true;
          break;
        }
        link_target_safe_ = false;
      }
      const auto action = emit_instruction(instruction, pc);
      if (action == Action::unsupported) {
        /*
         * Keep a supported native prefix instead of discarding it
         * merely because the following instruction is unsupported.
         * A block beginning with an unsupported or unavailable
         * instruction becomes a precise guest illegal-instruction
         * terminal. The generated block contains no runtime decode or
         * feature check, and the faulting instruction does not retire.
         */
        if (count_ == 0) {
          guest_illegal_ = true;
          store_pc(pc);
          emitter_.return_constant(Event::illegal_instruction, count_ + 1U);
          ++count_;
          terminated_ = true;
        } else {
          store_pc(pc);
          emitter_.return_constant(Event::continued, count_);
          terminated_ = true;
        }
        break;
      }
      accesses_fp_state_ =
          accesses_fp_state_ ||
          arm64::detail::instruction_accesses_fp_state(instruction);
      ++count_;
      if (action == Action::terminate) {
        terminated_ = true;
        break;
      }
    }
    if (!terminated_) {
      store_pc(guest_pc + count_ * sizeof(std::uint32_t));
      emitter_.return_constant(Event::continued, count_);
    }
    const auto hot_words = emitter_.size();
    for (const auto &translation : translation_cold_)
      emit_translation_cold(translation);
    for (const auto &cold : cold_) {
      set_exit_gpr_mask(cold.dirty_gprs);
      set_exit_vector_mask(cold.dirty_vectors);
      emitter_.bind(cold.label);
      /*
       * Every edge leading here is flag-neutral. Publish the translation-time
       * snapshot only on the cold arm, then let the existing terminal helpers
       * consume CpuState as their canonical source.
       */
      publish_cold_flags(cold.flags);
      assume_flags_in_memory();
      if (cold.action == ColdPath::Action::memory_fault) {
        store_pc(cold.pc);
        emitter_.return_constant(Event::memory_fault, cold.count);
      } else if (cold.action == ColdPath::Action::pair_slow) {
        emit_pair_slow(cold.instruction, cold.pc, cold.count);
      } else if (cold.action == ColdPath::Action::simd_structure_slow) {
        emit_simd_structure_slow(cold.instruction, cold.pc, cold.count);
      } else if (cold.action == ColdPath::Action::alignment_fault) {
        emit_alignment_fault(cold.instruction, cold.pc, cold.count);
      } else if (cold.action == ColdPath::Action::ordered_slow) {
        emit_ordered_slow(cold.instruction, cold.pc, cold.count);
      } else if (cold.action == ColdPath::Action::exclusive_slow) {
        /*
         * A native atomic block publishes its possible direct store for the
         * whole generated invocation. Its semantic fallback must therefore
         * unwind that publication before entering the serialized exclusive
         * helper. Otherwise two vCPUs can deadlock: the turn owner waits for
         * the other publication while that vCPU waits for the turn with its
         * own publication still active.
         */
        if (direct_memory_write_mask_ != 0U)
          emit_deferred_exclusive_slow(cold.instruction, cold.pc,
                                       cold.count);
        else
          emit_exclusive_slow(cold.instruction, cold.pc, cold.count);
      } else if (cold.action ==
                 ColdPath::Action::deferred_exclusive_slow) {
        emit_deferred_exclusive_slow(cold.instruction, cold.pc, cold.count);
      } else if (cold.action == ColdPath::Action::memory_slow) {
        emit_memory_slow(cold.instruction, cold.pc, cold.count);
      } else if (cold.action == ColdPath::Action::exclusive_compare_mismatch) {
        emitter_.emit(0xd503'3f5fU); // clrex
        clear_local_exclusive_monitor();
        store_pc(cold.pc);
        emitter_.return_constant(Event::continued, cold.count);
      }
    }
    const auto words = emitter_.finish();
    Result result{
        .words = words,
        .relocations = emitter_.relocations(),
        .relocatable = emitter_.relocatable(),
        .direct_link_refusals = direct_link_refusals(),
        .indirect_link_refusals = indirect_link_refusals(),
        .guest_instructions = count_,
        .hot_words = hot_words,
        .internal_entry_word = internal_entry_word,
        .memory_write_mask = memory_write_mask_,
        .direct_memory_write_mask = direct_memory_write_mask_,
        .accesses_fp_state = accesses_fp_state_,
        .link_target_safe = link_target_safe_,
        .native_fp = native_fp_,
        .native_fpmr = native_fpmr_,
        .guest_illegal = guest_illegal_,
    };
    return result;
  }

void JitCompiler::select_constrained_vector_pins(
      std::span<const std::uint32_t> instructions) noexcept {
    std::array<unsigned, 32> frequency{};
    std::array<std::size_t, 32> first_use{};
    first_use.fill(std::numeric_limits<std::size_t>::max());
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const bool reservation_boundary =
          reservation_sensitive_allocation(instructions[index]);
      if (index != 0U && reservation_boundary)
        break;
      const auto guest = constrained_vector_source(instructions[index]);
      if (guest) {
        ++frequency[*guest];
        first_use[*guest] = std::min(first_use[*guest], index);
      }
      if (reservation_boundary ||
          obvious_translated_block_terminator(instructions[index]))
        break;
    }

    for (std::size_t pin = 0; pin < 2U; ++pin) {
      unsigned selected = 32U;
      for (unsigned guest = 0; guest < 32U; ++guest) {
        if (frequency[guest] == 0U)
          continue;
        if (selected == 32U ||
            frequency[guest] > frequency[selected] ||
            (frequency[guest] == frequency[selected] &&
             first_use[guest] < first_use[selected]))
          selected = guest;
      }
      if (selected == 32U)
        break;
      pin_constrained_vector(selected);
      frequency[selected] = 0U;
    }
  }

void JitCompiler::select_consecutive_vector_lists(
      std::span<const std::uint32_t> instructions) noexcept {
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const bool reservation_boundary =
          reservation_sensitive_allocation(instructions[index]);
      if (index != 0U && reservation_boundary)
        break;
      if (const auto list =
              consecutive_vector_list(instructions[index]))
        static_cast<void>(
            pin_consecutive_vector_list(list->first, list->count));
      if (reservation_boundary ||
          obvious_translated_block_terminator(instructions[index]))
        break;
    }
  }

[[nodiscard]] bool
  JitCompiler::translation_supports(ArmFeature feature) const noexcept {
    if (translation_features_ != nullptr)
      return translation_features_->test(
          static_cast<std::size_t>(feature));
    return host_capabilities().has(feature);
  }

[[nodiscard]] bool
  JitCompiler::reservation_sensitive_allocation(std::uint32_t instruction) noexcept {
    const bool native_scalar_lse =
        (instruction & 0x0100'0000U) == 0U &&
        host_capabilities().has(ArmFeature::lse) &&
        ((instruction & 0x3f20'0c00U) == 0x3820'0000U ||
         (instruction & 0x3fa0'0000U) == 0x08a0'0000U);
    if (native_scalar_lse)
      return false;
    const bool read_check_write =
        detail::decode_read_check_write(instruction).recognized;
    const bool pair_atomic =
        (instruction & 0xbf20'0c00U) == 0x1920'0000U;
    const bool scalar_atomic =
        (instruction & 0x3f20'0c00U) == 0x3820'0000U ||
        (instruction & 0x3f20'0c00U) == 0x1920'0400U;
    const bool exclusive_or_ordered =
        (instruction & 0x3f00'0000U) == 0x0800'0000U ||
        (instruction & 0x3f00'0000U) == 0x0900'0000U;
    return read_check_write || pair_atomic || scalar_atomic ||
           exclusive_or_ordered;
  }

void JitCompiler::mark_memory_write() noexcept {
    if (count_ < maximum_block_instructions)
      memory_write_mask_ |= std::uint64_t{1} << count_;
  }

void JitCompiler::mark_direct_memory_write() noexcept {
    mark_memory_write();
    if (count_ < maximum_block_instructions)
      direct_memory_write_mask_ |= std::uint64_t{1} << count_;
  }

void JitCompiler::track_direct_store(unsigned address, std::size_t size) {
    track_direct_store(address, size, count_);
  }

void JitCompiler::track_direct_store(unsigned address, std::size_t size,
                          std::size_t instruction_index) {
    if (!track_direct_stores_ ||
        instruction_index >= maximum_block_instructions)
      return;
    /* Every caller records after emit_translate(), so address is the
     * permission-checked host backing pointer rather than a guest VA.
     */
    const auto offset =
        instruction_index * sizeof(JitStoreRange);
    emitter_.load_x(3, 31, 32);
    emitter_.load_x(3, 3, offsetof(JitChainState, stores));
    emitter_.store_x(address, 3, offset + offsetof(JitStoreRange, address));
    emitter_.load_constant(4, size);
    emitter_.store_x(4, 3, offset + offsetof(JitStoreRange, size));
  }

[[nodiscard]] bool JitCompiler::emit_primary_hint(std::uint32_t instruction) {
    if ((instruction & 0xffff'f01fU) != 0xd503'201fU)
      return false;

    /*
     * CHKFEAT and GCSB are NOPs: the virtual CPU enables none of the
     * features CHKFEAT reports, so X16 keeps its value.
     */
    const unsigned immediate = (instruction >> 5) & 0x7fU;
    /*
     * These hints directly affect the host execution carrying the guest
     * block. Optional HINT-space encodings are architecturally NOP on older
     * hosts, so no runtime feature branch or fallback sequence is needed.
     */
    if (immediate == 1U || immediate == 6U || immediate == 20U ||
        immediate == 22U) {
      emitter_.emit(instruction);
      return true;
    }
    return true;
  }

void JitCompiler::mask_register(unsigned reg, std::uint64_t mask, bool wide) {
    emitter_.load_constant(9, mask);
    emitter_.emit((wide ? 0x8a00'0000U : 0x0a00'0000U) | (9U << 16) |
                  ((reg & 31U) << 5) | (reg & 31U));
  }

[[nodiscard]] bool JitCompiler::emit_system_register(std::uint32_t instruction) {
    constexpr std::uint32_t fpcr_mask = arm64::fpcr_writable_mask;
    constexpr std::uint32_t fpsr_mask = 0xf800'009fU;
    constexpr std::uint64_t fpmr_mask = 0x0000'003f'ff7f'c1ffULL;
    const auto system = instruction & 0xffff'ffe0U;
    const unsigned transfer = instruction & 31U;
    const auto write_destination = [&](auto emit_value) {
      if (transfer == 31U)
        return;
      const auto native_destination =
          write_native_gpr(transfer, false, false, 8);
      emit_value(native_destination.host);
      commit_native_gpr(native_destination, transfer, false);
    };
    const auto copy_source_to_scratch = [&] {
      const auto native_source =
          read_native_gpr(transfer, false, 8);
      if (native_source.host != 8U)
        emitter_.move_x(8, native_source.host);
    };
    switch (system) {
    case 0xd53b'd040U: // TPIDR_EL0
      write_destination([&](unsigned destination) {
        emitter_.load_x(destination, 15, offsetof(CpuState, tls));
      });
      return true;
    case 0xd51b'd040U: {
      const auto native_source =
          read_native_gpr(transfer, false, 8);
      emitter_.store_x(native_source.host, 15, offsetof(CpuState, tls));
      return true;
    }
    case 0xd53b'd060U: // TPIDRRO_EL0
      write_destination(
          [&](unsigned destination) {
            emitter_.load_constant(destination, 0);
          });
      return true;
    case 0xd53b'00e0U: // DCZID_EL0
      write_destination(
          [&](unsigned destination) {
            emitter_.load_constant(destination, 4U);
          });
      return true;
    case 0xd53b'0020U: // CTR_EL0
      write_destination([&](unsigned destination) {
        emitter_.load_constant(destination, 0x8444'8004U);
      });
      return true;
    case 0xd53b'4400U: // FPCR
      write_destination([&](unsigned destination) {
        emitter_.load_w(destination, 15, offsetof(CpuState, fpcr));
        mask_register(destination, fpcr_mask, false);
      });
      return true;
    case 0xd51b'4400U:
      copy_source_to_scratch();
      mask_register(8, fpcr_mask, false);
      emitter_.store_w(8, 15, offsetof(CpuState, fpcr));
      mask_register(
          8, fpcr_mask & ~arm64::detail::fpcr_exception_enable_mask, false);
      emitter_.emit(0xd51b'4408U); // msr fpcr, x8
      native_fp_ = true;
      return true;
    case 0xd53b'4420U:             // FPSR
      write_destination([&](unsigned destination) {
        emitter_.emit(0xd53b'4420U | destination);
        mask_register(destination, native_fpsr_mutable_mask, false);
        emitter_.load_w(10, 15, offsetof(CpuState, fpsr));
        mask_register(10, 0xf000'0000U, false);
        emitter_.emit(0x2a00'0000U | (10U << 16) |
                      (destination << 5) | destination);
      });
      native_fp_ = true;
      return true;
    case 0xd51b'4420U:
      copy_source_to_scratch();
      mask_register(8, fpsr_mask, false);
      emitter_.store_w(8, 15, offsetof(CpuState, fpsr));
      emitter_.emit(0xd51b'4428U); // msr fpsr, x8
      native_fp_ = true;
      return true;
    case 0xd53b'4440U: // FPMR
      write_destination([&](unsigned destination) {
        emitter_.load_x(destination, 15, offsetof(CpuState, fpmr));
      });
      return true;
    case 0xd51b'4440U:
      copy_source_to_scratch();
      mask_register(8, fpmr_mask, true);
      emitter_.store_x(8, 15, offsetof(CpuState, fpmr));
      if (translation_supports(ArmFeature::fp8)) {
        emitter_.emit(0xd51b'4448U); // msr FPMR, x8
        native_fpmr_ = true;
      }
      return true;
    case 0xd53b'4200U: // NZCV
      write_destination([&](unsigned destination) {
        read_flags(destination);
      });
      return true;
    case 0xd51b'4200U:
      copy_source_to_scratch();
      mask_register(8, 0xf000'0000U, false);
      emitter_.store_w(8, 15, offsetof(CpuState, nzcv));
      /* The guest write supersedes any formerly resident host value. */
      assume_flags_in_memory();
      return true;
    /* DIT is the register view of PSTATE.DIT that the MSR immediate form
     * writes: read is Zeros(39):PSTATE.DIT:Zeros(24), write takes X[t]<24>.
     * Both directions are gated on the same feature as that immediate form so
     * a guest can always read back what it just set under this engine. */
    case 0xd53b'42a0U: // DIT
      if (!translation_supports(ArmFeature::dit))
        return false;
      write_destination([&](unsigned destination) {
        emitter_.load_b(destination, 15, offsetof(CpuState, pstate_dit));
        emitter_.ubfm(destination, destination, 40, 39); // lsl #24
      });
      return true;
    case 0xd51b'42a0U:
      if (!translation_supports(ArmFeature::dit))
        return false;
      copy_source_to_scratch();
      emitter_.ubfm(8, 8, 24, 24); // ubfx #24, #1
      emitter_.store_b(8, 15, offsetof(CpuState, pstate_dit));
      return true;
    case 0xd53b'e000U: // CNTFRQ_EL0
      write_destination([&](unsigned destination) {
        emitter_.emit(0xd53b'e000U | destination);
      });
      return true;
    case 0xd53b'e040U: // CNTVCT_EL0
      write_destination([&](unsigned destination) {
        emitter_.emit(0xd53b'e040U | destination);
      });
      return true;
    /* FEAT_ECV's self-synchronized view of the same count, for which a
     * guest need not order earlier loads with an ISB. Only a host that
     * implements it can execute the register. CNTPCTSS_EL0 stays UNDEFINED
     * with CNTPCT_EL0: Linux gives EL0 no physical count. */
    case 0xd53b'e0c0U: // CNTVCTSS_EL0
      if (!translation_supports(ArmFeature::ecv))
        return false;
      write_destination([&](unsigned destination) {
        emitter_.emit(0xd53b'e0c0U | destination);
      });
      return true;
    default:
      break;
    }
    /* Behind the named EL0 registers so ordinary translation still leaves this
     * function through the same switch. An emulated identification register
     * holds one value for the whole process, so the read compiles to the
     * constant itself rather than a call back into the engine. */
    if (!detail::emulated_id_register(instruction))
      return false;
    write_destination([&](unsigned destination) {
      emitter_.load_constant(
          destination, detail::emulated_id_register_value(instruction));
    });
    return true;
  }

void JitCompiler::emit_cache_zero(std::uint32_t instruction, std::uint64_t pc) {
    mark_direct_memory_write();
    const unsigned source = instruction & 31U;
    load_gpr(8, source, false);
    if (source != 31U) {
      const auto no_tagged_state = emitter_.make_label();
      emitter_.load_b(0, 15, offsetof(CpuState, tbi_enabled));
      emitter_.compare_zero(no_tagged_state, 0, false, false);
      emitter_.ubfm(8, 8, 0, 55);
      emitter_.bind(no_tagged_state);
    }
    mask_register(8, ~std::uint64_t{63}, true);
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    /* The advertised 64-byte block size divides every supported guest
     * page, so an aligned DC ZVA block needs exactly one translation.
     */
    emit_translate(8, Access::write, fault, fault, 1);
    track_direct_store(8, 64);
    for (unsigned offset = 0; offset < 64; offset += 16)
      emitter_.emit(0xa900'0000U | ((offset / 8U) << 15) | (31U << 10) |
                    (8U << 5) | 31U);
    clear_local_exclusive_monitor();
  }

void JitCompiler::emit_code_synchronize(std::uint32_t instruction, std::uint64_t pc,
                             std::size_t count) {
    /*
     * This instruction is an architectural translated-code boundary. Publish
     * the complete native register cache before the cold helper, then return
     * to C++ at the known fallthrough so the changed code sequence is
     * observed before another translated instruction executes.
     */
    store_pc(pc + sizeof(std::uint32_t));
    spill_gprs_for_terminal_call();
    const unsigned source = instruction & 31U;
    if (source == 31U)
      emitter_.move_x(0, 31);
    else {
      emitter_.load_x(0, 15, register_offset(source, false));
      const auto no_tagged_state = emitter_.make_label();
      emitter_.load_b(8, 15, offsetof(CpuState, tbi_enabled));
      emitter_.compare_zero(no_tagged_state, 8, false, false);
      emitter_.ubfm(0, 0, 0, 55);
      emitter_.bind(no_tagged_state);
    }
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_guest_code_synchronize));
    emitter_.return_constant(Event::continued, count);
  }

void JitCompiler::emit_pair_slow(std::uint32_t instruction, std::uint64_t pc,
                      std::size_t count) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.call(reinterpret_cast<const void *>(&uml_arm64_jit_execute_pair));
    emitter_.return_dynamic(count);
  }

void JitCompiler::emit_simd_structure_slow(std::uint32_t instruction, std::uint64_t pc,
                                std::size_t count) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_jit_execute_simd_structure));
    emitter_.return_dynamic(count);
  }

void JitCompiler::emit_alignment_fault(std::uint32_t instruction, std::uint64_t pc,
                            std::size_t count) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_jit_alignment_fault));
    emitter_.return_dynamic(count);
  }

void JitCompiler::emit_ordered_slow(std::uint32_t instruction, std::uint64_t pc,
                         std::size_t count) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_jit_execute_ordered));
    emitter_.return_dynamic(count);
  }

void JitCompiler::emit_exclusive_slow(std::uint32_t instruction, std::uint64_t pc,
                           std::size_t count) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_jit_execute_exclusive));
    emitter_.return_dynamic(count);
  }

void JitCompiler::emit_deferred_exclusive_slow(std::uint32_t instruction,
                                    std::uint64_t pc,
                                    std::size_t count) {
    store_pc(pc);
    emitter_.load_constant(0, instruction);
    emitter_.store_w(
        0, 19, offsetof(JitChainState, deferred_exclusive_instruction));
    /*
     * The instruction at pc has not executed. return_constant() flushes the
     * live guest prefix before unwinding the one outer generated frame.
     */
    emitter_.return_constant(Event::continued,
                             count == 0U ? 0U : count - 1U);
  }

void JitCompiler::emit_memory_slow(std::uint32_t instruction, std::uint64_t pc,
                        std::size_t count) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_jit_execute_memory));
    emitter_.return_dynamic(count);
  }

void JitCompiler::emit_memory_copy_set(std::uint32_t instruction,
                                       std::uint64_t pc, std::size_t count,
                                       unsigned stages) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.load_constant(3, stages);
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_jit_execute_mops));
    emitter_.return_dynamic(count);
  }

[[nodiscard]] std::size_t JitCompiler::try_emit_memory_copy_set_sequence(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    /*
     * A compiler emits each operation as its prologue, main and epilogue in
     * sequence. Run the three as one helper call, and so one block exit, when
     * the words agree in everything but their stage. Any other arrangement
     * executes stage by stage through emit_instruction().
     */
    if (instructions.size() < 3U)
      return 0;
    const auto prologue = instructions[0];
    const auto mops = arm64::detail::decode_mops(prologue);
    if (!mops.recognized || !mops.valid || mops.stage != 0U ||
        instructions[1] !=
            arm64::detail::mops_stage_word(prologue, mops.family, 1U) ||
        instructions[2] !=
            arm64::detail::mops_stage_word(prologue, mops.family, 2U))
      return 0;
    if (count_ + 2U < maximum_block_instructions)
      memory_write_mask_ |= std::uint64_t{1} << (count_ + 2U);
    emit_memory_copy_set(prologue, pc, count_ + 1U, 3U);
    return 3;
  }

void JitCompiler::emit_simd_modified_immediate(std::uint32_t instruction,
                                    bool reads_destination) {
    const unsigned destination = instruction & 31U;
    const auto native_destination =
        write_native_vector(destination, reads_destination, 0);
    emitter_.emit((instruction & ~31U) | native_destination.host);
    commit_native_vector(native_destination, destination);
  }

void JitCompiler::emit_simd_duplicate_general(std::uint32_t instruction) {
    const unsigned source = (instruction >> 5) & 31U;
    const unsigned destination = instruction & 31U;
    const auto native_source = read_native_gpr(source, false, 8);
    const auto native_destination =
        write_native_vector(destination, false, 0);
    emitter_.emit((instruction & ~((31U << 5) | 31U)) |
                  (native_source.host << 5) | native_destination.host);
    commit_native_vector(native_destination, destination);
  }

[[nodiscard]] bool JitCompiler::emit_simd_insert(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd)
      return false;
    const bool from_general = (instruction & 0xffe0'fc00U) == 0x4e00'1c00U;
    const bool from_element = (instruction & 0xffe0'8400U) == 0x6e00'0400U;
    if (!from_general && !from_element)
      return false;
    if (!arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U))
      return false;
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    unsigned native_source{};
    if (from_general)
      native_source = read_native_gpr(source, false, 8).host;
    else
      native_source = read_native_vector(source, 1).host;
    const auto native_destination =
        write_native_vector(destination, true, 0);
    emitter_.emit((instruction & ~0x3ffU) | (native_source << 5) |
                  native_destination.host);
    commit_native_vector(native_destination, destination);
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_duplicate_element(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd)
      return false;
    const bool vector = (instruction & 0xbfe0'fc00U) == 0x0e00'0400U;
    const bool scalar = (instruction & 0xffe0'fc00U) == 0x5e00'0400U;
    if (!vector && !scalar)
      return false;
    const auto lane =
        arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U);
    const bool q = (instruction & 0x4000'0000U) != 0;
    if (!lane || (vector && !q && lane->element_shift == 3U))
      return false;
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    const auto native_source = read_native_vector(source, 1);
    const auto native_destination =
        write_native_vector(destination, false, 0);
    emitter_.emit((instruction & ~0x3ffU) | (native_source.host << 5) |
                  native_destination.host);
    commit_native_vector(native_destination, destination);
    return true;
  }

void JitCompiler::emit_simd_unary_native(std::uint32_t instruction) {
    const unsigned source = (instruction >> 5) & 31U;
    const unsigned destination = instruction & 31U;
    const auto native_source = read_native_vector(source, 1);
    const auto native_destination =
        write_native_vector(destination, false, 0);
    emitter_.emit((instruction & ~((31U << 5) | 31U)) |
                  (native_source.host << 5) | native_destination.host);
    commit_native_vector(native_destination, destination);
  }

[[nodiscard]] bool JitCompiler::emit_simd_bit_unary(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd)
      return false;
    const auto operation = instruction & 0x9f3f'fc00U;
    if (operation != 0x0e20'5800U && operation != 0x0e20'0800U &&
        operation != 0x0e20'1800U)
      return false;
    const unsigned opcode = (instruction >> 10) & 63U;
    const bool alternate = (instruction & 0x2000'0000U) != 0;
    const unsigned size = (instruction >> 22) & 3U;
    const bool valid =
        opcode == 22U
            ? ((!alternate && size == 0U) || (alternate && size <= 1U))
        : opcode == 2U ? ((!alternate && size < 3U) || (alternate && size < 2U))
                       : opcode == 6U && !alternate && size == 0U;
    if (!valid)
      return false;
    emit_simd_unary_native(instruction);
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_integer_reduce(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd ||
        (instruction & 0x9f3e'0c00U) != 0x0e30'0800U)
      return false;
    const unsigned operation = (instruction >> 12) & 31U;
    const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
    if (operation != 3U && operation != 10U && operation != 26U &&
        (operation != 27U || unsigned_group))
      return false;
    const bool q = (instruction & 0x4000'0000U) != 0;
    const unsigned size = (instruction >> 22) & 3U;
    if (size > 2U || (!q && size == 2U))
      return false;
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    const auto native_source = read_native_vector(source, 1);
    const auto native_destination =
        write_native_vector(destination, false, 0);
    emitter_.emit((instruction & ~0x3ffU) | (native_source.host << 5) |
                  native_destination.host);
    commit_native_vector(native_destination, destination);
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_scalar_pairwise_add(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd ||
        (instruction & 0xffff'fc00U) != 0x5ef1'b800U)
      return false;
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    const auto native_source = read_native_vector(source, 1);
    const auto native_destination =
        write_native_vector(destination, false, 0);
    emitter_.emit((instruction & ~0x3ffU) | (native_source.host << 5) |
                  native_destination.host);
    commit_native_vector(native_destination, destination);
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_shift_immediate(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd)
      return false;
    const bool vector = (instruction & 0x9f00'0400U) == 0x0f00'0400U;
    const bool scalar = (instruction & 0xdf00'0400U) == 0x5f00'0400U;
    if (!vector && !scalar)
      return false;
    const unsigned opcode = (instruction >> 11) & 31U;
    const bool alternate = (instruction & 0x2000'0000U) != 0;
    if (opcode != 0U && opcode != 2U && opcode != 4U && opcode != 6U &&
        opcode != 8U && opcode != 10U && opcode != 12U && opcode != 14U &&
        opcode != 16U && opcode != 17U && opcode != 18U && opcode != 19U)
      return false;
    if ((opcode == 8U || opcode == 12U) && !alternate)
      return false;
    const unsigned encoded = (instruction >> 16) & 0x7fU;
    if (encoded < 8U)
      return false;
    const unsigned lane_bits = std::bit_floor(encoded);
    if (lane_bits > 64U)
      return false;
    const bool q = (instruction & 0x4000'0000U) != 0;
    const bool narrow = opcode >= 16U;
    const bool saturating_left =
        opcode == 12U || opcode == 14U;
    if (narrow) {
      if (encoded >= 64U || (scalar && opcode <= 17U && !alternate))
        return false;
    } else {
      if (scalar && !saturating_left && lane_bits != 64U)
        return false;
      const unsigned vector_bits = scalar ? lane_bits : q ? 128U : 64U;
      if (lane_bits > vector_bits || (!scalar && lane_bits == 64U && !q))
        return false;
      const bool shift_left =
          opcode == 10U || opcode == 12U || opcode == 14U;
      const unsigned shift =
          shift_left ? encoded - lane_bits : 2U * lane_bits - encoded;
      if ((shift_left && shift >= lane_bits) ||
          (!shift_left && (shift == 0U || shift > lane_bits)))
        return false;
    }
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    const bool reads_destination =
        opcode == 2U || opcode == 6U || opcode == 8U ||
        (opcode == 10U && alternate) || (narrow && !scalar && q);
    const auto native_source = read_native_vector(source, 1);
    const auto native_destination =
        write_native_vector(destination, reads_destination, 0);
    emitter_.emit((instruction & ~0x3ffU) | (native_source.host << 5) |
                  native_destination.host);
    commit_native_vector(native_destination, destination);
    const bool saturating =
        saturating_left || opcode >= 18U ||
        ((opcode == 16U || opcode == 17U) && alternate);
    if (saturating)
      native_fp_ = true;
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_shift_left_long(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd ||
        (instruction & 0x9f80'fc00U) != 0x0f00'a400U)
      return false;
    const unsigned encoded = (instruction >> 16) & 0x7fU;
    const unsigned source_bits = std::bit_floor(encoded);
    if (source_bits < 8U || source_bits > 32U ||
        encoded - source_bits >= source_bits)
      return false;
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    const auto native_source = read_native_vector(source, 1);
    const auto native_destination =
        write_native_vector(destination, false, 0);
    emitter_.emit((instruction & ~0x3ffU) | (native_source.host << 5) |
                  native_destination.host);
    commit_native_vector(native_destination, destination);
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_general_move(std::uint32_t instruction) {
    const auto opcode = instruction & 0xffff'fc00U;
    bool vector_to_gpr = (instruction & 0xbfe0'fc00U) == 0x0e00'3c00U ||
                         (instruction & 0xbfe0'fc00U) == 0x0e00'2c00U;
    if (vector_to_gpr) {
      const bool signed_move = (instruction & 0x1000U) == 0;
      const auto lane =
          arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U);
      const bool wide = (instruction & 0x4000'0000U) != 0;
      if (!lane || (signed_move ? lane->element_shift >= (wide ? 3U : 2U)
                                : (wide ? lane->element_shift != 3U
                                        : lane->element_shift >= 3U)))
        return false;
    } else {
      switch (opcode) {
      case 0x1ee6'0000U:
      case 0x9ee6'0000U:
      case 0x9e66'0000U:
      case 0x1e26'0000U:
      case 0x9eae'0000U:
        vector_to_gpr = true;
        break;
      case 0x9e67'0000U:
      case 0x1ee7'0000U:
      case 0x9ee7'0000U:
      case 0x1e27'0000U:
      case 0x9eaf'0000U:
        break;
      default:
        return false;
      }
      if ((opcode == 0x1ee6'0000U || opcode == 0x9ee6'0000U ||
           opcode == 0x1ee7'0000U || opcode == 0x9ee7'0000U) &&
          !host_capabilities().fp16)
        return false;
    }
    const unsigned source = (instruction >> 5) & 31U;
    const unsigned destination = instruction & 31U;
    auto native = instruction & ~((31U << 5) | 31U);
    if (vector_to_gpr) {
      const auto native_source = read_native_vector(source, 0);
      const auto native_destination =
          write_native_gpr(destination, false, false, 8);
      native |= (native_source.host << 5) | native_destination.host;
      emitter_.emit(native);
      commit_native_gpr(native_destination, destination, false);
    } else {
      const auto native_source = read_native_gpr(source, false, 8);
      const bool reads_destination = opcode == 0x9eaf'0000U;
      const auto native_destination =
          write_native_vector(destination, reads_destination, 0);
      native |= (native_source.host << 5) | native_destination.host;
      emitter_.emit(native);
      commit_native_vector(native_destination, destination);
    }
    return true;
  }

} // namespace uml::cpu::arm64
