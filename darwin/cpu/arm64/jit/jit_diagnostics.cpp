// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_diagnostics.hpp"

#include "jit_profile_output.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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

class ThreadOverlapRecorder;

struct OverlapRegistry final {
  std::mutex mutex;
  TranslatedPcCounts counts;
  ThreadOverlapRecorder *live_recorders{};
  unsigned threads{};
  std::uint64_t translations{};
  std::uint64_t distinct_sum{};
  std::uint64_t next_report{};
  std::uint64_t final_reported_translations{};
};

struct TranslationOverlapReport final {
  std::array<std::uint64_t, 64> shares{};
  unsigned threads{};
  std::uint64_t translations{};
  std::uint64_t distinct_sum{};
  std::size_t union_distinct{};
  std::uint64_t overflow{};
};

OverlapRegistry &overlap_registry() noexcept;
[[nodiscard]] TranslationOverlapReport
snapshot_translation_overlap(const OverlapRegistry &registry) noexcept;
void report_translation_overlap(const TranslationOverlapReport &report) noexcept;
void report_final_translation_overlap() noexcept;

OverlapRegistry &overlap_registry() noexcept {
  /* Intentionally leaked. Thread-local recorders flush into it from their
   * destructors, which can run after a function-local static would already
   * have been destroyed.
   */
  static auto *registry = [] {
    auto *created = new OverlapRegistry{};
    std::atexit(report_final_translation_overlap);
    return created;
  }();
  return *registry;
}

TranslationOverlapReport
snapshot_translation_overlap(const OverlapRegistry &registry) noexcept {
  TranslationOverlapReport report{
      .threads = registry.threads,
      .translations = registry.translations,
      .distinct_sum = registry.distinct_sum,
      .union_distinct = registry.counts.size(),
      .overflow = registry.counts.overflow(),
  };
  registry.counts.tally(report.shares);
  return report;
}

void report_translation_overlap(
    const TranslationOverlapReport &report) noexcept {
  const auto onion = static_cast<double>(
      std::max<std::size_t>(report.union_distinct, 1));
  JitProfileRecord record;
  record.append(
      "Darwin JIT translation overlap threads=%u translations=%llu "
      "per-thread-distinct=%llu union-distinct=%zu duplication=%.2f "
      "overflow=%llu shared-by=",
      report.threads,
      static_cast<unsigned long long>(report.translations),
      static_cast<unsigned long long>(report.distinct_sum),
      report.union_distinct,
      static_cast<double>(report.distinct_sum) / onion,
      static_cast<unsigned long long>(report.overflow));
  const auto reported =
      std::min<std::size_t>(std::max(report.threads, 1U), report.shares.size());
  for (std::size_t index = 0; index < reported; ++index)
    record.append("%s%zu:%llu", index == 0 ? "" : ",", index + 1U,
                  static_cast<unsigned long long>(report.shares[index]));
  record.emit();
}

/* Per-thread recorder. The set is thread-private; only batched pending PCs
 * cross into the registry, and only under its mutex.
 */
class ThreadOverlapRecorder final {
public:
  ThreadOverlapRecorder() {
    auto &registry = overlap_registry();
    std::scoped_lock registry_lock{registry.mutex};
    next_ = registry.live_recorders;
    if (next_ != nullptr)
      next_->previous_ = this;
    registry.live_recorders = this;
    ++registry.threads;
  }

  ~ThreadOverlapRecorder() {
    auto &registry = overlap_registry();
    TranslationOverlapReport report;
    bool emit = false;
    {
      std::scoped_lock registry_lock{registry.mutex};
      emit = flush_locked(registry, true);
      if (emit)
        report = snapshot_translation_overlap(registry);
      if (previous_ != nullptr)
        previous_->next_ = next_;
      else
        registry.live_recorders = next_;
      if (next_ != nullptr)
        next_->previous_ = previous_;
    }
    if (emit)
      report_translation_overlap(report);
  }

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
  friend void report_final_translation_overlap() noexcept;

  void flush() {
    auto &registry = overlap_registry();
    TranslationOverlapReport report;
    bool emit = false;
    {
      std::scoped_lock registry_lock{registry.mutex};
      emit = flush_locked(registry, true);
      if (emit)
        report = snapshot_translation_overlap(registry);
    }
    if (emit)
      report_translation_overlap(report);
  }

  [[nodiscard]] bool flush_locked(OverlapRegistry &registry, bool periodic) {
    if (pending_.empty() && translations_ == 0)
      return false;
    for (const auto pc : pending_)
      registry.counts.add(pc);
    registry.distinct_sum += pending_.size();
    registry.translations += translations_;
    pending_.clear();
    translations_ = 0;
    if (!periodic || registry.translations < registry.next_report)
      return false;
    registry.next_report = registry.translations + jit_overlap_report_interval();
    return true;
  }

  TranslatedPcSet translated_;
  std::vector<std::uint64_t> pending_;
  std::uint64_t translations_{};
  ThreadOverlapRecorder *previous_{};
  ThreadOverlapRecorder *next_{};
};

void report_final_translation_overlap() noexcept {
  if (!jit_overlap_profile_enabled())
    return;
  auto &registry = overlap_registry();
  TranslationOverlapReport report;
  bool emit = false;
  {
    const std::scoped_lock registry_lock{registry.mutex};
    for (auto *recorder = registry.live_recorders; recorder != nullptr;
         recorder = recorder->next_)
      (void)recorder->flush_locked(registry, false);
    emit = registry.translations != registry.final_reported_translations;
    if (emit)
      report = snapshot_translation_overlap(registry);
    registry.final_reported_translations = registry.translations;
  }
  if (emit)
    report_translation_overlap(report);
}

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
  std::uint64_t direct_link_refusals{};
  std::uint64_t indirect_link_refusals{};
  std::uint64_t target_link_refusals{};
  std::uint64_t direct_link_table_high_water{};
  std::uint64_t direct_link_table_capacity{};
  std::uint64_t indirect_link_table_high_water{};
  std::uint64_t indirect_link_table_capacity{};
  std::uint64_t evictions{};
  std::uint64_t arena_resets{};
  std::uint64_t link_table_resets{};
  std::uint64_t code_sequence_transitions{};
  std::uint64_t generation_transitions{};
  std::uint64_t sequence_discarded_blocks{};
  std::uint64_t validation_attempts{};
  std::uint64_t validation_hits{};
  std::uint64_t validation_mismatches{};
  std::uint64_t validation_faults{};
  std::uint64_t validation_sequence_races{};
  std::uint64_t local_compile_calls{};
  std::uint64_t local_compile_nanoseconds{};
  std::uint64_t shared_materialization_calls{};
  std::uint64_t shared_materialization_nanoseconds{};
  std::array<std::uint64_t, 7> block_lengths{};
  std::uint64_t largest_ratio_pc{};
  std::size_t largest_ratio_guest{};
  std::size_t largest_ratio_hot{};
  std::size_t largest_ratio_total{};

  bool operator==(const JitCompileProfile &) const noexcept = default;
};

void report_jit_compile_profile(const JitCompileProfile &profile,
                                bool final = false);

struct ThreadJitCompileProfile;

struct JitCompileProfileRegistry final {
  std::mutex mutex;
  JitCompileProfile aggregate;
  ThreadJitCompileProfile *live_recorders{};
  JitCompileProfile final_reported_profile;
  bool final_reported{};
};

JitCompileProfileRegistry &jit_compile_profile_registry() noexcept;
void report_final_jit_compile_profile() noexcept;

JitCompileProfileRegistry &jit_compile_profile_registry() noexcept {
  static auto *registry = [] {
    auto *created = new JitCompileProfileRegistry{};
    std::atexit(report_final_jit_compile_profile);
    return created;
  }();
  return *registry;
}

void merge_jit_compile_profile(JitCompileProfile &destination,
                               const JitCompileProfile &source) noexcept {
  destination.blocks += source.blocks;
  destination.guest_instructions += source.guest_instructions;
  destination.hot_words += source.hot_words;
  destination.total_words += source.total_words;
  destination.tracked_blocks += source.tracked_blocks;
  destination.direct_link_refusals += source.direct_link_refusals;
  destination.indirect_link_refusals += source.indirect_link_refusals;
  destination.target_link_refusals += source.target_link_refusals;
  destination.direct_link_table_high_water = std::max(
      destination.direct_link_table_high_water,
      source.direct_link_table_high_water);
  destination.direct_link_table_capacity = std::max(
      destination.direct_link_table_capacity,
      source.direct_link_table_capacity);
  destination.indirect_link_table_high_water = std::max(
      destination.indirect_link_table_high_water,
      source.indirect_link_table_high_water);
  destination.indirect_link_table_capacity = std::max(
      destination.indirect_link_table_capacity,
      source.indirect_link_table_capacity);
  destination.evictions += source.evictions;
  destination.arena_resets += source.arena_resets;
  destination.link_table_resets += source.link_table_resets;
  destination.code_sequence_transitions += source.code_sequence_transitions;
  destination.generation_transitions += source.generation_transitions;
  destination.sequence_discarded_blocks += source.sequence_discarded_blocks;
  destination.validation_attempts += source.validation_attempts;
  destination.validation_hits += source.validation_hits;
  destination.validation_mismatches += source.validation_mismatches;
  destination.validation_faults += source.validation_faults;
  destination.validation_sequence_races += source.validation_sequence_races;
  destination.local_compile_calls += source.local_compile_calls;
  destination.local_compile_nanoseconds += source.local_compile_nanoseconds;
  destination.shared_materialization_calls +=
      source.shared_materialization_calls;
  destination.shared_materialization_nanoseconds +=
      source.shared_materialization_nanoseconds;
  for (std::size_t index = 0; index < destination.block_lengths.size(); ++index)
    destination.block_lengths[index] += source.block_lengths[index];
  const auto destination_guest =
      std::max<std::size_t>(destination.largest_ratio_guest, 1);
  const auto source_guest =
      std::max<std::size_t>(source.largest_ratio_guest, 1);
  if (source.largest_ratio_total * destination_guest >
      destination.largest_ratio_total * source_guest) {
    destination.largest_ratio_pc = source.largest_ratio_pc;
    destination.largest_ratio_guest = source.largest_ratio_guest;
    destination.largest_ratio_hot = source.largest_ratio_hot;
    destination.largest_ratio_total = source.largest_ratio_total;
  }
}

struct ThreadJitCompileProfile final {
  ThreadJitCompileProfile() {
    auto &registry = jit_compile_profile_registry();
    const std::scoped_lock registry_lock{registry.mutex};
    next = registry.live_recorders;
    if (next != nullptr)
      next->previous = this;
    registry.live_recorders = this;
  }

  ~ThreadJitCompileProfile() {
    auto &registry = jit_compile_profile_registry();
    const std::scoped_lock registry_lock{registry.mutex};
    merge_jit_compile_profile(registry.aggregate, profile);
    if (previous != nullptr)
      previous->next = next;
    else
      registry.live_recorders = next;
    if (next != nullptr)
      next->previous = previous;
  }

  JitCompileProfile profile;
  ThreadJitCompileProfile *previous{};
  ThreadJitCompileProfile *next{};
};

void report_final_jit_compile_profile() noexcept {
  if (!jit_compile_profile_enabled())
    return;
  auto &registry = jit_compile_profile_registry();
  JitCompileProfile aggregate;
  {
    const std::scoped_lock registry_lock{registry.mutex};
    for (auto *recorder = registry.live_recorders; recorder != nullptr;
         recorder = recorder->next) {
      merge_jit_compile_profile(registry.aggregate, recorder->profile);
      recorder->profile = {};
    }
    if (registry.aggregate.blocks == 0 ||
        (registry.final_reported &&
         registry.aggregate == registry.final_reported_profile))
      return;
    registry.final_reported_profile = registry.aggregate;
    registry.final_reported = true;
    aggregate = registry.aggregate;
  }
  report_jit_compile_profile(aggregate, true);
}

JitCompileProfile &jit_compile_profile() noexcept {
  static thread_local ThreadJitCompileProfile recorder;
  return recorder.profile;
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

constexpr std::size_t expansion_words_per_record = 24;

[[nodiscard]] constexpr std::size_t expansion_record_count(
    std::size_t words) noexcept {
  return words / expansion_words_per_record +
         (words % expansion_words_per_record != 0);
}

void write_jit_expansion_words(
    const void *thread, std::uint64_t pc, const char *section,
    std::span<const std::uint32_t> words) noexcept {
  const auto records = expansion_record_count(words.size());
  for (std::size_t record_index = 0; record_index < records;
       ++record_index) {
    const auto offset = record_index * expansion_words_per_record;
    const auto count = std::min(expansion_words_per_record,
                                words.size() - offset);
    JitProfileRecord record;
    record.append(
        "Darwin JIT expansion words thread=%p pc=%#llx section=%s "
        "chunk=%zu/%zu offset=%zu count=%zu words=",
        thread, static_cast<unsigned long long>(pc), section,
        record_index + 1U, records, offset, count);
    for (const auto word : words.subspan(offset, count))
      record.append("%08x,", word);
    record.emit();
  }
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
  const auto hot = host_words.first(
      std::min<std::size_t>(hot_words, host_words.size()));
  const void *thread = &emitted;
  write_jit_profile_record(
      "Darwin JIT expansion thread=%p pc=%#llx guest=%zu hot=%zu total=%zu "
      "guest-chunks=%zu host-chunks=%zu\n",
      thread, static_cast<unsigned long long>(pc), guest_words.size(),
      hot_words, host_words.size(), expansion_record_count(guest_words.size()),
      expansion_record_count(hot.size()));
  write_jit_expansion_words(thread, pc, "guest", guest_words);
  write_jit_expansion_words(thread, pc, "host", hot);
}

void report_jit_compile_profile(const JitCompileProfile &profile,
                                bool final) {
  const auto guest =
      static_cast<double>(std::max<std::uint64_t>(
          profile.guest_instructions, std::uint64_t{1}));
  const auto blocks =
      static_cast<double>(std::max<std::uint64_t>(profile.blocks,
                                                 std::uint64_t{1}));
  const auto compile_calls = static_cast<double>(std::max<std::uint64_t>(
      profile.local_compile_calls, std::uint64_t{1}));
  const auto materialization_calls =
      static_cast<double>(std::max<std::uint64_t>(
          profile.shared_materialization_calls, std::uint64_t{1}));
  write_jit_profile_record(
      "Darwin JIT compile profile %s=%p blocks=%llu guest=%llu "
      "guest-per-block=%.2f hot-words=%llu total-words=%llu "
      "hot-per-guest=%.2f total-per-guest=%.2f tracked=%llu evictions=%llu "
      "direct-link-refusals=%llu indirect-link-refusals=%llu "
      "target-link-refusals=%llu "
      "direct-link-table-high-water=%llu direct-link-table-capacity=%llu "
      "indirect-link-table-high-water=%llu "
      "indirect-link-table-capacity=%llu "
      "arena-resets=%llu link-table-resets=%llu "
      "code-sequence-transitions=%llu "
      "generation-transitions=%llu sequence-discarded-blocks=%llu "
      "validation-attempts=%llu validation-hits=%llu "
      "validation-mismatches=%llu validation-faults=%llu "
      "validation-sequence-races=%llu "
      "local-compile-calls=%llu local-compile-ns=%llu "
      "local-compile-avg-ns=%.2f "
      "shared-materialization-calls=%llu shared-materialization-ns=%llu "
      "shared-materialization-avg-ns=%.2f "
      "lengths=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
      "largest-pc=%#llx largest-guest=%zu largest-hot=%zu largest-total=%zu\n",
      final ? "final" : "thread", static_cast<const void *>(&profile),
      static_cast<unsigned long long>(profile.blocks),
      static_cast<unsigned long long>(profile.guest_instructions),
      profile.guest_instructions / blocks,
      static_cast<unsigned long long>(profile.hot_words),
      static_cast<unsigned long long>(profile.total_words),
      profile.hot_words / guest, profile.total_words / guest,
      static_cast<unsigned long long>(profile.tracked_blocks),
      static_cast<unsigned long long>(profile.evictions),
      static_cast<unsigned long long>(profile.direct_link_refusals),
      static_cast<unsigned long long>(profile.indirect_link_refusals),
      static_cast<unsigned long long>(profile.target_link_refusals),
      static_cast<unsigned long long>(profile.direct_link_table_high_water),
      static_cast<unsigned long long>(profile.direct_link_table_capacity),
      static_cast<unsigned long long>(profile.indirect_link_table_high_water),
      static_cast<unsigned long long>(profile.indirect_link_table_capacity),
      static_cast<unsigned long long>(profile.arena_resets),
      static_cast<unsigned long long>(profile.link_table_resets),
      static_cast<unsigned long long>(profile.code_sequence_transitions),
      static_cast<unsigned long long>(profile.generation_transitions),
      static_cast<unsigned long long>(profile.sequence_discarded_blocks),
      static_cast<unsigned long long>(profile.validation_attempts),
      static_cast<unsigned long long>(profile.validation_hits),
      static_cast<unsigned long long>(profile.validation_mismatches),
      static_cast<unsigned long long>(profile.validation_faults),
      static_cast<unsigned long long>(profile.validation_sequence_races),
      static_cast<unsigned long long>(profile.local_compile_calls),
      static_cast<unsigned long long>(profile.local_compile_nanoseconds),
      profile.local_compile_nanoseconds / compile_calls,
      static_cast<unsigned long long>(profile.shared_materialization_calls),
      static_cast<unsigned long long>(
          profile.shared_materialization_nanoseconds),
      profile.shared_materialization_nanoseconds / materialization_calls,
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
}

} // namespace

void report_final_jit_diagnostics() noexcept {
  report_final_jit_compile_profile();
  report_final_translation_overlap();
}

void profile_jit_compilation(
    std::uint64_t pc, std::span<const std::uint32_t> guest_words,
    std::span<const std::uint32_t> host_words, std::size_t hot_words,
    bool tracked, std::size_t direct_link_refusals,
    std::size_t indirect_link_refusals
    ) {
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
  profile.direct_link_refusals += direct_link_refusals;
  profile.indirect_link_refusals += indirect_link_refusals;
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

void profile_jit_link_table_reset() noexcept {
  if (jit_compile_profile_enabled())
    ++jit_compile_profile().link_table_resets;
}

void profile_jit_link_table_state(
    std::size_t direct_size, std::size_t direct_capacity,
    std::size_t indirect_size, std::size_t indirect_capacity) noexcept {
  if (!jit_compile_profile_enabled())
    return;
  auto &profile = jit_compile_profile();
  profile.direct_link_table_high_water = std::max(
      profile.direct_link_table_high_water,
      static_cast<std::uint64_t>(direct_size));
  profile.direct_link_table_capacity = std::max(
      profile.direct_link_table_capacity,
      static_cast<std::uint64_t>(direct_capacity));
  profile.indirect_link_table_high_water = std::max(
      profile.indirect_link_table_high_water,
      static_cast<std::uint64_t>(indirect_size));
  profile.indirect_link_table_capacity = std::max(
      profile.indirect_link_table_capacity,
      static_cast<std::uint64_t>(indirect_capacity));
}

void profile_jit_target_link_refusal(bool refused) noexcept {
  if (refused && jit_compile_profile_enabled())
    ++jit_compile_profile().target_link_refusals;
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

std::uint64_t jit_profile_timestamp_ns() noexcept {
  if (!jit_compile_profile_enabled())
    return 0;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void profile_jit_local_compile_latency(std::uint64_t nanoseconds) noexcept {
  if (!jit_compile_profile_enabled() || nanoseconds == 0)
    return;
  auto &profile = jit_compile_profile();
  ++profile.local_compile_calls;
  profile.local_compile_nanoseconds += nanoseconds;
}

void profile_jit_shared_materialization_latency(
    std::uint64_t nanoseconds) noexcept {
  if (!jit_compile_profile_enabled() || nanoseconds == 0)
    return;
  auto &profile = jit_compile_profile();
  ++profile.shared_materialization_calls;
  profile.shared_materialization_nanoseconds += nanoseconds;
}
#endif

} // namespace uml::cpu::arm64
