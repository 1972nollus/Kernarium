// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_publication.hpp"

#include <algorithm>

namespace uml::cpu::arm64 {

std::atomic<unsigned> active_jit_reservations{};
std::mutex jit_store_participants_mutex;
std::vector<JitStoreParticipant *> jit_store_participants;
alignas(64) std::atomic<unsigned> jit_exclusive_operation_phase{};
alignas(64) std::atomic<unsigned> jit_waiting_store_count_value{};
alignas(64) std::atomic<bool> jit_exclusive_operation_turn{};
alignas(64) std::atomic<unsigned> jit_exclusive_operation_count{};
std::mutex jit_reservations_mutex;
std::unordered_set<CpuState *> jit_reservations;
thread_local JitStoreParticipant *current_jit_store_participant{};

bool has_published_jit_store(
    const JitStoreParticipant *excluded) noexcept {
  std::scoped_lock lock{jit_store_participants_mutex};
  return std::ranges::any_of(
      jit_store_participants,
      [excluded](const JitStoreParticipant *participant) {
        return participant != excluded &&
               participant->state.load(std::memory_order_seq_cst) !=
                   JitStoreState::idle;
      });
}

JitExclusiveOperationScope::JitExclusiveOperationScope() noexcept {
  jit_exclusive_operation_count.fetch_add(1, std::memory_order_seq_cst);

  bool expected = false;
  while (!jit_exclusive_operation_turn.compare_exchange_weak(
      expected, true, std::memory_order_acquire,
      std::memory_order_relaxed)) {
    if (!jit_gate_value_changed(jit_exclusive_operation_turn, true))
      jit_exclusive_operation_turn.wait(true, std::memory_order_relaxed);
    expected = false;
  }

  auto waiting =
      jit_waiting_store_count_value.load(std::memory_order_seq_cst);
  while (waiting != 0U) {
    if (!jit_gate_value_changed(jit_waiting_store_count_value, waiting))
      jit_waiting_store_count_value.wait(waiting,
                                         std::memory_order_seq_cst);
    waiting =
        jit_waiting_store_count_value.load(std::memory_order_seq_cst);
  }
  jit_exclusive_operation_phase.store(jit_exclusive_operation_active,
                                      std::memory_order_seq_cst);
  while (has_published_jit_store(current_jit_store_participant))
    __asm__ volatile("yield");
}

JitExclusiveOperationScope::~JitExclusiveOperationScope() {
  jit_exclusive_operation_phase.store(0U, std::memory_order_seq_cst);
  jit_exclusive_operation_phase.notify_all();
  jit_exclusive_operation_turn.store(false, std::memory_order_release);
  jit_exclusive_operation_turn.notify_one();
  jit_exclusive_operation_count.fetch_sub(1, std::memory_order_seq_cst);
}

void register_jit_store_participant(JitStoreParticipant &participant) {
  std::scoped_lock lock{jit_store_participants_mutex};
  /* Implementations register exactly once in their constructor. Treat a
   * repeated registration as the same lifetime instead of inserting a second
   * raw pointer which a single destructor could leave stale. */
  if (std::ranges::find(jit_store_participants, &participant) !=
      jit_store_participants.end())
    return;
  jit_store_participants.push_back(&participant);
}

void unregister_jit_store_participant(JitStoreParticipant &participant) {
  std::scoped_lock lock{jit_store_participants_mutex};
  /* The scan takes this same mutex, so erasing the sole entry waits for any
   * in-flight scan and no later scan can observe the participant. A repeated
   * unregister therefore has no lifetime work left to perform. */
  const auto found =
      std::ranges::find(jit_store_participants, &participant);
  if (found == jit_store_participants.end())
    return;
  jit_store_participants.erase(found);
}

bool begin_jit_reservation(CpuState &cpu) {
  bool inserted = false;
  {
    std::scoped_lock lock{jit_reservations_mutex};
    inserted = jit_reservations.insert(&cpu).second;
    if (inserted) {
      active_jit_reservations.fetch_add(1, std::memory_order_seq_cst);
      if (current_jit_store_participant != nullptr)
        current_jit_store_participant->reservation_cpu.store(
            &cpu, std::memory_order_seq_cst);
    }
  }
  return inserted;
}

void end_jit_reservation(CpuState &cpu) noexcept {
  std::scoped_lock lock{jit_reservations_mutex};
  if (jit_reservations.erase(&cpu) != 0)
    active_jit_reservations.fetch_sub(1, std::memory_order_seq_cst);
}

} // namespace uml::cpu::arm64
