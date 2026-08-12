// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_arena.hpp"
#include "jit_cache.hpp"
#include "jit_cache_geometry.hpp"
#include "jit_executor.hpp"
#include "jit_publication.hpp"
#include "jit_runtime_contract.hpp"
#include "jit_shared_translations.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#ifndef UML_JIT_RUNTIME_DIAGNOSTICS
#define UML_JIT_RUNTIME_DIAGNOSTICS 0
#endif

namespace uml::cpu::arm64 {

using ::uml::memory::Access;
using ::uml::memory::StaticGuestMemory;
using ::uml::memory::Tlb;

template <typename Pointer>
class FixedPointerList final {
public:
  explicit FixedPointerList(std::size_t capacity)
      : entries_{capacity == 0
                     ? nullptr
                     : std::make_unique_for_overwrite<Pointer[]>(capacity)},
        capacity_{capacity} {}

  void push_back(Pointer pointer) noexcept {
    /* Every stable record can become active at most once per epoch. The
     * corresponding fixed link table has the same capacity, so exhaustion is
     * an internal invariant violation rather than a reason to allocate.
     */
    if (size_ >= capacity_)
      std::terminate();
    entries_[size_++] = pointer;
  }

  void clear() noexcept { size_ = 0; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] Pointer *begin() noexcept { return entries_.get(); }
  [[nodiscard]] Pointer *end() noexcept {
    return size_ == 0 ? entries_.get() : entries_.get() + size_;
  }
  [[nodiscard]] const Pointer *begin() const noexcept { return entries_.get(); }
  [[nodiscard]] const Pointer *end() const noexcept {
    return size_ == 0 ? entries_.get() : entries_.get() + size_;
  }

private:
  std::unique_ptr<Pointer[]> entries_;
  std::size_t capacity_{};
  std::size_t size_{};
};

class JitExecutor::Implementation final {
public:
  explicit Implementation(
      JitCacheExecutorLease lease, JitExecutorOptions options
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
  [[nodiscard]] bool budget_admitted() const noexcept;
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
  void discard_links() noexcept;
  void deactivate_links() noexcept;
  void deactivate_indirect_links() noexcept;
  void prepare_links(unsigned long generation, std::uint64_t changes);
  void refresh_cached_link_target(std::uint64_t pc);
  [[gnu::noinline]] void refresh_link(CompiledBlock &block);
  [[gnu::always_inline]] void activate_link(CompiledBlock &block) {
    if (block.link_epoch != link_epoch_)
      refresh_link(block);
  }

  [[nodiscard]] SharedTranslationCache::CodegenProfile
  shared_codegen_profile() const noexcept;
  /* Rewrites a shared translation's link pointers to this executor's tables.
   * False when a link record could not be obtained, in which case the
   * translation's code shape does not match what this executor can run and it
   * must compile for itself.
   */
  [[nodiscard]] std::optional<SharedTranslationMaterializationFailure>
  materialize_shared_translation(const SharedTranslation &translation);
  [[nodiscard]] CompiledBlock *install_shared_translation(
      unsigned long generation, std::uint64_t changes, std::uint64_t pc,
      std::span<const std::uint32_t> guest_words);
  [[nodiscard]] std::shared_ptr<const SharedTranslation>
  publish_shared_translation(
      std::uint64_t pc, std::span<const std::uint32_t> guest_words,
      const JitCompiler::Result &compiled);

  [[nodiscard]] JitCacheValidationResult validate_stale_block(
      const CompiledBlock &block, std::uint64_t changes,
      StaticGuestMemory &memory);
  [[nodiscard]] CompiledBlock *find(unsigned long generation,
                                    std::uint64_t changes, std::uint64_t pc,
                                    StaticGuestMemory &memory);
  [[nodiscard]] CacheSlot replacement(unsigned long generation,
                                      std::uint64_t changes,
                                      std::uint64_t pc);
  CompiledBlock &store_native(unsigned long generation, std::uint64_t changes,
                              std::uint64_t pc, JitFunction function,
                              std::uintptr_t internal_entry,
                              JitBlockLink *target_link,
                              std::span<const JitRelocation> relocations,
                              std::size_t direct_link_refusals,
                              std::span<const std::uint32_t> guest_words,
                              std::shared_ptr<const std::uint32_t[]>
                                  saved_guest_words,
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

  JitCacheExecutorLease cache_lease_;
  JitCacheGeometry geometry_;
  std::unique_ptr<BlockCache> block_cache_;
  /* Allocated without value-initializing the word payload. Private
   * translations touch only their cache slots; shared translations retain
   * the global immutable owner and leave these pages uncommitted.
   */
  std::shared_ptr<SavedGuestWordSet[]> fallback_saved_guest_words_;
  JitBlockLink empty_link_;
  JitBlockLinkTable links_;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  JitProfileBlockLinkTable profile_links_;
#endif
  JitIndirectLinkTable indirect_links_;
  FixedPointerList<JitBlockLink *> active_links_;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  FixedPointerList<JitBlockLink *> active_profile_links_;
#endif
  FixedPointerList<JitIndirectLink *> active_indirect_links_;
  jit::detail::ExecutableArena arena_;
  jit::detail::ExecutableArena tracked_store_arena_;
  jit::detail::ExecutableArena floating_transaction_arena_;
  Tlb tlb_;
  void *execution_context_{};
  unsigned long execution_generation_{};
  StaticGuestMemory *prepared_memory_{};
  unsigned long mapping_generation_{};
  std::uint64_t mapping_changes_{};
  unsigned long link_generation_{};
  std::uint64_t link_code_changes_{};
  std::uint64_t link_epoch_{1};
  std::uint64_t compiled_in_link_sequence_{};
  std::uint64_t stamp_{};
  std::array<JitStoreRange, maximum_block_instructions>
      tracked_store_ranges_{};
  /* Link pointers resolved for a reused translation, applied by the arena as
   * it stores the code.
   */
  std::vector<std::uintptr_t> relocated_link_pointers_;
  JitStoreParticipant store_participant_;
  CpuState *current_cpu_{};
  std::uint32_t floating_point_syndrome_{};
#if UML_JIT_RUNTIME_DIAGNOSTICS
  JitCacheValidationDiagnostics cache_validation_diagnostics_{};
  JitIndirectLinkDiagnostics indirect_link_diagnostics_{};
  void *shared_publication_context_{};
  JitExecutorDiagnosticsOptions::BeforeSharedPublication
      before_shared_publication_{};
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
