// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if UML_ENABLE_JIT_PROFILE

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64 {

class JitExecutorPoolProfileRegistry;

/* Profiling-build-only, wrapper-local pool trace. The guest-entry path writes
 * only these ordinary fields. Registration, retirement and final aggregation
 * use the process registry mutex, and final aggregation is valid only at the
 * existing quiescent reporting boundary.
 */
class JitExecutorPoolProfile final {
public:
  explicit JitExecutorPoolProfile(std::uint64_t session) noexcept;
  ~JitExecutorPoolProfile();

  JitExecutorPoolProfile(const JitExecutorPoolProfile &) = delete;
  JitExecutorPoolProfile &operator=(const JitExecutorPoolProfile &) = delete;

  [[nodiscard]] bool enabled() const noexcept { return enabled_; }

  void observe_sequence(const unsigned int *sequence) noexcept;
  void record_borrow_invocation(unsigned preferred_slot) noexcept;
  void record_cell_probe(bool claimed) noexcept;
  void record_fallback_claim() noexcept;
  void record_waiter_enqueue(unsigned waiter_count) noexcept;
  void record_waiter_wake() noexcept;
  void record_fifo_handoff() noexcept;
  void record_construction_attempt() noexcept;
  void record_created(std::size_t live) noexcept;
  void record_successful_borrow(unsigned slot, unsigned preferred_slot,
                                std::size_t observed_borrowed) noexcept;

private:
  friend class JitExecutorPoolProfileRegistry;

  static constexpr std::size_t sequence_capacity = 64;
  static constexpr unsigned no_preferred_slot = 64;

public:
  /* Registry value type, exposed only inside this CPU-private profiling
   * header so bounded snapshot helpers can merge it without virtual state.
   */
  struct Counters final {
    std::uint64_t wrappers{};
    std::uint64_t borrows{};
    std::uint64_t no_preference{};
    std::uint64_t preferred_attempts{};
    std::uint64_t preferred_hits{};
    std::uint64_t fallback_claims{};
    std::uint64_t cells_probed{};
    std::uint64_t failed_cas{};
    std::uint64_t waiter_enqueues{};
    std::uint64_t waiter_wakes{};
    std::uint64_t fifo_handoffs{};
    std::uint64_t max_waiters{};
    std::uint64_t construction_attempts{};
    std::uint64_t created{};
    std::uint64_t peak_live{};
    std::uint64_t peak_borrowed{};
    std::uint64_t slot_changes{};
    std::uint64_t wrappers_slot_changed{};
    std::uint64_t max_slot_changes{};
    std::uint64_t null_sequences{};
    std::uint64_t sequence_changes{};
    std::uint64_t wrappers_sequence_changed{};
    std::uint64_t max_sequence_changes{};
    std::uint64_t same_sequence_different_slot{};
    std::uint64_t sequence_association_overflow{};

    [[nodiscard]] bool operator==(const Counters &) const noexcept = default;
  };

private:
  struct SequenceAssociation final {
    std::uint64_t session{};
    std::uintptr_t sequence{};
    std::uint64_t slot_mask{};
  };

  void associate_sequence(std::uintptr_t sequence, unsigned slot) noexcept;

  Counters counters_{};
  std::array<SequenceAssociation, sequence_capacity> sequences_{};
  std::size_t sequence_count_{};
  std::uint64_t session_{};
  std::uintptr_t current_sequence_{};
  std::uintptr_t previous_observed_sequence_{};
  std::uintptr_t previous_borrow_sequence_{};
  unsigned previous_borrow_slot_{no_preferred_slot};
  bool observed_sequence_{};
  bool successful_borrow_{};
  bool enabled_{};
  JitExecutorPoolProfile *previous_{};
  JitExecutorPoolProfile *next_{};
};

/* Cold standalone lease lifecycle accounting. The construction function
 * returns whether a matching destruction must be reported.
 */
[[nodiscard]] bool profile_jit_standalone_executor_constructed() noexcept;
void profile_jit_standalone_executor_destroyed() noexcept;

/* Emit a cumulative record only when its bounded snapshot changed. */
void report_final_jit_executor_pool_profile() noexcept;

} // namespace uml::cpu::arm64

#endif
