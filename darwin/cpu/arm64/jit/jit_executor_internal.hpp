// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_cache.hpp"

#ifndef UML_JIT_RUNTIME_DIAGNOSTICS
#define UML_JIT_RUNTIME_DIAGNOSTICS 0
#endif

namespace uml::cpu::arm64 {

class JitExecutor::Implementation final {
public:
  explicit Implementation(
      JitExecutorOptions options
#if UML_JIT_RUNTIME_DIAGNOSTICS
      , JitExecutorDiagnosticsOptions diagnostics
#endif
  );
  ~Implementation();

  [[nodiscard]] bool begin_execution(unsigned long generation,
                                     std::uint64_t guest_pc);
  void end_execution() noexcept;
  [[nodiscard]] Event run_block(CpuState &cpu, StaticGuestMemory &memory,
                                unsigned long generation,
                                std::size_t &executed);
  [[nodiscard]] bool preempted() const noexcept;
  [[nodiscard]] std::uint64_t fault_address() const noexcept;
  [[nodiscard]] Access fault_access() const noexcept;
  [[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept;
#if UML_JIT_RUNTIME_DIAGNOSTICS
  [[nodiscard]] JitCacheValidationDiagnostics
  cache_validation_diagnostics() const noexcept;
  [[nodiscard]] JitIndirectLinkDiagnostics
  indirect_link_diagnostics() const noexcept;
#endif
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  [[nodiscard]] JitGprLinkProfileDiagnostics
  gpr_link_profile_diagnostics() noexcept;
#endif

private:
  struct PendingIndirect final {
    std::uint64_t source_pc{};
    std::uint64_t target_pc{};
    CpuState *cpu{};
    unsigned long generation{};
    std::uint64_t code_changes{};
    std::uint64_t link_epoch{};
    bool valid{};
  };

  [[nodiscard]] JitFunction
  tracked_store_function(CompiledBlock &block, StaticGuestMemory &memory);
  void clear_tracked_store_cache() noexcept;
  [[nodiscard]] Event run_floating_exception_mode(
      CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed);
  void select_cpu(CpuState &cpu) noexcept;
  void release_current_reservation() noexcept;
  void reconcile_reservation(CpuState &cpu) noexcept;
  [[nodiscard]] Event run_semantic_instruction(
      CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed);
  [[nodiscard]] Event run_deferred_exclusive_instruction(
      CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed);

  [[nodiscard]] void *execution_context(unsigned long generation);
  [[nodiscard]] static const void *
  resolve_block_link(void *context, std::uint64_t pc);
  [[nodiscard]] static const void *
  resolve_indirect_link(void *context, std::uint64_t source_pc);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  [[nodiscard]] static std::uint64_t *
  resolve_direct_link_profile(void *context, const void *target,
                              std::uint64_t source_pc,
                              std::uint32_t dirty_live_out);
  [[nodiscard]] static jit::JitIndirectLinkProfileCounters
  resolve_indirect_link_profile(void *context, const void *site,
                                std::uint64_t source_pc,
                                std::uint32_t dirty_live_out);
#endif
  [[nodiscard]] JitBlockLink *link_for(std::uint64_t pc);
  [[nodiscard]] JitIndirectLink *
  indirect_link_for(std::uint64_t source_pc);
  void record_pending_indirect(void *pointer, std::uint64_t source_pc,
                               CpuState &cpu, unsigned long generation,
                               std::uint64_t changes) noexcept;
  [[nodiscard]] std::optional<std::uint64_t>
  consume_pending_indirect(CpuState &cpu, unsigned long generation,
                           std::uint64_t changes,
                           std::uint64_t target_pc) noexcept;
  void train_indirect(std::uint64_t source_pc,
                      const CompiledBlock &target);
  void deactivate_links() noexcept;
  void deactivate_indirect_links() noexcept;
  void prepare_links(unsigned long generation, std::uint64_t changes);
  [[gnu::noinline]] void refresh_link(CompiledBlock &block);
  [[gnu::always_inline]] void activate_link(CompiledBlock &block) {
    if (block.link_epoch != link_epoch_)
      refresh_link(block);
  }

  [[nodiscard]] JitCacheValidationResult validate_stale_block(
      const CompiledBlock &block, const SavedGuestWords &guest_words,
      std::uint64_t changes, StaticGuestMemory &memory);
  [[nodiscard]] CompiledBlock *find(unsigned long generation,
                                    std::uint64_t changes, std::uint64_t pc,
                                    StaticGuestMemory &memory);
  [[nodiscard]] CacheSlot replacement(unsigned long generation,
                                      std::uint64_t changes,
                                      std::uint64_t pc);
  CompiledBlock &store_native(unsigned long generation, std::uint64_t changes,
                              std::uint64_t pc, JitFunction function,
                              std::uintptr_t internal_entry,
                              std::span<const std::uint32_t> guest_words,
                              std::uint64_t memory_write_mask,
                              std::uint64_t direct_memory_write_mask,
                              bool accesses_fp_state, bool link_target_safe,
                              bool native_fp, bool native_fpmr,
                              bool guest_illegal
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
                              ,
                              std::uint32_t profile_read_before_write
#endif
  );
  void clear_cache() noexcept;

  std::unique_ptr<BlockCache> block_cache_{std::make_unique<BlockCache>()};
  std::unique_ptr<SavedGuestWordCache> saved_guest_words_{
      std::make_unique_for_overwrite<SavedGuestWordCache>()};
  JitBlockLink empty_link_;
  JitBlockLinkTable links_;
  JitIndirectLinkTable indirect_links_;
  std::vector<JitBlockLink *> active_links_;
  std::vector<JitIndirectLink *> active_indirect_links_;
  jit::detail::ExecutableArena arena_{arena_size};
  jit::detail::ExecutableArena tracked_store_arena_{tracked_store_arena_size};
  jit::detail::ExecutableArena floating_transaction_arena_{
      floating_transaction_arena_size};
  Tlb tlb_;
  void *execution_context_{};
  unsigned long execution_generation_{};
  unsigned long mapping_generation_{};
  std::uint64_t mapping_changes_{};
  unsigned long link_generation_{};
  std::uint64_t link_code_changes_{};
  std::uint64_t link_epoch_{1};
  std::uint64_t compiled_in_link_sequence_{};
  std::uint64_t stamp_{};
  std::array<JitStoreRange, maximum_block_instructions>
      tracked_store_ranges_{};
  JitStoreParticipant store_participant_;
  CpuState *current_cpu_{};
  std::uint32_t floating_point_syndrome_{};
#if UML_JIT_RUNTIME_DIAGNOSTICS
  JitCacheValidationDiagnostics cache_validation_diagnostics_{};
  JitIndirectLinkDiagnostics indirect_link_diagnostics_{};
#endif
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  JitGprLinkProfiler gpr_link_profile_{};
#endif
  PendingIndirect pending_indirect_{};
  bool preempted_{};
  bool enable_guarded_control_flow_{};
  bool enable_pointer_authentication_{true};
#if UML_JIT_RUNTIME_DIAGNOSTICS
  bool reuse_validated_stale_blocks_{};
#endif
  bool link_sequence_initialized_{};
  ::uml::cpu::HostCapabilities host_capabilities_;
};

} // namespace uml::cpu::arm64
