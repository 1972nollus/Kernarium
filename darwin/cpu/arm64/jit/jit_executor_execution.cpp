// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_executor_internal.hpp"

#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/instructions/load_store_memory.hpp"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstring>
#include <optional>
#include <span>

namespace uml::cpu::arm64 {

[[nodiscard]] Event JitExecutor::Implementation::run_block(CpuState &cpu, StaticGuestMemory &memory,
                              unsigned long generation,
                              std::size_t &executed) {
  if (!cache_lease_.admitted()) {
    executed = 0;
    return Event::fatal;
  }
  const ::uml::cpu::HostCapabilitiesScope capabilities_scope{
      host_capabilities_};
  executed = 0;
  floating_point_syndrome_ = 0;
  select_cpu(cpu);
  const auto sequences =
      darwin_uml_static_vm_execution_context_sequences(execution_context_);
  preempted_ = sequences.execution_interrupt != 0;
  if (preempted_)
    return Event::interrupt;
  const bool mapping_changed = mapping_generation_ != generation ||
                               mapping_changes_ != sequences.mapping_changes;
  /* StaticGuestMemory belongs to the task-carrier thread, while a pooled JIT
   * executor may serve a different carrier on its next guest entry. Prepare
   * each distinct per-entry memory object even when the executor's global
   * generation/change view is already current. */
  if (prepared_memory_ != &memory || mapping_changed) {
    memory.prepare(generation, sequences.mapping_changes);
    prepared_memory_ = &memory;
  }
  if (mapping_changed) {
    tlb_.prepare(generation, sequences.mapping_changes);
    mapping_generation_ = generation;
    mapping_changes_ = sequences.mapping_changes;
  }
  prepare_links(generation, sequences.code_changes);
  const auto block_pc = cpu.pc;
  const auto pending_source = consume_pending_indirect(
      cpu, generation, sequences.code_changes, block_pc);

  /*
   * Enabled guest exception traps are rare.  Keep them completely outside
   * the cached multi-instruction hot path so no translated block can run
   * past a potentially trapping instruction.  This cold path still lowers
   * and executes the exact native JIT form; it merely limits the boundary
   * to one guest instruction so a register-only FP/SIMD operation can be
   * committed transactionally.
   */
  const auto guest_fpcr = cpu.fp_loaded ? cpu.fpcr : cpu.deferred_fpcr;
  if (arm64::detail::enabled_exception_flags(guest_fpcr) != 0U) {
#if UML_JIT_RUNTIME_DIAGNOSTICS
    indirect_link_diagnostics_.unsafe_target_refusals +=
        pending_source.has_value();
#endif
    return run_floating_exception_mode(cpu, memory, executed);
  }

  auto *block =
      find(generation, sequences.code_changes, cpu.pc, memory);
  if (block == nullptr) {
    std::array<std::uint32_t, maximum_block_instructions> instructions{};
    std::size_t count = 0;
    while (count < instructions.size()) {
      std::array<std::byte, sizeof(std::uint32_t)> encoded{};
      if (!memory.fetch(cpu.pc + count * 4, encoded))
        break;
      std::memcpy(&instructions[count], encoded.data(), encoded.size());
      const auto terminates =
          obvious_translated_block_terminator(instructions[count]);
      ++count;
      if (terminates) {
        // A compare/exchange retry has a conditional exit before its store.
        // Look ahead only far enough to recognize that bounded native region.
        // A speculative fetch failure leaves the ordinary prefix intact.
        if (count == 3U &&
            (instructions[0] & 0x3fe0'0000U) == 0x0840'0000U &&
            (instructions[1] & 0x7fe0'fc1fU) == 0x6b00'001fU &&
            (instructions[2] & 0xff00'001fU) == 0x5400'0001U) {
          for (; count < 5U; ++count) {
            if (!memory.fetch(cpu.pc + count * 4U, encoded))
              break;
            std::memcpy(&instructions[count], encoded.data(), encoded.size());
          }
        }
        break;
      }
    }
    if (count == 0) {
      const auto fault = memory.last_fault();
      tlb_.fault_address = fault.address;
      tlb_.fault_access = ::uml::memory::translated_access(fault.access);
      return Event::memory_fault;
    }
    /*
     * Another executor may already have decoded and emitted exactly these
     * guest words at exactly this PC. Reuse that work by copying its
     * translation and rewriting the link pointers for this executor's own
     * tables; the host code, arena and link records all stay private.
     */
    const auto fetched = std::span{instructions}.first(count);
    auto *shared = install_shared_translation(
        generation, sequences.code_changes, cpu.pc, fetched);
    if (shared != nullptr) {
      block = shared;
    } else {
      JitCompiler compiler{
          enable_guarded_control_flow_, this,
          &Implementation::resolve_block_link, false,
          &Implementation::resolve_indirect_link
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
          ,
          /*persistent_linked_block_abi=*/
          persistent_linked_block_abi_default,
          this, &Implementation::resolve_direct_link_profile,
          &Implementation::resolve_indirect_link_profile
#endif
      };
      // Compile once on a miss. Speculatively lowering the whole block a
      // second time for GPR residency delays cold multi-process workloads.
      compiler.set_gpr_residency_planning_enabled(false);
      compiler.set_pointer_authentication_enabled(
          enable_pointer_authentication_);
#if UML_ENABLE_JIT_PROFILE
      const auto local_compile_path_started = jit_profile_timestamp_ns();
#endif
      const auto compiled = compiler.compile(cpu.pc, fetched);
#if UML_JIT_RUNTIME_DIAGNOSTICS
      cache_validation_diagnostics_.direct_link_refusals +=
          compiled.direct_link_refusals;
      cache_validation_diagnostics_.indirect_link_refusals +=
          compiled.indirect_link_refusals;
#endif
      {
        const auto valid_compilation = [&](const auto &candidate) {
          return !candidate.words.empty() && candidate.guest_instructions != 0 &&
                 candidate.guest_instructions <= instructions.size() &&
                 valid_link_state_metadata(candidate.accesses_fp_state,
                                           candidate.native_fp,
                                           candidate.native_fpmr);
        };
        const auto install_compilation =
            [&](const auto &candidate,
                const jit::detail::ExecutableArena::StoredBlock &stored,
                const std::shared_ptr<const SharedTranslation> &translation) {
          const auto internal_entry =
              reinterpret_cast<std::uintptr_t>(stored.code) +
              candidate.internal_entry_word * sizeof(std::uint32_t);
          auto saved_guest_words =
              translation == nullptr
                  ? std::shared_ptr<const std::uint32_t[]>{}
                  : translation->guest_words;
          return &store_native(
              generation, sequences.code_changes, cpu.pc,
              sign_jit_function<JitFunction>(stored.code), internal_entry,
              stored.target_link, candidate.relocations,
              stored.direct_link_refusals,
              std::span{instructions}.first(candidate.guest_instructions),
              std::move(saved_guest_words),
              candidate.memory_write_mask, candidate.direct_memory_write_mask,
              candidate.accesses_fp_state, candidate.link_target_safe,
              candidate.native_fp, candidate.native_fpmr,
              candidate.guest_illegal
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
              ,
              candidate.profile_read_before_write
#endif
          );
        };
        if (!valid_compilation(compiled))
          return run_semantic_instruction(cpu, memory, executed);
        auto stored = arena_.store_block(
            cpu.pc, compiled.words, compiled.relocations, {}, links_,
            empty_link_, generation);
        if (stored) {
          auto shared = publish_shared_translation(cpu.pc, fetched, compiled);
          block = install_compilation(compiled, stored, shared);
        } else {
          profile_jit_arena_reset();
#if UML_JIT_RUNTIME_DIAGNOSTICS
          ++cache_validation_diagnostics_.arena_resets;
#endif
          clear_cache();
          arena_.reset();

          /*
           * Indirect PIC operands are absolute pointers into the descriptor
           * table. clear_cache() invalidates that storage, so the retry must
           * be freshly lowered rather than copying its stale site pointer.
           * Direct descriptors are regenerated in the private link table.
           */
          JitCompiler retry_compiler{
              enable_guarded_control_flow_, this,
              &Implementation::resolve_block_link, false,
              &Implementation::resolve_indirect_link
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
              ,
              /*persistent_linked_block_abi=*/
              persistent_linked_block_abi_default,
              this, &Implementation::resolve_direct_link_profile,
              &Implementation::resolve_indirect_link_profile
#endif
          };
          retry_compiler.set_gpr_residency_planning_enabled(false);
          retry_compiler.set_pointer_authentication_enabled(
              enable_pointer_authentication_);
          const auto retry_compiled = retry_compiler.compile(
              cpu.pc, std::span{instructions}.first(count));
#if UML_JIT_RUNTIME_DIAGNOSTICS
          cache_validation_diagnostics_.direct_link_refusals +=
              retry_compiled.direct_link_refusals;
          cache_validation_diagnostics_.indirect_link_refusals +=
              retry_compiled.indirect_link_refusals;
#endif
          if (!valid_compilation(retry_compiled))
            return run_semantic_instruction(cpu, memory, executed);
          stored = arena_.store_block(
              cpu.pc, retry_compiled.words, retry_compiled.relocations, {},
              links_, empty_link_, generation);
          if (!stored)
            return run_semantic_instruction(cpu, memory, executed);
          auto shared =
              publish_shared_translation(cpu.pc, fetched, retry_compiled);
          block = install_compilation(retry_compiled, stored, shared);
        }
      }
#if UML_ENABLE_JIT_PROFILE
      /* Compare complete successful miss paths: compiler lowering, private
       * arena publication, optional canonical publication, and block install
       * versus shared lookup, relocation, private publication, and install.
       */
      profile_jit_local_compile_latency(
          jit_profile_timestamp_ns() - local_compile_path_started);
#endif
    }
  }
  if (pending_source)
    train_indirect(*pending_source, *block);
  bool deferred_semantic_instruction = false;
  const auto execute_native = [&](JitFunction function,
                                  bool tracked_stores) -> Event {
    if (tracked_stores)
      tracked_store_ranges_.fill({});
    std::uint8_t link_capabilities =
        block->direct_memory_write_mask != 0
            ? link_capability(JitLinkCapability::store_publication)
            : std::uint8_t{0};
    JitChainState chain{
        .stores = tracked_stores ? tracked_store_ranges_.data() : nullptr,
    };
    if (block->accesses_fp_state) {
      cpu.access_fp_state();
      link_capabilities |=
          link_capability(JitLinkCapability::fp_state_materialized);
      if (block->native_fp) {
        activate_native_fp_environment(cpu);
        link_capabilities |=
            link_capability(JitLinkCapability::native_fp_environment);
      }
    }
    if (block->native_fpmr) {
      activate_native_fpmr_environment(cpu);
      link_capabilities |=
          link_capability(JitLinkCapability::native_fpmr_environment);
    }
    chain.link_selector = checked_link_selector(link_capabilities);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    if (!tracked_stores)
      gpr_link_profile_.record_outer_invocation();
#endif
    const auto result = function(&cpu, &tlb_, &chain);
    if (block->native_fpmr)
      arm64::restore_native_fpmr_environment(cpu);
    if (tracked_stores)
      uml_arm64_jit_invalidate_store_ranges(
          reinterpret_cast<const std::uint64_t *>(
              tracked_store_ranges_.data()),
          block->instruction_count);
    const auto current_executed =
        static_cast<std::size_t>(result >> 32);
    const auto event =
        static_cast<Event>(static_cast<std::int32_t>(result & 0xffff'ffffU));
    deferred_semantic_instruction =
        chain.deferred_semantic_instruction != 0U;
    executed = maximum_chain_instructions -
                   static_cast<std::size_t>(chain.remaining_instructions) +
               current_executed;
    if (event == Event::memory_fault)
      tlb_.discard(tlb_.fault_address);
    if (event == Event::continued && chain.indirect_miss != nullptr)
      record_pending_indirect(chain.indirect_miss,
                              chain.indirect_source_pc, cpu, generation,
                              sequences.code_changes);
    if (!deferred_semantic_instruction)
      reconcile_reservation(cpu);
    return event;
  };

  if (block->direct_memory_write_mask == 0)
    return execute_native(block->function, false);
  Event event{};
  bool compile_fallback = false;
  {
    JitBlockPublication publication{store_participant_, cpu};
    if (!publication.needs_tracking()) {
      event = execute_native(block->function, false);
    } else {
      const auto tracked = tracked_store_function(*block, memory);
      if (tracked == nullptr)
        compile_fallback = true;
      else
        event = execute_native(tracked, true);
    }
  }
  if (deferred_semantic_instruction || compile_fallback) {
    const auto retired_prefix = executed;
    std::size_t semantic_executed = 0;
    if (deferred_semantic_instruction)
      event = run_deferred_exclusive_instruction(
          cpu, memory, semantic_executed);
    else
      event = run_semantic_instruction(cpu, memory, semantic_executed);
    executed = retired_prefix + semantic_executed;
  }
  reconcile_reservation(cpu);
  return event;
}

[[nodiscard]] JitFunction
JitExecutor::Implementation::tracked_store_function(CompiledBlock &block, StaticGuestMemory &memory) {
  if (block.tracked_function != nullptr)
    return block.tracked_function;

  std::array<std::uint32_t, maximum_block_instructions> instructions{};
  for (std::size_t index = 0; index < block.instruction_count; ++index) {
    if (!memory.fetch(
            block.pc + index * sizeof(std::uint32_t),
            std::as_writable_bytes(std::span{&instructions[index], 1})))
      return nullptr;
  }
  JitCompiler compiler{enable_guarded_control_flow_, nullptr, nullptr, true};
  compiler.set_gpr_residency_planning_enabled(false);
  compiler.set_pointer_authentication_enabled(
      enable_pointer_authentication_);
  const auto compiled = compiler.compile(
      block.pc, std::span{instructions}.first(block.instruction_count));
  if (compiled.words.empty() ||
      compiled.guest_instructions != block.instruction_count ||
      !valid_link_state_metadata(compiled.accesses_fp_state,
                                 compiled.native_fp,
                                 compiled.native_fpmr) ||
      compiled.direct_memory_write_mask != block.direct_memory_write_mask)
    return nullptr;

  auto *code = tracked_store_arena_.store(compiled.words);
  if (code == nullptr) {
    clear_tracked_store_cache();
    code = tracked_store_arena_.store(compiled.words);
    if (code == nullptr)
      return nullptr;
  }
  block.tracked_function = sign_jit_function<JitFunction>(code);
  return block.tracked_function;
}

void JitExecutor::Implementation::clear_tracked_store_cache() noexcept {
  for (auto &set : *block_cache_)
    for (auto &block : set.ways)
      block.tracked_function = nullptr;
  tracked_store_arena_.reset();
}

[[nodiscard]] Event JitExecutor::Implementation::run_floating_exception_mode(
    CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed) {
  std::uint32_t instruction{};
  if (!memory.fetch(
          cpu.pc,
          std::as_writable_bytes(std::span{&instruction, std::size_t{1}}))) {
    const auto fault = memory.last_fault();
    tlb_.fault_address = fault.address;
    tlb_.fault_access = ::uml::memory::translated_access(fault.access);
    return Event::memory_fault;
  }

  const auto guest_fpcr = cpu.fp_loaded ? cpu.fpcr : cpu.deferred_fpcr;
  const bool transactional =
      arm64::detail::needs_floating_exception_transaction(guest_fpcr,
                                                          instruction);
  if (transactional) {
    /*
     * A previous native block may have retained the guest environment in
     * host registers.  Materialize it before taking the isolated snapshot.
     */
    arm64::restore_native_fp_environment(cpu);
    cpu.access_fp_state();
  }

  JitCompiler compiler{enable_guarded_control_flow_};
  compiler.set_gpr_residency_planning_enabled(false);
  compiler.set_pointer_authentication_enabled(
      enable_pointer_authentication_);
  const auto compiled =
      compiler.compile(cpu.pc, std::span{&instruction, std::size_t{1}});
  if (compiled.words.empty() || compiled.guest_instructions != 1 ||
      !valid_link_state_metadata(compiled.accesses_fp_state,
                                 compiled.native_fp,
                                 compiled.native_fpmr))
    return run_semantic_instruction(cpu, memory, executed);
  floating_transaction_arena_.reset();
  auto *code = floating_transaction_arena_.store(compiled.words);
  if (code == nullptr)
    return run_semantic_instruction(cpu, memory, executed);
  const auto function = sign_jit_function<JitFunction>(code);

  const auto invoke = [&](CpuState &target,
                          bool restore_environment) -> Event {
    if (compiled.accesses_fp_state) {
      target.access_fp_state();
      if (compiled.native_fp)
        activate_native_fp_environment(target);
    }
    if (compiled.native_fpmr) {
      activate_native_fpmr_environment(target);
    }
    std::uint8_t link_capabilities =
        compiled.direct_memory_write_mask != 0
            ? link_capability(JitLinkCapability::store_publication)
            : std::uint8_t{0};
    JitChainState chain{};
    if (compiled.accesses_fp_state)
      link_capabilities |=
          link_capability(JitLinkCapability::fp_state_materialized);
    if (compiled.native_fp)
      link_capabilities |=
          link_capability(JitLinkCapability::native_fp_environment);
    if (compiled.native_fpmr)
      link_capabilities |=
          link_capability(JitLinkCapability::native_fpmr_environment);
    chain.link_selector = checked_link_selector(link_capabilities);
    const auto result = function(&target, &tlb_, &chain);
    if (compiled.native_fpmr)
      arm64::restore_native_fpmr_environment(target);
    if (restore_environment)
      arm64::restore_native_fp_environment(target);
    executed = static_cast<std::size_t>(result >> 32);
    const auto event =
        static_cast<Event>(static_cast<std::int32_t>(result & 0xffff'ffffU));
    if (event == Event::memory_fault)
      tlb_.discard(tlb_.fault_address);
    return event;
  };

  if (transactional) {
    if (compiled.memory_write_mask != 0)
      return run_semantic_instruction(cpu, memory, executed);
    const auto original_fpsr = cpu.fpsr;
    auto isolated = cpu;
    isolated.fpsr &=
        ~arm64::detail::fpsr_exception_cumulative_mask;
    isolated.gadget_host_fpcr = 0;
    isolated.gadget_host_fpsr = 0;
    isolated.gadget_fp_active = false;
    isolated.gadget_indirect_miss = nullptr;
    isolated.gadget_host_fpmr = 0;
    isolated.gadget_fpmr_active = false;
    const auto event = invoke(isolated, true);
    if (event != Event::continued)
      return event;
    const auto decision = arm64::detail::decide_floating_exception(
        cpu.fpcr, original_fpsr, isolated.fpsr);
    if (decision.traps()) {
      cpu.fpsr = decision.committed_fpsr;
      floating_point_syndrome_ = decision.syndrome;
      executed = 0;
      return Event::floating_point_trap;
    }
    isolated.fpsr =
        (isolated.fpsr &
         ~arm64::detail::fpsr_exception_cumulative_mask) |
        (decision.committed_fpsr &
         arm64::detail::fpsr_exception_cumulative_mask);
    cpu = isolated;
    reconcile_reservation(cpu);
    return event;
  }

  const auto execute_native = [&] { return invoke(cpu, false); };
  Event event{};
  if (compiled.direct_memory_write_mask == 0) {
    event = execute_native();
  } else {
    JitBlockPublication publication{store_participant_, cpu};
    if (publication.needs_tracking())
      event = run_semantic_instruction(cpu, memory, executed);
    else
      event = execute_native();
  }
  reconcile_reservation(cpu);
  return event;
}

void JitExecutor::Implementation::select_cpu(CpuState &cpu) noexcept {
  if (current_cpu_ == &cpu)
    return;
  pending_indirect_ = {};
  if (current_cpu_ != nullptr)
    arm64::restore_native_fp_environment(*current_cpu_);
  release_current_reservation();
  current_cpu_ = &cpu;
}

void JitExecutor::Implementation::release_current_reservation() noexcept {
  if (current_cpu_ == nullptr ||
      store_participant_.reservation_cpu.load(
          std::memory_order_seq_cst) != current_cpu_)
    return;
  store_participant_.reservation_cpu.store(nullptr,
                                           std::memory_order_seq_cst);
  end_jit_reservation(*current_cpu_);
}

void JitExecutor::Implementation::reconcile_reservation(CpuState &cpu) noexcept {
  if (cpu.exclusive_size != 0) {
    store_participant_.reservation_cpu.store(
        &cpu, std::memory_order_seq_cst);
    return;
  }
  if (store_participant_.reservation_cpu.load(
          std::memory_order_seq_cst) != &cpu)
    return;
  store_participant_.reservation_cpu.store(nullptr,
                                           std::memory_order_seq_cst);
  end_jit_reservation(cpu);
}

[[nodiscard]] Event JitExecutor::Implementation::run_semantic_instruction(
    CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed) {
  std::uint32_t instruction{};
  if (!memory.fetch(
          cpu.pc,
          std::as_writable_bytes(std::span{&instruction, std::size_t{1}}))) {
    const auto fault = memory.last_fault();
    tlb_.fault_address = fault.address;
    tlb_.fault_access = ::uml::memory::translated_access(fault.access);
    return Event::memory_fault;
  }
  std::uint32_t syndrome = 0;
  const auto result = uml_arm64_jit_execute_memory_with_syndrome(
      &cpu, &tlb_, instruction, &syndrome);
  executed = static_cast<std::size_t>(result >> 32U);
  const auto event =
      static_cast<Event>(static_cast<std::int32_t>(result & 0xffff'ffffU));
  if (event == Event::floating_point_trap)
    floating_point_syndrome_ = syndrome;
  if (event == Event::memory_fault)
    tlb_.discard(tlb_.fault_address);
  return event;
}

[[nodiscard]] Event JitExecutor::Implementation::run_deferred_exclusive_instruction(
    CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed) {
  std::uint32_t instruction{};
  if (!memory.fetch(
          cpu.pc,
          std::as_writable_bytes(std::span{&instruction, std::size_t{1}}))) {
    const auto fault = memory.last_fault();
    tlb_.fault_address = fault.address;
    tlb_.fault_access = ::uml::memory::translated_access(fault.access);
    executed = 0;
    return Event::memory_fault;
  }
  const auto result =
      uml_arm64_jit_execute_exclusive(&cpu, &tlb_, instruction);
  executed = static_cast<std::size_t>(result >> 32U);
  const auto event =
      static_cast<Event>(static_cast<std::int32_t>(result & 0xffff'ffffU));
  if (event == Event::memory_fault)
    tlb_.discard(tlb_.fault_address);
  return event;
}

void JitExecutor::Implementation::record_pending_indirect(void *pointer, std::uint64_t source_pc,
                             CpuState &cpu,
                             unsigned long generation,
                             std::uint64_t changes) noexcept {
  /*
   * Resolve the translation-owned source address before touching the
   * generated-code pointer. A miss can therefore never turn a corrupted or
   * stale pointer into a host dereference.
   */
  if ((cpu.pc & (alignof(std::uint32_t) - 1U)) != 0U) {
    pending_indirect_ = {};
    return;
  }
  auto *site = indirect_links_.find(source_pc, generation);
  if (site == nullptr || static_cast<void *>(site) != pointer) {
    pending_indirect_ = {};
    return;
  }
  pending_indirect_ = {
      .source_pc = source_pc,
      .target_pc = cpu.pc,
      .cpu = &cpu,
      .generation = generation,
      .code_changes = changes,
      .link_epoch = link_epoch_,
      .valid = true,
  };
#if UML_JIT_RUNTIME_DIAGNOSTICS
  ++indirect_link_diagnostics_.misses;
#endif
}

[[nodiscard]] std::optional<std::uint64_t>
JitExecutor::Implementation::consume_pending_indirect(CpuState &cpu, unsigned long generation,
                         std::uint64_t changes,
                         std::uint64_t target_pc) noexcept {
  const auto pending = pending_indirect_;
  pending_indirect_ = {};
  if (!pending.valid || pending.cpu != &cpu ||
      pending.generation != generation ||
      pending.code_changes != changes ||
      pending.link_epoch != link_epoch_ ||
      pending.target_pc != target_pc) {
#if UML_JIT_RUNTIME_DIAGNOSTICS
    indirect_link_diagnostics_.scope_rejections += pending.valid;
#endif
    return std::nullopt;
  }
  return pending.source_pc;
}

void JitExecutor::Implementation::prepare_links(unsigned long generation, std::uint64_t changes) {
  if (link_generation_ == generation && link_code_changes_ == changes &&
      link_context_ != nullptr)
    return;
  if (link_sequence_initialized_)
    profile_jit_sequence_transition(link_generation_ != generation,
                                    compiled_in_link_sequence_);
  else
    link_sequence_initialized_ = true;
  compiled_in_link_sequence_ = 0;
  pending_indirect_ = {};

  auto *selected = &link_contexts_.front();
  for (auto &context : link_contexts_) {
    if (context.epoch != 0 && context.generation == generation) {
      selected = &context;
      break;
    }
    if (context.stamp < selected->stamp)
      selected = &context;
  }
  link_context_ = selected;
  if (selected->epoch == 0 || selected->generation != generation ||
      selected->code_changes != changes) {
    deactivate_indirect_links();
    deactivate_links();
    *selected = {.generation = generation,
                 .code_changes = changes,
                 .epoch = ++next_link_epoch_};
  }
  selected->stamp = ++link_context_stamp_;
  link_epoch_ = selected->epoch;
  link_generation_ = generation;
  link_code_changes_ = changes;
}

} // namespace uml::cpu::arm64
