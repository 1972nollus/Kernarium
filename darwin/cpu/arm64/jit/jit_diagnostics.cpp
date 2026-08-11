// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace uml::cpu::arm64 {

#if UML_ENABLE_JIT_PROFILE
namespace {

/* Cross-thread translation overlap.
 *
 * Every executor owns a private block cache, so two threads running the same
 * guest code each translate it separately. This records the distinct guest PCs
 * each thread translated and merges those sets to count how many threads
 * translated each PC. The ratio of summed per-thread distinct PCs to their
 * union is the duplicated work a shared cache would remove; a low ratio means
 * a shared cache buys nothing and only adds a lock to the compile path.
 */

[[nodiscard]] bool jit_overlap_profile_enabled() noexcept {
  static const bool enabled = [] {
    const auto *value = std::getenv("UML_JIT_PROFILE_OVERLAP");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

[[nodiscard]] std::uint64_t jit_overlap_report_interval() noexcept {
  static const std::uint64_t interval = [] {
    const auto *value = std::getenv("UML_JIT_PROFILE_OVERLAP_INTERVAL");
    if (value == nullptr || value[0] == '\0')
      return std::uint64_t{1} << 20U;
    const auto parsed = std::strtoull(value, nullptr, 0);
    return parsed == 0 ? std::uint64_t{1} << 20U : parsed;
  }();
  return interval;
}

[[nodiscard]] std::size_t overlap_slot(std::uint64_t pc,
                                       std::size_t mask) noexcept {
  auto hash = pc * 0x9e3779b97f4a7c15ULL;
  hash ^= hash >> 29U;
  return static_cast<std::size_t>(hash) & mask;
}

/* Open-addressed set of guest PCs. Slot value 0 is empty; no guest block
 * starts at PC 0, so no sentinel word is needed.
 */
class TranslatedPcSet final {
public:
  [[nodiscard]] bool insert(std::uint64_t pc) {
    if (pc == 0)
      return false;
    if (slots_.empty() || (count_ + 1U) * 4U > slots_.size() * 3U)
      grow();
    if (!place(slots_, pc))
      return false;
    ++count_;
    return true;
  }

private:
  [[nodiscard]] static bool place(std::vector<std::uint64_t> &slots,
                                  std::uint64_t pc) {
    const auto mask = slots.size() - 1U;
    for (auto index = overlap_slot(pc, mask);; index = (index + 1U) & mask) {
      if (slots[index] == pc)
        return false;
      if (slots[index] == 0) {
        slots[index] = pc;
        return true;
      }
    }
  }

  void grow() {
    std::vector<std::uint64_t> grown(
        slots_.empty() ? std::size_t{1024} : slots_.size() * 2U);
    for (const auto pc : slots_)
      if (pc != 0)
        (void)place(grown, pc);
    slots_.swap(grown);
  }

  std::vector<std::uint64_t> slots_;
  std::size_t count_{};
};

/* Guest PC to the number of distinct threads that translated it. Only the
 * registry mutates this, and only under its mutex.
 */
class TranslatedPcCounts final {
public:
  TranslatedPcCounts() : keys_(4096), counts_(4096) {}

  void add(std::uint64_t pc) {
    const auto mask = keys_.size() - 1U;
    for (auto index = overlap_slot(pc, mask);; index = (index + 1U) & mask) {
      if (keys_[index] == pc) {
        ++counts_[index];
        return;
      }
      if (keys_[index] == 0) {
        if (saturated_) {
          ++overflow_;
          return;
        }
        keys_[index] = pc;
        counts_[index] = 1;
        ++count_;
        if (count_ * 4U > keys_.size() * 3U)
          grow();
        return;
      }
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t overflow() const noexcept { return overflow_; }

  void tally(std::span<std::uint64_t> shares) const {
    for (std::size_t index = 0; index < keys_.size(); ++index) {
      if (keys_[index] == 0)
        continue;
      const auto bucket = std::min<std::size_t>(counts_[index] - 1U,
                                                shares.size() - 1U);
      ++shares[bucket];
    }
  }

private:
  /* 8.4M distinct PCs before the table stops admitting new ones. A run that
   * saturates reports its overflow count so the union stays honest.
   */
  static constexpr std::size_t maximum_slots = std::size_t{1} << 23U;

  void grow() {
    if (keys_.size() >= maximum_slots) {
      saturated_ = true;
      return;
    }
    std::vector<std::uint64_t> grown_keys(keys_.size() * 2U);
    std::vector<std::uint32_t> grown_counts(keys_.size() * 2U);
    const auto mask = grown_keys.size() - 1U;
    for (std::size_t source = 0; source < keys_.size(); ++source) {
      if (keys_[source] == 0)
        continue;
      auto index = overlap_slot(keys_[source], mask);
      while (grown_keys[index] != 0)
        index = (index + 1U) & mask;
      grown_keys[index] = keys_[source];
      grown_counts[index] = counts_[source];
    }
    keys_.swap(grown_keys);
    counts_.swap(grown_counts);
  }

  std::vector<std::uint64_t> keys_;
  std::vector<std::uint32_t> counts_;
  std::size_t count_{};
  std::uint64_t overflow_{};
  bool saturated_{};
};

struct OverlapRegistry final {
  std::mutex mutex;
  TranslatedPcCounts counts;
  unsigned threads{};
  std::uint64_t translations{};
  std::uint64_t distinct_sum{};
  std::uint64_t next_report{};
};

OverlapRegistry &overlap_registry() noexcept {
  /* Intentionally leaked. Thread-local recorders flush into it from their
   * destructors, which can run after a function-local static would already
   * have been destroyed.
   */
  static auto *registry = new OverlapRegistry{};
  return *registry;
}

void report_translation_overlap(const OverlapRegistry &registry) {
  std::array<std::uint64_t, 64> shares{};
  registry.counts.tally(shares);
  const auto onion = static_cast<double>(
      std::max<std::size_t>(registry.counts.size(), 1));
  std::fprintf(stderr,
               "Darwin JIT translation overlap threads=%u translations=%llu "
               "per-thread-distinct=%llu union-distinct=%zu duplication=%.2f "
               "overflow=%llu shared-by=",
               registry.threads,
               static_cast<unsigned long long>(registry.translations),
               static_cast<unsigned long long>(registry.distinct_sum),
               registry.counts.size(),
               static_cast<double>(registry.distinct_sum) / onion,
               static_cast<unsigned long long>(registry.counts.overflow()));
  const auto reported =
      std::min<std::size_t>(std::max(registry.threads, 1U), shares.size());
  for (std::size_t index = 0; index < reported; ++index)
    std::fprintf(stderr, "%s%zu:%llu", index == 0 ? "" : ",", index + 1U,
                 static_cast<unsigned long long>(shares[index]));
  std::fputc('\n', stderr);
}

/* Per-thread recorder. The set is thread-private; only batched pending PCs
 * cross into the registry, and only under its mutex.
 */
class ThreadOverlapRecorder final {
public:
  ThreadOverlapRecorder() {
    auto &registry = overlap_registry();
    std::scoped_lock registry_lock{registry.mutex};
    ++registry.threads;
  }

  ~ThreadOverlapRecorder() { flush(); }

  ThreadOverlapRecorder(const ThreadOverlapRecorder &) = delete;
  ThreadOverlapRecorder &operator=(const ThreadOverlapRecorder &) = delete;

  void record(std::uint64_t pc) {
    ++translations_;
    if (translated_.insert(pc))
      pending_.push_back(pc);
    if (pending_.size() >= flush_threshold ||
        translations_ >= flush_threshold)
      flush();
  }

private:
  static constexpr std::size_t flush_threshold = 4096;

  void flush() {
    auto &registry = overlap_registry();
    std::scoped_lock registry_lock{registry.mutex};
    for (const auto pc : pending_)
      registry.counts.add(pc);
    registry.distinct_sum += pending_.size();
    registry.translations += translations_;
    pending_.clear();
    translations_ = 0;
    if (registry.translations < registry.next_report)
      return;
    registry.next_report = registry.translations + jit_overlap_report_interval();
    report_translation_overlap(registry);
  }

  TranslatedPcSet translated_;
  std::vector<std::uint64_t> pending_;
  std::uint64_t translations_{};
};

void record_translation_overlap(std::uint64_t pc) {
  if (!jit_overlap_profile_enabled())
    return;
  static thread_local ThreadOverlapRecorder recorder;
  recorder.record(pc);
}

[[nodiscard]] bool jit_compile_profile_enabled() noexcept {
  static const bool enabled = [] {
    const auto *value = std::getenv("UML_JIT_PROFILE_COMPILE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

struct JitCompileProfile final {
  std::uint64_t blocks{};
  std::uint64_t guest_instructions{};
  std::uint64_t hot_words{};
  std::uint64_t total_words{};
  std::uint64_t tracked_blocks{};
  std::uint64_t evictions{};
  std::uint64_t arena_resets{};
  std::uint64_t code_sequence_transitions{};
  std::uint64_t generation_transitions{};
  std::uint64_t sequence_discarded_blocks{};
  std::uint64_t validation_attempts{};
  std::uint64_t validation_hits{};
  std::uint64_t validation_mismatches{};
  std::uint64_t validation_faults{};
  std::uint64_t validation_sequence_races{};
  std::array<std::uint64_t, 7> block_lengths{};
  std::uint64_t largest_ratio_pc{};
  std::size_t largest_ratio_guest{};
  std::size_t largest_ratio_hot{};
  std::size_t largest_ratio_total{};
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  std::array<std::uint64_t, 9> gpr_slot_high_water{};
  std::uint64_t gpr_allocation_refusals{};
  std::uint64_t gpr_fallback_read_loads{};
  std::uint64_t gpr_fallback_write_stores{};
  std::uint64_t gpr_repeated_overflow_accesses{};
  std::array<std::uint64_t,
             jit::jit_gpr_pressure_candidate_capacities.size()>
      gpr_estimated_avoided_accesses{};
#endif
};

JitCompileProfile &jit_compile_profile() noexcept {
  static thread_local JitCompileProfile profile;
  return profile;
}

[[nodiscard]] std::uint64_t jit_compile_profile_interval() noexcept {
  static const std::uint64_t interval = [] {
    const auto *value = std::getenv("UML_JIT_PROFILE_COMPILE_INTERVAL");
    if (value == nullptr || value[0] == '\0')
      return std::uint64_t{4096};
    const auto parsed = std::strtoull(value, nullptr, 0);
    return parsed == 0 ? std::uint64_t{4096} : parsed;
  }();
  return interval;
}

[[nodiscard]] bool jit_compile_expansion_dump_enabled() noexcept {
  static const bool enabled = [] {
    const auto *value = std::getenv("UML_JIT_DUMP_EXPANSION");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

[[nodiscard]] bool jit_compile_expansion_pc_enabled(
    std::uint64_t pc) noexcept {
  static const std::uint64_t minimum = [] {
    const auto *value = std::getenv("UML_JIT_DUMP_MIN_PC");
    return value == nullptr ? std::uint64_t{}
                            : std::strtoull(value, nullptr, 0);
  }();
  static const std::uint64_t maximum = [] {
    const auto *value = std::getenv("UML_JIT_DUMP_MAX_PC");
    return value == nullptr ? ~std::uint64_t{}
                            : std::strtoull(value, nullptr, 0);
  }();
  return pc >= minimum && pc <= maximum;
}

void dump_jit_expansion(std::uint64_t pc,
                        std::span<const std::uint32_t> guest_words,
                        std::span<const std::uint32_t> host_words,
                        std::size_t hot_words) {
  static thread_local unsigned emitted;
  if (!jit_compile_expansion_dump_enabled() ||
      !jit_compile_expansion_pc_enabled(pc) || emitted >= 32)
    return;
  ++emitted;
  static std::mutex output_mutex;
  std::scoped_lock output_lock{output_mutex};
  std::fprintf(stderr,
               "Darwin JIT expansion thread=%p pc=%#llx guest=%zu hot=%zu "
               "total=%zu guest-words=",
               static_cast<void *>(&emitted),
               static_cast<unsigned long long>(pc), guest_words.size(),
               hot_words, host_words.size());
  for (const auto word : guest_words)
    std::fprintf(stderr, "%08x,", word);
  std::fputs(" host-words=", stderr);
  for (const auto word : host_words.first(
           std::min<std::size_t>(hot_words, host_words.size())))
    std::fprintf(stderr, "%08x,", word);
  std::fputc('\n', stderr);
}

void report_jit_compile_profile(const JitCompileProfile &profile) {
  const auto guest =
      static_cast<double>(std::max<std::uint64_t>(
          profile.guest_instructions, std::uint64_t{1}));
  const auto blocks =
      static_cast<double>(std::max<std::uint64_t>(profile.blocks,
                                                 std::uint64_t{1}));
  std::fprintf(
      stderr,
      "Darwin JIT compile profile thread=%p blocks=%llu guest=%llu "
      "guest-per-block=%.2f hot-words=%llu total-words=%llu "
      "hot-per-guest=%.2f total-per-guest=%.2f tracked=%llu evictions=%llu "
      "arena-resets=%llu code-sequence-transitions=%llu "
      "generation-transitions=%llu sequence-discarded-blocks=%llu "
      "validation-attempts=%llu validation-hits=%llu "
      "validation-mismatches=%llu validation-faults=%llu "
      "validation-sequence-races=%llu "
      "lengths=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
      "largest-pc=%#llx largest-guest=%zu largest-hot=%zu largest-total=%zu\n",
      static_cast<const void *>(&profile),
      static_cast<unsigned long long>(profile.blocks),
      static_cast<unsigned long long>(profile.guest_instructions),
      profile.guest_instructions / blocks,
      static_cast<unsigned long long>(profile.hot_words),
      static_cast<unsigned long long>(profile.total_words),
      profile.hot_words / guest, profile.total_words / guest,
      static_cast<unsigned long long>(profile.tracked_blocks),
      static_cast<unsigned long long>(profile.evictions),
      static_cast<unsigned long long>(profile.arena_resets),
      static_cast<unsigned long long>(profile.code_sequence_transitions),
      static_cast<unsigned long long>(profile.generation_transitions),
      static_cast<unsigned long long>(profile.sequence_discarded_blocks),
      static_cast<unsigned long long>(profile.validation_attempts),
      static_cast<unsigned long long>(profile.validation_hits),
      static_cast<unsigned long long>(profile.validation_mismatches),
      static_cast<unsigned long long>(profile.validation_faults),
      static_cast<unsigned long long>(profile.validation_sequence_races),
      static_cast<unsigned long long>(profile.block_lengths[0]),
      static_cast<unsigned long long>(profile.block_lengths[1]),
      static_cast<unsigned long long>(profile.block_lengths[2]),
      static_cast<unsigned long long>(profile.block_lengths[3]),
      static_cast<unsigned long long>(profile.block_lengths[4]),
      static_cast<unsigned long long>(profile.block_lengths[5]),
      static_cast<unsigned long long>(profile.block_lengths[6]),
      static_cast<unsigned long long>(profile.largest_ratio_pc),
      profile.largest_ratio_guest, profile.largest_ratio_hot,
      profile.largest_ratio_total);
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  std::fprintf(
      stderr,
      "Darwin JIT GPR pressure aggregate thread=%p "
      "slot-high-water=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
      "allocation-refusals=%llu fallback-read-loads=%llu "
      "fallback-write-stores=%llu repeated-overflow-accesses=%llu "
      "estimated-avoided-cap10=%llu estimated-avoided-cap12=%llu "
      "estimated-avoided-cap15=%llu\n",
      static_cast<const void *>(&profile),
      static_cast<unsigned long long>(profile.gpr_slot_high_water[0]),
      static_cast<unsigned long long>(profile.gpr_slot_high_water[1]),
      static_cast<unsigned long long>(profile.gpr_slot_high_water[2]),
      static_cast<unsigned long long>(profile.gpr_slot_high_water[3]),
      static_cast<unsigned long long>(profile.gpr_slot_high_water[4]),
      static_cast<unsigned long long>(profile.gpr_slot_high_water[5]),
      static_cast<unsigned long long>(profile.gpr_slot_high_water[6]),
      static_cast<unsigned long long>(profile.gpr_slot_high_water[7]),
      static_cast<unsigned long long>(profile.gpr_slot_high_water[8]),
      static_cast<unsigned long long>(profile.gpr_allocation_refusals),
      static_cast<unsigned long long>(profile.gpr_fallback_read_loads),
      static_cast<unsigned long long>(profile.gpr_fallback_write_stores),
      static_cast<unsigned long long>(
          profile.gpr_repeated_overflow_accesses),
      static_cast<unsigned long long>(
          profile.gpr_estimated_avoided_accesses[0]),
      static_cast<unsigned long long>(
          profile.gpr_estimated_avoided_accesses[1]),
      static_cast<unsigned long long>(
          profile.gpr_estimated_avoided_accesses[2]));
#endif
}

#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
[[nodiscard]] bool jit_gpr_pressure_dump_enabled() noexcept {
  static const bool enabled = [] {
    const auto *value = std::getenv("UML_JIT_DUMP_GPR_PRESSURE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

void dump_jit_gpr_pressure(std::uint64_t pc,
                           std::size_t guest_instructions,
                           const jit::JitGprPressureProfile &pressure) {
  if (!jit_gpr_pressure_dump_enabled())
    return;
  static thread_local unsigned thread_identity;
  static std::mutex output_mutex;
  std::scoped_lock output_lock{output_mutex};
  std::fprintf(
      stderr,
      "Darwin JIT GPR pressure thread=%p pc=%#llx guest=%zu "
      "slot-high-water=%zu allocation-refusals=%zu "
      "fallback-read-loads=%zu fallback-write-stores=%zu "
      "repeated-overflow-accesses=%zu estimated-avoided-cap10=%zu "
      "estimated-avoided-cap12=%zu estimated-avoided-cap15=%zu\n",
      static_cast<void *>(&thread_identity),
      static_cast<unsigned long long>(pc), guest_instructions,
      pressure.slot_high_water, pressure.allocation_refusals,
      pressure.fallback_read_loads, pressure.fallback_write_stores,
      pressure.repeated_overflow_accesses,
      pressure.estimated_avoided_accesses[0],
      pressure.estimated_avoided_accesses[1],
      pressure.estimated_avoided_accesses[2]);
}
#endif

} // namespace

void profile_jit_compilation(
    std::uint64_t pc, std::span<const std::uint32_t> guest_words,
    std::span<const std::uint32_t> host_words, std::size_t hot_words,
    bool tracked
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
    ,
    const jit::JitGprPressureProfile &gpr_pressure
#endif
    ) {
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  dump_jit_gpr_pressure(pc, guest_words.size(), gpr_pressure);
#endif
  record_translation_overlap(pc);
  if (!jit_compile_profile_enabled())
    return;
  const auto guest_instructions = guest_words.size();
  const auto total_words = host_words.size();
  auto &profile = jit_compile_profile();
  ++profile.blocks;
  profile.guest_instructions += guest_instructions;
  profile.hot_words += hot_words;
  profile.total_words += total_words;
  profile.tracked_blocks += tracked;
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  const auto high_water = std::min(
      gpr_pressure.slot_high_water,
      profile.gpr_slot_high_water.size() - 1U);
  ++profile.gpr_slot_high_water[high_water];
  profile.gpr_allocation_refusals +=
      gpr_pressure.allocation_refusals;
  profile.gpr_fallback_read_loads +=
      gpr_pressure.fallback_read_loads;
  profile.gpr_fallback_write_stores +=
      gpr_pressure.fallback_write_stores;
  profile.gpr_repeated_overflow_accesses +=
      gpr_pressure.repeated_overflow_accesses;
  for (std::size_t index = 0;
       index < profile.gpr_estimated_avoided_accesses.size(); ++index)
    profile.gpr_estimated_avoided_accesses[index] +=
        gpr_pressure.estimated_avoided_accesses[index];
#endif
  const auto bucket =
      guest_instructions <= 1
          ? 0U
          : guest_instructions <= 2
                ? 1U
                : guest_instructions <= 4
                      ? 2U
                      : guest_instructions <= 8
                            ? 3U
                            : guest_instructions <= 16
                                  ? 4U
                                  : guest_instructions <= 32 ? 5U : 6U;
  ++profile.block_lengths[bucket];
  const auto prior_guest =
      std::max<std::size_t>(profile.largest_ratio_guest, 1);
  const auto current_guest = std::max<std::size_t>(guest_instructions, 1);
  if (total_words * prior_guest >
      profile.largest_ratio_total * current_guest) {
    profile.largest_ratio_pc = pc;
    profile.largest_ratio_guest = guest_instructions;
    profile.largest_ratio_hot = hot_words;
    profile.largest_ratio_total = total_words;
    dump_jit_expansion(pc, guest_words, host_words, hot_words);
  }
  if ((profile.blocks % jit_compile_profile_interval()) == 0)
    report_jit_compile_profile(profile);
}

void profile_jit_cache_replacement(bool eviction) noexcept {
  if (eviction && jit_compile_profile_enabled())
    ++jit_compile_profile().evictions;
}

void profile_jit_arena_reset() noexcept {
  if (jit_compile_profile_enabled())
    ++jit_compile_profile().arena_resets;
}

void profile_jit_sequence_transition(bool generation_changed,
                                     std::uint64_t discarded_blocks) noexcept {
  if (!jit_compile_profile_enabled())
    return;
  auto &profile = jit_compile_profile();
  if (generation_changed)
    ++profile.generation_transitions;
  else
    ++profile.code_sequence_transitions;
  profile.sequence_discarded_blocks += discarded_blocks;
}

void profile_jit_cache_validation(
    JitCacheValidationResult result) noexcept {
  if (!jit_compile_profile_enabled())
    return;
  auto &profile = jit_compile_profile();
  ++profile.validation_attempts;
  switch (result) {
  case JitCacheValidationResult::hit:
    ++profile.validation_hits;
    break;
  case JitCacheValidationResult::mismatch:
    ++profile.validation_mismatches;
    break;
  case JitCacheValidationResult::fault:
    ++profile.validation_faults;
    break;
  case JitCacheValidationResult::sequence_race:
    ++profile.validation_sequence_races;
    break;
  }
}
#endif

} // namespace uml::cpu::arm64
