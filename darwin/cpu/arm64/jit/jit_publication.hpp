// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"

#include <atomic>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace uml::cpu::arm64 {

enum class JitStoreState : unsigned {
  idle,
  checking,
  direct,
  tracked,
};

struct JitStoreParticipant final {
  alignas(64) std::atomic<JitStoreState> state{JitStoreState::idle};
  std::atomic<CpuState *> reservation_cpu{};
};

extern std::atomic<unsigned> active_jit_reservations;
extern std::mutex jit_store_participants_mutex;
extern std::vector<JitStoreParticipant *> jit_store_participants;
/*
 * Only the current reservation helper closes the store phase. Queued helpers
 * wait outside that phase, and stores parked by the current helper are allowed
 * to publish and drain before the next helper can enter. This avoids both the
 * old spinning-store cost and the reader/writer-lock convoy in which queued
 * writers kept ordinary stores asleep across several helper turns.
 *
 * The uncontended direct-store path remains lock-free: it publishes its
 * participant state, reads the phase once, and proceeds. Stores already in
 * flight when a helper announces itself are covered by the participant scan.
 */
alignas(64) extern std::atomic<unsigned> jit_exclusive_operation_phase;
alignas(64) extern std::atomic<unsigned> jit_waiting_store_count_value;
alignas(64) extern std::atomic<bool> jit_exclusive_operation_turn;
constexpr unsigned jit_exclusive_operation_active = 1U;
constexpr unsigned jit_gate_spin_iterations = 64U;
[[nodiscard]] constexpr bool
jit_exclusive_operation_is_active(unsigned phase) noexcept {
  return phase != 0U;
}
template <typename T>
[[nodiscard]] bool jit_gate_value_changed(
    const std::atomic<T> &value, T expected) noexcept {
  for (unsigned iteration = 0; iteration < jit_gate_spin_iterations;
       ++iteration) {
    if (value.load(std::memory_order_relaxed) != expected)
      return true;
    __asm__ volatile("yield");
  }
  return value.load(std::memory_order_relaxed) != expected;
}
alignas(64) extern std::atomic<unsigned> jit_exclusive_operation_count;
[[nodiscard]] inline unsigned
jit_waiting_store_count(unsigned) noexcept {
  return jit_waiting_store_count_value.load(std::memory_order_seq_cst);
}
extern std::mutex jit_reservations_mutex;
extern std::unordered_set<CpuState *> jit_reservations;
extern thread_local JitStoreParticipant *current_jit_store_participant;

[[nodiscard]] bool has_published_jit_store(
    const JitStoreParticipant *excluded) noexcept;

class JitExclusiveOperationScope final {
public:
  JitExclusiveOperationScope() noexcept;

  JitExclusiveOperationScope(const JitExclusiveOperationScope &) = delete;
  JitExclusiveOperationScope &
  operator=(const JitExclusiveOperationScope &) = delete;

  ~JitExclusiveOperationScope();
};

class JitBlockPublication final {
public:
  [[gnu::always_inline]] explicit JitBlockPublication(
      JitStoreParticipant &participant, const CpuState &cpu) noexcept
      : participant_{participant} {
    current_jit_store_participant = &participant_;
    participant_.state.store(JitStoreState::checking,
                             std::memory_order_seq_cst);
    auto phase =
        jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
    if (jit_exclusive_operation_is_active(phase)) {
      participant_.state.store(JitStoreState::idle,
                               std::memory_order_seq_cst);
      waiting_ = true;
      jit_waiting_store_count_value.fetch_add(1,
                                               std::memory_order_seq_cst);
      do {
        while (jit_exclusive_operation_is_active(phase)) {
          if (!jit_gate_value_changed(jit_exclusive_operation_phase,
                                      phase))
            jit_exclusive_operation_phase.wait(
                phase, std::memory_order_seq_cst);
          phase =
              jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
        }
        participant_.state.store(JitStoreState::checking,
                                 std::memory_order_seq_cst);
        phase =
            jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
        if (jit_exclusive_operation_is_active(phase)) {
          participant_.state.store(JitStoreState::idle,
                                   std::memory_order_seq_cst);
        }
      } while (jit_exclusive_operation_is_active(phase));
    }
    const auto active =
        active_jit_reservations.load(std::memory_order_seq_cst);
    tracked_ =
        active != 0 &&
        (active != 1 ||
         participant_.reservation_cpu.load(std::memory_order_seq_cst) !=
             &cpu);
    participant_.state.store(tracked_ ? JitStoreState::tracked
                                      : JitStoreState::direct,
                             std::memory_order_seq_cst);
    if (waiting_) {
      jit_waiting_store_count_value.fetch_sub(1,
                                               std::memory_order_seq_cst);
      jit_waiting_store_count_value.notify_all();
    }
  }

  JitBlockPublication(const JitBlockPublication &) = delete;
  JitBlockPublication &operator=(const JitBlockPublication &) = delete;

  [[gnu::always_inline]] ~JitBlockPublication() {
    participant_.state.store(JitStoreState::idle, std::memory_order_seq_cst);
    current_jit_store_participant = nullptr;
  }

  [[nodiscard, gnu::always_inline]] bool needs_tracking() const noexcept {
    return tracked_;
  }

private:
  JitStoreParticipant &participant_;
  bool waiting_{};
  bool tracked_{};
};

void register_jit_store_participant(JitStoreParticipant &participant);
void unregister_jit_store_participant(JitStoreParticipant &participant);
[[nodiscard]] bool begin_jit_reservation(CpuState &cpu);
void end_jit_reservation(CpuState &cpu) noexcept;

} // namespace uml::cpu::arm64
