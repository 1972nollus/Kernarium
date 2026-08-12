// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_compiler.hpp"
#include "jit_executor.hpp"
#include "jit_linking.hpp"
#include "jit_runtime_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace uml::cpu::arm64 {

struct CompiledBlock final {
  unsigned long generation{};
  std::uint64_t code_changes{};
  std::uint64_t pc{};
  JitFunction function{};
  JitFunction tracked_function{};
  std::uintptr_t internal_entry{};
  JitBlockLink *link{};
  std::uint64_t stamp{};
  std::uint64_t link_epoch{};
  std::uint64_t memory_write_mask{};
  std::uint64_t direct_memory_write_mask{};
  std::size_t instruction_count{};
  /* Exact guest bytes used to validate this block after an unrelated global
   * code-sequence change. Shared translations and every executor that
   * materializes one retain the same immutable allocation; a translation that
   * is not admitted to the shared cache aliases its executor's bounded
   * fallback slab.
   */
  std::shared_ptr<const std::uint32_t[]> saved_guest_words;
  bool accesses_fp_state{};
  bool link_target_safe{};
  bool native_fp{};
  bool native_fpmr{};
  bool guest_illegal{};
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  std::uint32_t profile_read_before_write{};
#endif
};

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
struct JitGprDirectLinkSample final {
  alignas(std::uint64_t) std::uint64_t successes{};
  const JitBlockLink *target{};
  std::uint64_t source_pc{};
  std::uint32_t dirty_live_out{};
};

struct JitGprIndirectLinkSample final {
  alignas(std::uint64_t) std::array<std::uint64_t, 2> successes{};
  const JitIndirectLink *site{};
  std::uint64_t source_pc{};
  std::uint32_t dirty_live_out{};
};

struct JitGprProfileEdgeKey final {
  std::uint64_t source_pc{};
  std::uint64_t target_pc{};
  std::uint32_t dirty_live_out{};
  std::uint32_t read_before_write{};
  bool indirect{};

  [[nodiscard]] bool
  operator==(const JitGprProfileEdgeKey &) const noexcept = default;
};

struct JitGprProfileEdgeHash final {
  [[nodiscard]] std::size_t
  operator()(const JitGprProfileEdgeKey &key) const noexcept;
};

class JitGprLinkProfileGlobal final {
public:
  using Edges = std::unordered_map<JitGprProfileEdgeKey, std::uint64_t,
                                   JitGprProfileEdgeHash>;
  using LiveCollector =
      void (*)(void *, JitGprLinkProfileDiagnostics &, Edges &) noexcept;

  struct LiveProfile final {
    void *context{};
    LiveCollector collect{};
  };

  [[nodiscard]] static JitGprLinkProfileGlobal &instance() noexcept;
  void merge(const JitGprLinkProfileDiagnostics &totals,
             const Edges &edges) noexcept;
  void register_live(void *context, LiveCollector collect);
  void unregister_live(void *context) noexcept;
  [[nodiscard]] JitGprLinkProfileDiagnostics diagnostics() noexcept;
  void report() noexcept;

private:
  static void report_at_exit() noexcept;

  std::mutex mutex_;
  Edges edges_;
  std::vector<LiveProfile> live_;
  JitGprLinkProfileDiagnostics totals_{};
  bool reported_{};
};

class JitGprLinkProfiler final {
public:
  JitGprLinkProfiler() noexcept;
  ~JitGprLinkProfiler();

  [[nodiscard]] std::uint64_t *
  add_direct(const JitBlockLink *target, std::uint64_t source_pc,
             std::uint32_t dirty_live_out);
  [[nodiscard]] jit::JitIndirectLinkProfileCounters
  add_indirect(const JitIndirectLink *site, std::uint64_t source_pc,
               std::uint32_t dirty_live_out);
  void record_outer_invocation() noexcept;
  void drain_direct() noexcept;
  void drain_indirect_slot(const JitIndirectLink &site,
                           std::size_t slot) noexcept;
  void drain_indirect_site(const JitIndirectLink &site) noexcept;
  void drain_all() noexcept;
  void clear_code() noexcept;
  [[nodiscard]] JitGprLinkProfileDiagnostics diagnostics() noexcept;
  void publish() noexcept;

private:
  void collect(JitGprLinkProfileDiagnostics &totals,
               JitGprLinkProfileGlobal::Edges &edges) noexcept;
  void drain(std::uint64_t &counter, const JitBlockLink *target,
             std::uint64_t source_pc, std::uint32_t dirty_live_out,
             bool indirect) noexcept;

  std::vector<std::unique_ptr<JitGprDirectLinkSample>> direct_;
  std::vector<std::unique_ptr<JitGprIndirectLinkSample>> indirect_;
  std::unordered_map<const JitIndirectLink *,
                     std::vector<JitGprIndirectLinkSample *>>
      indirect_by_site_;
  JitGprLinkProfileGlobal::Edges pending_edges_;
  JitGprLinkProfileDiagnostics lifetime_totals_{};
  JitGprLinkProfileDiagnostics pending_totals_{};
};
#endif

struct CacheSet final {
  std::array<CompiledBlock, cache_ways> ways{};
};

/* Sized at construction from the JIT cache budget; see
 * jit_cache_geometry.hpp. cache_index() must be given the same set count.
 */
using BlockCache = std::vector<CacheSet>;
using GuestWordBuffer =
    std::array<std::uint32_t, maximum_block_instructions>;
struct SavedGuestWordSet final {
  std::array<GuestWordBuffer, cache_ways> ways;
};

struct CacheSlot final {
  CompiledBlock *block{};
  GuestWordBuffer *fallback_guest_words{};
};

/* Geometry reserves this worst-case fallback payload for every cached block
 * even though shared translations leave their corresponding slab pages
 * untouched. Keeping the conservative charge makes rejected publications
 * and single-executor fallbacks stay inside the whole-process budget.
 */
inline constexpr std::size_t maximum_saved_guest_word_bytes =
    maximum_block_instructions * sizeof(std::uint32_t);
/* Conservative room for the single allocation and shared-ownership metadata
 * of an executor's fallback slab. The shared_ptr in each CompiledBlock is
 * already part of sizeof(CompiledBlock).
 */
inline constexpr std::size_t saved_guest_word_slab_allocation_allowance = 64U;

[[nodiscard]] constexpr std::size_t
cache_index(unsigned long generation, std::uint64_t pc,
            std::size_t sets) noexcept {
  const auto mixed = pc ^ (pc >> 17U) ^
                     (static_cast<std::uint64_t>(generation) * 0x9e37'79b9U);
  return static_cast<std::size_t>(mixed) & (sets - 1);
}

} // namespace uml::cpu::arm64
