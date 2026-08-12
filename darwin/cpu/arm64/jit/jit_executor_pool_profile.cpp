// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_executor_pool_profile.hpp"

#if UML_ENABLE_JIT_PROFILE

#include "jit_cache_geometry.hpp"
#include "jit_profile_output.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <mutex>

namespace uml::cpu::arm64 {
namespace {

constexpr std::size_t global_sequence_capacity = 4096;
static_assert(maximum_jit_cache_executors == 64);

[[nodiscard]] bool executor_pool_profile_enabled() noexcept {
  static const bool enabled = [] {
    const auto *value = std::getenv("UML_JIT_PROFILE_EXECUTOR_POOL");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

[[nodiscard]] constexpr std::size_t sequence_slot(
    std::uint64_t session, std::uintptr_t sequence) noexcept {
  auto hash = static_cast<std::uint64_t>(sequence) *
              std::uint64_t{0x9e3779b97f4a7c15ULL};
  hash ^= session + std::uint64_t{0x9e3779b97f4a7c15ULL} +
          (hash << 6U) + (hash >> 2U);
  hash ^= hash >> 29U;
  return static_cast<std::size_t>(hash) &
         (global_sequence_capacity - 1U);
}

struct SequenceFanout final {
  std::uint64_t session{};
  std::uintptr_t sequence{};
  std::uint64_t slot_mask{};
  std::uint64_t wrappers{};
};

struct ExecutorPoolProfileSnapshot final {
  JitExecutorPoolProfile::Counters counters;
  std::uint64_t sequence_distinct{};
  std::uint64_t sequence_shared{};
  std::uint64_t sequence_extra_wrapper_associations{};
  std::uint64_t sequence_max_fanout{};
  std::uint64_t sequence_multislot{};
  std::uint64_t sequence_extra_slot_associations{};
  std::uint64_t sequence_max_slot_fanout{};
  std::uint64_t sequence_global_overflow{};
  std::uint64_t standalone_constructions{};
  std::uint64_t standalone_live_peak{};

  [[nodiscard]] bool operator==(
      const ExecutorPoolProfileSnapshot &) const noexcept = default;
};

void merge_counters(JitExecutorPoolProfile::Counters &destination,
                    const JitExecutorPoolProfile::Counters &source) noexcept {
  destination.wrappers += source.wrappers;
  destination.borrows += source.borrows;
  destination.no_preference += source.no_preference;
  destination.preferred_attempts += source.preferred_attempts;
  destination.preferred_hits += source.preferred_hits;
  destination.fallback_claims += source.fallback_claims;
  destination.cells_probed += source.cells_probed;
  destination.failed_cas += source.failed_cas;
  destination.waiter_enqueues += source.waiter_enqueues;
  destination.waiter_wakes += source.waiter_wakes;
  destination.fifo_handoffs += source.fifo_handoffs;
  destination.max_waiters =
      std::max(destination.max_waiters, source.max_waiters);
  destination.construction_attempts += source.construction_attempts;
  destination.created += source.created;
  destination.peak_live = std::max(destination.peak_live, source.peak_live);
  destination.peak_borrowed =
      std::max(destination.peak_borrowed, source.peak_borrowed);
  destination.slot_changes += source.slot_changes;
  destination.wrappers_slot_changed += source.wrappers_slot_changed;
  destination.max_slot_changes =
      std::max(destination.max_slot_changes, source.max_slot_changes);
  destination.null_sequences += source.null_sequences;
  destination.sequence_changes += source.sequence_changes;
  destination.wrappers_sequence_changed +=
      source.wrappers_sequence_changed;
  destination.max_sequence_changes =
      std::max(destination.max_sequence_changes,
               source.max_sequence_changes);
  destination.same_sequence_different_slot +=
      source.same_sequence_different_slot;
  destination.sequence_association_overflow +=
      source.sequence_association_overflow;
}

void associate_sequence(std::array<SequenceFanout, global_sequence_capacity> &map,
                        std::uint64_t &overflow,
                        std::uint64_t session, std::uintptr_t sequence,
                        std::uint64_t slot_mask) noexcept {
  auto slot = sequence_slot(session, sequence);
  for (std::size_t probe = 0; probe < map.size(); ++probe) {
    auto &entry = map[slot];
    if (entry.session == session && entry.sequence == sequence) {
      ++entry.wrappers;
      entry.slot_mask |= slot_mask;
      return;
    }
    if (entry.sequence == 0) {
      entry.session = session;
      entry.sequence = sequence;
      entry.slot_mask = slot_mask;
      entry.wrappers = 1;
      return;
    }
    slot = (slot + 1U) & (map.size() - 1U);
  }
  ++overflow;
}

} // namespace

class JitExecutorPoolProfileRegistry final {
public:
  void register_wrapper(JitExecutorPoolProfile &profile) noexcept {
    std::lock_guard lock{mutex_};
    profile.next_ = live_profiles_;
    if (profile.next_ != nullptr)
      profile.next_->previous_ = &profile;
    live_profiles_ = &profile;
  }

  void retire_wrapper(JitExecutorPoolProfile &profile) noexcept {
    std::lock_guard lock{mutex_};
    merge_counters(retired_counters_, profile.counters_);
    merge_sequences(profile, retired_sequences_,
                    retired_sequence_overflow_);
    if (profile.previous_ != nullptr)
      profile.previous_->next_ = profile.next_;
    else
      live_profiles_ = profile.next_;
    if (profile.next_ != nullptr)
      profile.next_->previous_ = profile.previous_;
    profile.previous_ = nullptr;
    profile.next_ = nullptr;
  }

  [[nodiscard]] bool standalone_constructed() noexcept {
    if (!executor_pool_profile_enabled())
      return false;
    std::lock_guard lock{mutex_};
    ++standalone_constructions_;
    ++standalone_live_;
    standalone_live_peak_ =
        std::max(standalone_live_peak_, standalone_live_);
    return true;
  }

  void standalone_destroyed() noexcept {
    std::lock_guard lock{mutex_};
    if (standalone_live_ != 0)
      --standalone_live_;
  }

  void report() noexcept {
    if (!executor_pool_profile_enabled())
      return;
    ExecutorPoolProfileSnapshot snapshot;
    {
      std::lock_guard lock{mutex_};
      snapshot.counters = retired_counters_;
      auto sequences = retired_sequences_;
      auto sequence_overflow = retired_sequence_overflow_;
      for (auto *profile = live_profiles_; profile != nullptr;
           profile = profile->next_) {
        merge_counters(snapshot.counters, profile->counters_);
        merge_sequences(*profile, sequences, sequence_overflow);
      }
      for (const auto &entry : sequences) {
        if (entry.sequence == 0)
          continue;
        ++snapshot.sequence_distinct;
        if (entry.wrappers >= 2)
          ++snapshot.sequence_shared;
        snapshot.sequence_extra_wrapper_associations += entry.wrappers - 1U;
        snapshot.sequence_max_fanout =
            std::max(snapshot.sequence_max_fanout, entry.wrappers);
        const auto slot_fanout =
            static_cast<std::uint64_t>(std::popcount(entry.slot_mask));
        if (slot_fanout >= 2)
          ++snapshot.sequence_multislot;
        snapshot.sequence_extra_slot_associations += slot_fanout - 1U;
        snapshot.sequence_max_slot_fanout =
            std::max(snapshot.sequence_max_slot_fanout, slot_fanout);
      }
      snapshot.sequence_global_overflow = sequence_overflow;
      snapshot.standalone_constructions = standalone_constructions_;
      snapshot.standalone_live_peak = standalone_live_peak_;
      if ((reported_ && snapshot == reported_snapshot_) ||
          (snapshot.counters.wrappers == 0 &&
           snapshot.standalone_constructions == 0))
        return;
      reported_snapshot_ = snapshot;
      reported_ = true;
    }

    const auto &value = snapshot.counters;
    JitProfileRecord record;
    record.append(
        "Darwin JIT executor pool final wrappers=%llu borrows=%llu "
        "no-preference=%llu preferred-attempts=%llu preferred-hits=%llu "
        "fallback-claims=%llu cells-probed=%llu failed-cas=%llu "
        "waiter-enqueues=%llu waiter-wakes=%llu fifo-handoffs=%llu "
        "max-waiters=%llu construction-attempts=%llu created=%llu "
        "peak-live=%llu peak-borrowed-observed=%llu slot-changes=%llu "
        "wrappers-slot-changed=%llu max-slot-changes=%llu "
        "null-sequences=%llu sequence-changes=%llu "
        "wrappers-sequence-changed=%llu max-sequence-changes=%llu "
        "same-sequence-different-slot=%llu sequence-distinct=%llu "
        "sequence-shared=%llu sequence-extra-wrapper-associations=%llu "
        "sequence-max-fanout=%llu sequence-multislot=%llu "
        "sequence-extra-slot-associations=%llu "
        "sequence-max-slot-fanout=%llu "
        "sequence-association-overflow=%llu "
        "sequence-global-overflow=%llu standalone-constructions=%llu "
        "standalone-live-peak=%llu",
        static_cast<unsigned long long>(value.wrappers),
        static_cast<unsigned long long>(value.borrows),
        static_cast<unsigned long long>(value.no_preference),
        static_cast<unsigned long long>(value.preferred_attempts),
        static_cast<unsigned long long>(value.preferred_hits),
        static_cast<unsigned long long>(value.fallback_claims),
        static_cast<unsigned long long>(value.cells_probed),
        static_cast<unsigned long long>(value.failed_cas),
        static_cast<unsigned long long>(value.waiter_enqueues),
        static_cast<unsigned long long>(value.waiter_wakes),
        static_cast<unsigned long long>(value.fifo_handoffs),
        static_cast<unsigned long long>(value.max_waiters),
        static_cast<unsigned long long>(value.construction_attempts),
        static_cast<unsigned long long>(value.created),
        static_cast<unsigned long long>(value.peak_live),
        static_cast<unsigned long long>(value.peak_borrowed),
        static_cast<unsigned long long>(value.slot_changes),
        static_cast<unsigned long long>(value.wrappers_slot_changed),
        static_cast<unsigned long long>(value.max_slot_changes),
        static_cast<unsigned long long>(value.null_sequences),
        static_cast<unsigned long long>(value.sequence_changes),
        static_cast<unsigned long long>(value.wrappers_sequence_changed),
        static_cast<unsigned long long>(value.max_sequence_changes),
        static_cast<unsigned long long>(value.same_sequence_different_slot),
        static_cast<unsigned long long>(snapshot.sequence_distinct),
        static_cast<unsigned long long>(snapshot.sequence_shared),
        static_cast<unsigned long long>(
            snapshot.sequence_extra_wrapper_associations),
        static_cast<unsigned long long>(snapshot.sequence_max_fanout),
        static_cast<unsigned long long>(snapshot.sequence_multislot),
        static_cast<unsigned long long>(
            snapshot.sequence_extra_slot_associations),
        static_cast<unsigned long long>(snapshot.sequence_max_slot_fanout),
        static_cast<unsigned long long>(value.sequence_association_overflow),
        static_cast<unsigned long long>(snapshot.sequence_global_overflow),
        static_cast<unsigned long long>(snapshot.standalone_constructions),
        static_cast<unsigned long long>(snapshot.standalone_live_peak));
    record.emit();
  }

private:
  static void merge_sequences(
      const JitExecutorPoolProfile &profile,
      std::array<SequenceFanout, global_sequence_capacity> &sequences,
      std::uint64_t &overflow) noexcept {
    for (std::size_t index = 0; index < profile.sequence_count_; ++index)
      associate_sequence(sequences, overflow,
                         profile.sequences_[index].session,
                         profile.sequences_[index].sequence,
                         profile.sequences_[index].slot_mask);
  }

  std::mutex mutex_;
  JitExecutorPoolProfile *live_profiles_{};
  JitExecutorPoolProfile::Counters retired_counters_{};
  std::array<SequenceFanout, global_sequence_capacity> retired_sequences_{};
  std::uint64_t retired_sequence_overflow_{};
  std::uint64_t standalone_constructions_{};
  std::uint64_t standalone_live_{};
  std::uint64_t standalone_live_peak_{};
  ExecutorPoolProfileSnapshot reported_snapshot_{};
  bool reported_{};
};

namespace {

[[nodiscard]] JitExecutorPoolProfileRegistry &profile_registry() noexcept {
  /* Explicit quiescent facade calls own reporting. Keeping this intentionally
   * leaked also makes late wrapper retirement safe without registering a
   * competing atexit record after the shared-cache terminal reporter.
   */
  static auto *registry = new JitExecutorPoolProfileRegistry;
  return *registry;
}

} // namespace

JitExecutorPoolProfile::JitExecutorPoolProfile(std::uint64_t session) noexcept
    : session_{session}, enabled_{executor_pool_profile_enabled()} {
  if (!enabled_)
    return;
  counters_.wrappers = 1;
  profile_registry().register_wrapper(*this);
}

JitExecutorPoolProfile::~JitExecutorPoolProfile() {
  if (enabled_)
    profile_registry().retire_wrapper(*this);
}

void JitExecutorPoolProfile::associate_sequence(
    std::uintptr_t sequence, unsigned slot) noexcept {
  const auto slot_mask = std::uint64_t{1} << slot;
  for (std::size_t index = 0; index < sequence_count_; ++index) {
    auto &entry = sequences_[index];
    if (entry.session == session_ && entry.sequence == sequence) {
      entry.slot_mask |= slot_mask;
      return;
    }
  }
  if (sequence_count_ == sequences_.size()) {
    ++counters_.sequence_association_overflow;
    return;
  }
  sequences_[sequence_count_++] = {
      .session = session_, .sequence = sequence, .slot_mask = slot_mask};
}

void JitExecutorPoolProfile::observe_sequence(
    const unsigned int *sequence) noexcept {
  if (!enabled_)
    return;
  const auto address = reinterpret_cast<std::uintptr_t>(sequence);
  if (address == 0)
    ++counters_.null_sequences;
  if (observed_sequence_ && address != previous_observed_sequence_) {
    ++counters_.sequence_changes;
    counters_.wrappers_sequence_changed = 1;
    counters_.max_sequence_changes = counters_.sequence_changes;
  }
  observed_sequence_ = true;
  current_sequence_ = address;
  previous_observed_sequence_ = address;
}

void JitExecutorPoolProfile::record_borrow_invocation(
    unsigned preferred_slot) noexcept {
  if (!enabled_)
    return;
  if (preferred_slot < no_preferred_slot)
    ++counters_.preferred_attempts;
  else
    ++counters_.no_preference;
}

void JitExecutorPoolProfile::record_cell_probe(bool claimed) noexcept {
  if (!enabled_)
    return;
  ++counters_.cells_probed;
  counters_.failed_cas += !claimed;
}

void JitExecutorPoolProfile::record_fallback_claim() noexcept {
  if (enabled_)
    ++counters_.fallback_claims;
}

void JitExecutorPoolProfile::record_waiter_enqueue(
    unsigned waiter_count) noexcept {
  if (!enabled_)
    return;
  ++counters_.waiter_enqueues;
  counters_.max_waiters = std::max<std::uint64_t>(
      counters_.max_waiters, waiter_count);
}

void JitExecutorPoolProfile::record_waiter_wake() noexcept {
  if (enabled_)
    ++counters_.waiter_wakes;
}

void JitExecutorPoolProfile::record_fifo_handoff() noexcept {
  if (enabled_)
    ++counters_.fifo_handoffs;
}

void JitExecutorPoolProfile::record_construction_attempt() noexcept {
  if (enabled_)
    ++counters_.construction_attempts;
}

void JitExecutorPoolProfile::record_created(std::size_t live) noexcept {
  if (!enabled_)
    return;
  ++counters_.created;
  counters_.peak_live = std::max<std::uint64_t>(counters_.peak_live, live);
}

void JitExecutorPoolProfile::record_successful_borrow(
    unsigned slot, unsigned preferred_slot,
    std::size_t observed_borrowed) noexcept {
  if (!enabled_)
    return;
  ++counters_.borrows;
  if (preferred_slot < no_preferred_slot && slot == preferred_slot)
    ++counters_.preferred_hits;
  counters_.peak_borrowed = std::max<std::uint64_t>(
      counters_.peak_borrowed, observed_borrowed);
  if (current_sequence_ != 0)
    associate_sequence(current_sequence_, slot);
  if (successful_borrow_ && slot != previous_borrow_slot_) {
    ++counters_.slot_changes;
    counters_.wrappers_slot_changed = 1;
    counters_.max_slot_changes = counters_.slot_changes;
    if (current_sequence_ != 0 &&
        current_sequence_ == previous_borrow_sequence_)
      ++counters_.same_sequence_different_slot;
  }
  successful_borrow_ = true;
  previous_borrow_slot_ = slot;
  previous_borrow_sequence_ = current_sequence_;
}

bool profile_jit_standalone_executor_constructed() noexcept {
  if (!executor_pool_profile_enabled())
    return false;
  return profile_registry().standalone_constructed();
}

void profile_jit_standalone_executor_destroyed() noexcept {
  profile_registry().standalone_destroyed();
}

void report_final_jit_executor_pool_profile() noexcept {
  if (!executor_pool_profile_enabled())
    return;
  profile_registry().report();
}

} // namespace uml::cpu::arm64

#endif
