// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_runtime_contract.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace uml::cpu::arm64 {

/*
 * Translations shared between executors.
 *
 * Every executor owns a private block cache, so ten guest CPUs running the
 * same guest code each decoded and emitted it. Measured over multi-core Clang
 * and Object Detection, the summed per-thread distinct blocks were 7.4x their
 * union: the same block translated by ten threads 70% of the time.
 *
 * An entry is keyed by the guest PC and the exact guest instruction words it
 * was translated from, so it is a pure function of its input and can never go
 * stale. That deliberately decouples this cache from the generation and
 * code_changes invalidation the per-executor block cache uses: if the guest
 * bytes at a PC are what they were, the translation of them is still correct,
 * and if they are not, the key does not match.
 *
 * The host code still belongs to the executor that runs it. A reusing
 * executor copies the words into its own arena and rewrites the recorded
 * relocations to its own link tables, so no executable memory, link record or
 * arena is shared and the compile path gains no lock that execution can see.
 * What is shared is the decode and emit work.
 */
struct SharedTranslation final {
  std::vector<std::uint32_t> words;
  std::vector<JitRelocation> relocations;
  std::vector<std::uint32_t> guest_words;
  std::size_t guest_instructions{};
  std::size_t hot_words{};
  std::size_t internal_entry_word{};
  std::uint64_t memory_write_mask{};
  std::uint64_t direct_memory_write_mask{};
  bool accesses_fp_state{};
  bool link_target_safe{};
  bool native_fp{};
  bool native_fpmr{};
  bool guest_illegal{};
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  std::uint32_t profile_read_before_write{};
#endif
};

struct SharedTranslationStatistics final {
  std::uint64_t hits{};
  std::uint64_t misses{};
  std::uint64_t insertions{};
  std::uint64_t rejected{};
  std::uint64_t entries{};
};

class SharedTranslationCache final {
public:
  /* Codegen options that change emitted code. Two executors may only share a
   * translation when these agree, so they are part of the identity of the
   * cache rather than of each entry.
   */
  struct CodegenProfile final {
    ::uml::cpu::HostCapabilities capabilities{};
    bool guarded_control_flow{};
    bool pointer_authentication{};

    /* Compared field by field rather than by bytes so padding cannot make
     * two identical configurations look different.
     */
    [[nodiscard]] bool operator==(const CodegenProfile &other) const noexcept;
  };

  [[nodiscard]] std::shared_ptr<const SharedTranslation>
  find(const CodegenProfile &profile, std::uint64_t guest_pc,
       std::span<const std::uint32_t> guest_words) const;

  void insert(const CodegenProfile &profile, std::uint64_t guest_pc,
              std::span<const std::uint32_t> guest_words,
              std::shared_ptr<const SharedTranslation> translation);

  /* Sharing only pays when executors run at the same time. A process with a
   * single executor would otherwise copy and store every translation it
   * compiles for a reader that never arrives, which for a workload of
   * mostly-unique blocks is pure cost. Lookups stay enabled unconditionally
   * because an empty cache makes them a hash probe.
   */
  void add_executor() noexcept;
  void remove_executor() noexcept;

  [[nodiscard]] SharedTranslationStatistics statistics() const noexcept;

  [[nodiscard]] static SharedTranslationCache &instance() noexcept;

private:
  struct Entry final {
    CodegenProfile profile;
    std::shared_ptr<const SharedTranslation> translation;
  };

  /* Bounded near the largest working set measured so far, 283,353 distinct
   * blocks, so a pathological guest cannot grow it without limit. At roughly
   * a kilobyte an entry this caps the shared cache near 256 MiB for the whole
   * process. Once full it stops admitting and executors compile for
   * themselves.
   */
  static constexpr std::size_t maximum_entries = 1U << 18U;
  /* One mutex for every executor's misses and insertions serialised them into
   * a convoy that cost more than the compilation it saved. Shard by guest PC
   * so ten executors rarely contend, and pad each shard so their locks do not
   * share a cache line.
   */
  static constexpr std::size_t shard_count = 64;
  static constexpr std::size_t shard_maximum_entries =
      maximum_entries / shard_count;

  struct alignas(64) Shard final {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::uint64_t, std::vector<Entry>> entries;
    std::size_t count{};
  };

  [[nodiscard]] static std::size_t shard_of(std::uint64_t guest_pc) noexcept {
    auto hash = guest_pc * 0x9e3779b97f4a7c15ULL;
    hash ^= hash >> 29U;
    return static_cast<std::size_t>(hash) & (shard_count - 1U);
  }

  std::array<Shard, shard_count> shards_{};
  /* Counted outside the lock, so readers holding only a shared lock do not
   * race on them.
   */
  mutable std::atomic<std::uint64_t> hits_{};
  mutable std::atomic<std::uint64_t> misses_{};
  std::atomic<std::uint64_t> insertions_{};
  std::atomic<std::uint64_t> rejected_{};
  std::atomic<unsigned> live_executors_{};
};

} // namespace uml::cpu::arm64
