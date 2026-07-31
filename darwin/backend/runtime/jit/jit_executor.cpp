// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_executor.hpp"
#include "jit_compiler_families.hpp"
#include "jit_test_api.hpp"

#include "cpu/arm64/detail/bfloat_dot.hpp"
#include "cpu/arm64/detail/bfloat_matrix.hpp"
#include "cpu/arm64/detail/bfloat_multiply.hpp"
#include "cpu/arm64/detail/atomic_floating.hpp"
#include "cpu/arm64/detail/fp8.hpp"
#include "cpu/arm64/detail/floating_arithmetic.hpp"
#include "cpu/arm64/detail/floating_complex.hpp"
#include "cpu/arm64/detail/floating_compare.hpp"
#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/detail/floating_fused.hpp"
#include "cpu/arm64/detail/floating_horizontal.hpp"
#include "cpu/arm64/detail/floating_integer_conversion.hpp"
#include "cpu/arm64/detail/floating_reciprocal.hpp"
#include "cpu/arm64/detail/floating_round.hpp"
#include "cpu/arm64/detail/floating_scale.hpp"
#include "cpu/arm64/detail/fp_state_access.hpp"
#include "cpu/arm64/detail/gcs.hpp"
#include "cpu/arm64/detail/half_dot.hpp"
#include "cpu/arm64/detail/half_fused_long.hpp"
#include "cpu/arm64/detail/half_matrix.hpp"
#include "cpu/arm64/detail/instruction_helpers.hpp"
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/tlb.hpp"
#include "runtime/host_features.hpp"
#include "uml_darwin_runtime.h"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#if UML_JIT_PUBLICATION_GATE_TESTING
#include <chrono>
#include <semaphore>
#endif
#include <span>
#include <sys/mman.h>
#if UML_JIT_PUBLICATION_GATE_TESTING
#include <thread>
#endif
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <unistd.h>

#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif

extern "C" std::uint32_t uml_arm64_gadget_simd_structure_memory_transfer(
    uml::backend::arm64::CpuState *cpu,
    uml::backend::arm64::gadgets::Tlb *tlb, std::uint64_t tagged_address,
    void *data, std::size_t size, std::uint32_t access, std::uint32_t base_sp,
    std::uint32_t options) noexcept;

extern "C" void uml_arm64_jit_invalidate_store_ranges(
    const std::uint64_t *ranges, std::size_t count) noexcept;

extern "C" void
uml_arm64_guest_code_synchronize(std::uint64_t address) noexcept;

namespace uml::backend {
namespace {

using arm64::CpuState;
using arm64::gadgets::Access;
using arm64::gadgets::Event;
using arm64::gadgets::Tlb;

constexpr std::size_t maximum_block_instructions = 64;
constexpr std::size_t maximum_chain_instructions = 4096;
/*
 * Translation-time only: generated programs contain either the original
 * memory-backed chain ABI or the persistent x19-x21 ABI, never a runtime
 * mode branch. Flip this one default to isolate rollback of the experiment.
 */
constexpr bool persistent_linked_block_abi_default = true;
constexpr std::size_t cache_ways = 4;
constexpr std::size_t cache_sets = 8192;
constexpr std::size_t maximum_block_links = cache_sets * cache_ways * 2;
constexpr std::size_t maximum_indirect_links = maximum_block_links;
constexpr std::size_t arena_size = 64U * 1024U * 1024U;
constexpr std::size_t tracked_store_arena_size = 32U * 1024U * 1024U;
constexpr std::size_t floating_transaction_arena_size = 16U * 1024U;
constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr unsigned page_shift = DARWIN_UML_GUEST_PAGE_SHIFT;
constexpr std::uint32_t native_fpsr_mutable_mask = 0x0800'009fU;

[[nodiscard]] constexpr bool
floating_or_vector_primary_class(std::uint32_t instruction) noexcept {
  const auto op0 = (instruction >> 25U) & 15U;
  return op0 == 7U || op0 == 15U;
}

[[nodiscard]] constexpr bool
obvious_translated_block_terminator(std::uint32_t instruction) noexcept {
  return (instruction & 0x7c00'0000U) == 0x1400'0000U ||
         (instruction & 0xff00'0000U) == 0x5400'0000U ||
         (instruction & 0x7e00'0000U) == 0x3400'0000U ||
         (instruction & 0x7e00'0000U) == 0x3600'0000U ||
         (instruction & 0xfe00'0000U) == 0xd600'0000U ||
         (instruction & 0xff00'0000U) == 0xd400'0000U;
}

static_assert(floating_or_vector_primary_class(0x6f40'0000U));
static_assert(floating_or_vector_primary_class(0x4f00'1000U));
static_assert(!floating_or_vector_primary_class(0x8b02'0020U));
static_assert(!floating_or_vector_primary_class(0xf940'0000U));
static_assert(!floating_or_vector_primary_class(0x5400'0000U));
static_assert(!floating_or_vector_primary_class(0xd400'0001U));
static_assert(obvious_translated_block_terminator(0x1400'0000U));
static_assert(obvious_translated_block_terminator(0x9400'0000U));
static_assert(obvious_translated_block_terminator(0x5400'0000U));
static_assert(obvious_translated_block_terminator(0xb400'0000U));
static_assert(obvious_translated_block_terminator(0x3600'0000U));
static_assert(obvious_translated_block_terminator(0xd61f'0000U));
static_assert(obvious_translated_block_terminator(0xd63f'0000U));
static_assert(obvious_translated_block_terminator(0xd65f'0000U));
static_assert(obvious_translated_block_terminator(0xd400'0001U));
static_assert(!obvious_translated_block_terminator(0xd503'201fU));
static_assert(!obvious_translated_block_terminator(0x6f40'0000U));

[[nodiscard]] constexpr bool
terminal_does_not_retire(Event event) noexcept {
  return event == Event::memory_fault || event == Event::illegal_instruction ||
         event == Event::gcs_fault ||
         event == Event::floating_point_trap || event == Event::fatal;
}

[[nodiscard]] constexpr std::uint64_t
pack_jit_result(Event event, std::size_t retired) noexcept {
  return static_cast<std::uint32_t>(event) |
         (static_cast<std::uint64_t>(retired) << 32U);
}

struct JitBarrierLowering final {
  std::array<std::uint32_t, 2> words{};
  std::size_t count{};
};

[[nodiscard]] constexpr JitBarrierLowering
lower_memory_barrier(std::uint32_t instruction, bool native_sb,
                     bool native_xs) noexcept {
  constexpr std::uint32_t dsb_base = 0xd503'309fU;
  constexpr std::uint32_t isb_sy = 0xd503'3fdfU;
  constexpr std::uint32_t sb = 0xd503'30ffU;
  constexpr std::uint32_t dsb_nxs = 0xd503'323fU;

  if (instruction == sb) {
    if (native_sb)
      return {{{instruction, 0}}, 1};
    return {{{dsb_base | (15U << 8U), isb_sy}}, 2};
  }
  if ((instruction & 0xffff'f3ffU) == dsb_nxs) {
    if (native_xs)
      return {{{instruction, 0}}, 1};
    constexpr std::array<unsigned, 4> full_barrier_options{3, 7, 11, 15};
    const auto option = full_barrier_options[(instruction >> 10U) & 3U];
    return {{{dsb_base | (option << 8U), 0}}, 1};
  }
  return {{{instruction, 0}}, 1};
}

struct JitStoreRange final {
  /* The tracked-only program records the translated backing address so
   * distinct guest virtual aliases share one reservation identity.
   */
  std::uint64_t address;
  std::uint64_t size;
};

enum class JitLinkCapability : std::uint8_t {
  store_publication = 1U << 0U,
  fp_state_materialized = 1U << 1U,
  native_fp_environment = 1U << 2U,
  native_fpmr_environment = 1U << 3U,
};

[[nodiscard]] constexpr std::uint8_t
link_capability(JitLinkCapability capability) noexcept {
  return static_cast<std::uint8_t>(capability);
}

constexpr std::array<std::uint8_t, 10> jit_link_selector_capabilities{
    0,
    link_capability(JitLinkCapability::store_publication),
    link_capability(JitLinkCapability::fp_state_materialized),
    link_capability(JitLinkCapability::store_publication) |
        link_capability(JitLinkCapability::fp_state_materialized),
    link_capability(JitLinkCapability::fp_state_materialized) |
        link_capability(JitLinkCapability::native_fp_environment),
    link_capability(JitLinkCapability::store_publication) |
        link_capability(JitLinkCapability::fp_state_materialized) |
        link_capability(JitLinkCapability::native_fp_environment),
    link_capability(JitLinkCapability::fp_state_materialized) |
        link_capability(JitLinkCapability::native_fpmr_environment),
    link_capability(JitLinkCapability::store_publication) |
        link_capability(JitLinkCapability::fp_state_materialized) |
        link_capability(JitLinkCapability::native_fpmr_environment),
    link_capability(JitLinkCapability::fp_state_materialized) |
        link_capability(JitLinkCapability::native_fp_environment) |
        link_capability(JitLinkCapability::native_fpmr_environment),
    link_capability(JitLinkCapability::store_publication) |
        link_capability(JitLinkCapability::fp_state_materialized) |
        link_capability(JitLinkCapability::native_fp_environment) |
        link_capability(JitLinkCapability::native_fpmr_environment),
};
constexpr std::size_t jit_link_selector_count =
    jit_link_selector_capabilities.size();
constexpr std::uint8_t invalid_link_selector = 0xffU;

[[nodiscard]] constexpr bool
valid_link_capabilities(std::uint8_t capabilities) noexcept {
  const auto materialized =
      link_capability(JitLinkCapability::fp_state_materialized);
  const auto native =
      link_capability(JitLinkCapability::native_fp_environment) |
      link_capability(JitLinkCapability::native_fpmr_environment);
  return (capabilities & native) == 0U ||
         (capabilities & materialized) != 0U;
}

[[nodiscard]] constexpr std::uint8_t
link_selector_for_capabilities(std::uint8_t capabilities) noexcept {
  for (std::size_t selector = 0;
       selector < jit_link_selector_capabilities.size(); ++selector)
    if (jit_link_selector_capabilities[selector] == capabilities)
      return static_cast<std::uint8_t>(selector);
  return invalid_link_selector;
}

[[nodiscard]] std::uint8_t
checked_link_selector(std::uint8_t capabilities) noexcept {
  const auto selector = link_selector_for_capabilities(capabilities);
  if (selector == invalid_link_selector)
    std::abort();
  return selector;
}

[[nodiscard]] constexpr std::uint8_t
link_state_requirements(bool accesses_fp_state, bool native_fp,
                        bool native_fpmr) noexcept {
  std::uint8_t requirements = 0;
  if (accesses_fp_state || native_fp || native_fpmr)
    requirements |=
        link_capability(JitLinkCapability::fp_state_materialized);
  if (native_fp)
    requirements |=
        link_capability(JitLinkCapability::native_fp_environment);
  if (native_fpmr)
    requirements |=
        link_capability(JitLinkCapability::native_fpmr_environment);
  return requirements;
}

[[nodiscard]] constexpr bool
valid_link_state_metadata(bool accesses_fp_state, bool native_fp,
                          bool native_fpmr) noexcept {
  return accesses_fp_state || (!native_fp && !native_fpmr);
}

struct JitChainState final {
  /*
   * Instructions still available before the current translated block. The
   * persistent ABI keeps the same value in x20 and publishes it on return.
   */
  std::uint64_t remaining_instructions{maximum_chain_instructions};
  JitStoreRange *stores{};
  void *indirect_miss{};
  std::uint8_t link_selector{};
  /*
   * A helper-sensitive cold path cannot run while the outer direct-store
   * publication is alive. Generated code sets this byte and returns without
   * retiring the instruction; run_block performs the semantic instruction
   * after the publication scope has ended.
   */
  std::uint8_t deferred_semantic_instruction{};
  std::uint64_t indirect_source_pc{};
};

struct alignas(128) JitBlockLink final {
  /*
   * Generated code uses raw internal-entry branch targets.  The initial C++
   * call owns the one native stack frame for the complete chain; linked
   * successors enter after their ordinary external prologue and preserve that
   * frame until a cold, terminal, or fairness exit unwinds it exactly once.
   *
   * The host-owned selector describes setup performed by the initial C++
   * entry. A zero cell returns to C++ before a successor whose store or
   * architectural-state requirements have not been established.
   *
   * The ten reachable setup combinations and their metadata fit in one
   * 128-byte record. Generated code points at entries and reads the preceding
   * metadata with one aligned pair load.
   */
  std::uint64_t guest_pc{};
  std::uint64_t instruction_count{};
  std::array<std::uintptr_t, jit_link_selector_count> entries{};
  std::uint64_t activation_epoch{};
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  std::uint32_t profile_read_before_write{};
  bool profile_mask_valid{};
#endif
};

struct JitIndirectLinkEntry final {
  std::uint64_t guest_pc{};
  const std::uintptr_t *link_entries{};
};

struct JitIndirectLink final {
  std::array<JitIndirectLinkEntry, 2> entries{};
  std::uint64_t source_pc{};
  std::uint64_t activation_epoch{};
  std::uint8_t next_replacement{};
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  /*
   * Generated PIC data remains exactly {guest PC, entry-table pointer}.
   * Diagnostic attribution retains the owning records out of that hot array.
   */
  std::array<const JitBlockLink *, 2> profile_targets{};
#endif
};

/*
 * Generated code embeds pointers to JitBlockLink entry arrays, so records must
 * not move or be reclaimed while the code arena is live. Bound the stable
 * storage instead: once full, newly compiled branches use the existing
 * unlinked exit path.
 */
class JitBlockLinkTable final {
public:
  explicit JitBlockLinkTable(std::size_t capacity) : capacity_{capacity} {
    entries_.reserve(capacity);
  }

  [[nodiscard]] JitBlockLink *find_or_create(std::uint64_t pc) {
    if (const auto found = entries_.find(pc); found != entries_.end())
      return found->second.get();
    if (entries_.size() >= capacity_)
      return nullptr;
    auto entry = std::make_unique<JitBlockLink>();
    entry->guest_pc = pc;
    auto *result = entry.get();
    entries_.emplace(pc, std::move(entry));
    return result;
  }

  void clear() noexcept { entries_.clear(); }

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
  std::size_t capacity_{};
  std::unordered_map<std::uint64_t, std::unique_ptr<JitBlockLink>> entries_;
};

/*
 * Generated indirect exits embed pointers to these cells. Keep every cell
 * stable for the code arena's lifetime and bound their total number. Guest
 * targets are comparison data only; entries point exclusively to host-owned
 * JitBlockLink entry arrays, never to native code supplied by the guest.
 */
class JitIndirectLinkTable final {
public:
  JitIndirectLinkTable(std::size_t capacity, const JitBlockLink *empty_link)
      : capacity_{capacity}, empty_link_{empty_link} {
    entries_.reserve(capacity);
  }

  [[nodiscard]] JitIndirectLink *find_or_create(std::uint64_t source_pc) {
    if (const auto found = entries_.find(source_pc); found != entries_.end())
      return found->second.get();
    if (entries_.size() >= capacity_)
      return nullptr;
    auto entry = std::make_unique<JitIndirectLink>();
    reset(*entry);
    entry->source_pc = source_pc;
    auto *result = entry.get();
    entries_.emplace(source_pc, std::move(entry));
    return result;
  }

  [[nodiscard]] JitIndirectLink *find(std::uint64_t source_pc) const noexcept {
    const auto found = entries_.find(source_pc);
    return found == entries_.end() ? nullptr : found->second.get();
  }

  void reset(JitIndirectLink &site) const noexcept {
    /*
     * One is not an aligned A64 instruction address. The non-null empty link
     * also makes even a deliberately invalid target a safe C++ fallback.
     */
    for (auto &entry : site.entries) {
      entry.guest_pc = 1;
      entry.link_entries = empty_link_->entries.data();
    }
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    site.profile_targets.fill(empty_link_);
#endif
    site.next_replacement = 0;
    site.activation_epoch = 0;
  }

  void clear() noexcept { entries_.clear(); }

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
  std::size_t capacity_{};
  const JitBlockLink *empty_link_{};
  std::unordered_map<std::uint64_t, std::unique_ptr<JitIndirectLink>> entries_;
};

static_assert(offsetof(JitChainState, remaining_instructions) == 0);
static_assert(offsetof(JitChainState, stores) == 8);
static_assert(offsetof(JitChainState, indirect_miss) == 16);
static_assert(offsetof(JitChainState, link_selector) == 24);
static_assert(offsetof(JitChainState, indirect_source_pc) == 32);
static_assert(sizeof(JitChainState) == 40);
static_assert(sizeof(JitStoreRange) == 16);
static_assert(sizeof(JitChainState::link_selector) == 1);
static_assert(
    link_capability(JitLinkCapability::store_publication) == 1U);
static_assert(jit_link_selector_count == 10);
static_assert(valid_link_capabilities(0));
static_assert(valid_link_capabilities(1));
static_assert(valid_link_capabilities(2));
static_assert(valid_link_capabilities(3));
static_assert(valid_link_capabilities(6));
static_assert(valid_link_capabilities(7));
static_assert(valid_link_capabilities(10));
static_assert(valid_link_capabilities(11));
static_assert(valid_link_capabilities(14));
static_assert(valid_link_capabilities(15));
static_assert(link_selector_for_capabilities(0) == 0);
static_assert(link_selector_for_capabilities(15) == 9);
static_assert(link_selector_for_capabilities(4) ==
              invalid_link_selector);
static_assert(alignof(JitBlockLink) == 128);
static_assert(sizeof(JitBlockLink) == 128);
static_assert(offsetof(JitBlockLink, guest_pc) == 0);
static_assert(offsetof(JitBlockLink, instruction_count) == 8);
static_assert(offsetof(JitBlockLink, entries) == 16);
static_assert(sizeof(JitBlockLink::entries) ==
              jit_link_selector_count * sizeof(std::uintptr_t));
static_assert(sizeof(std::uintptr_t) == 8);
static_assert(offsetof(JitBlockLink, activation_epoch) == 96);
static_assert(offsetof(JitIndirectLinkEntry, guest_pc) == 0);
static_assert(offsetof(JitIndirectLinkEntry, link_entries) == 8);
static_assert(sizeof(JitIndirectLinkEntry) == 16);
static_assert(offsetof(JitIndirectLink, entries) == 0);
static_assert(offsetof(JitIndirectLink, source_pc) == 32);

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

std::atomic<unsigned> active_jit_reservations{};
std::mutex jit_store_participants_mutex;
std::vector<JitStoreParticipant *> jit_store_participants;
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
alignas(64) std::atomic<unsigned> jit_exclusive_operation_phase{};
alignas(64) std::atomic<unsigned> jit_waiting_store_count_value{};
alignas(64) std::atomic<bool> jit_exclusive_operation_turn{};
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
#if UML_JIT_PUBLICATION_GATE_TESTING
alignas(64) std::atomic<unsigned> jit_exclusive_operation_count{};
[[nodiscard]] unsigned
jit_waiting_store_count(unsigned) noexcept {
  return jit_waiting_store_count_value.load(std::memory_order_seq_cst);
}
#endif
std::mutex jit_reservations_mutex;
std::unordered_set<CpuState *> jit_reservations;
thread_local JitStoreParticipant *current_jit_store_participant;

[[nodiscard]] bool has_published_jit_store(
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

class JitExclusiveOperationScope final {
public:
  JitExclusiveOperationScope() noexcept {
#if UML_JIT_PUBLICATION_GATE_TESTING
    jit_exclusive_operation_count.fetch_add(1, std::memory_order_seq_cst);
#endif

    bool expected = false;
    while (!jit_exclusive_operation_turn.compare_exchange_weak(
        expected, true, std::memory_order_acquire,
        std::memory_order_relaxed)) {
      if (!jit_gate_value_changed(jit_exclusive_operation_turn, true))
        jit_exclusive_operation_turn.wait(true,
                                          std::memory_order_relaxed);
      expected = false;
    }

    /*
     * Stores woken by the previous helper retain their waiter count until
     * they have republished. Once the count reaches zero, either their
     * participant state is visible to the scan or they have completed.
     */
    auto waiting =
        jit_waiting_store_count_value.load(std::memory_order_seq_cst);
    while (waiting != 0U) {
      if (!jit_gate_value_changed(jit_waiting_store_count_value,
                                  waiting))
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

  JitExclusiveOperationScope(const JitExclusiveOperationScope &) = delete;
  JitExclusiveOperationScope &
  operator=(const JitExclusiveOperationScope &) = delete;

  ~JitExclusiveOperationScope() {
    jit_exclusive_operation_phase.store(0U, std::memory_order_seq_cst);
    jit_exclusive_operation_phase.notify_all();
    jit_exclusive_operation_turn.store(false, std::memory_order_release);
    jit_exclusive_operation_turn.notify_one();
#if UML_JIT_PUBLICATION_GATE_TESTING
    jit_exclusive_operation_count.fetch_sub(1, std::memory_order_seq_cst);
#endif
  }
};

class JitBlockPublication final {
public:
  explicit JitBlockPublication(JitStoreParticipant &participant,
                               const CpuState &cpu) noexcept
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

  ~JitBlockPublication() {
    participant_.state.store(JitStoreState::idle, std::memory_order_seq_cst);
    current_jit_store_participant = nullptr;
  }

  [[nodiscard]] bool needs_tracking() const noexcept { return tracked_; }

private:
  JitStoreParticipant &participant_;
  bool waiting_{};
  bool tracked_{};
};

void register_jit_store_participant(JitStoreParticipant &participant) {
  std::scoped_lock lock{jit_store_participants_mutex};
  if (std::ranges::find(jit_store_participants, &participant) !=
      jit_store_participants.end())
    std::abort();
  jit_store_participants.push_back(&participant);
}

void unregister_jit_store_participant(JitStoreParticipant &participant) {
  std::scoped_lock lock{jit_store_participants_mutex};
  const auto found =
      std::ranges::find(jit_store_participants, &participant);
  if (found == jit_store_participants.end())
    std::abort();
  jit_store_participants.erase(found);
}

[[nodiscard]] bool begin_jit_reservation(CpuState &cpu) {
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

[[nodiscard]] constexpr std::int64_t sign_extend(std::uint64_t value,
                                                 unsigned bits) noexcept {
  const auto shift = 64U - bits;
  return static_cast<std::int64_t>(value << shift) >> shift;
}

[[nodiscard]] bool verification_configured() noexcept {
  static const bool enabled = [] {
    const auto *value = std::getenv("UML_VERIFY_JIT");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

[[nodiscard]] bool
verification_address_enabled(std::uint64_t pc) noexcept {
  static const std::uint64_t minimum = [] {
    const auto *value = std::getenv("UML_VERIFY_JIT_MIN_PC");
    return value == nullptr ? std::uint64_t{}
                            : std::strtoull(value, nullptr, 0);
  }();
  static const std::uint64_t maximum = [] {
    const auto *value = std::getenv("UML_VERIFY_JIT_MAX_PC");
    return value == nullptr ? ~std::uint64_t{}
                            : std::strtoull(value, nullptr, 0);
  }();
  return pc >= minimum && pc <= maximum;
}

[[nodiscard]] bool
verification_replay_safe(std::uint32_t instruction) noexcept {
  /*
   * The reference pass executes before the translated pass. Any memory read
   * may legitimately observe a different value when another guest thread
   * changes the location between those passes, and an overlay cannot snapshot
   * the rest of the running VM. This includes ordinary loads as well as
   * atomic, exclusive, and ordered accesses. Keep the checker deterministic
   * by limiting it to blocks without a load/store allocation.
   */
  const bool load_store_allocation =
      (instruction & 0x0a00'0000U) == 0x0800'0000U;
  const bool counter_read =
      (instruction & 0xffff'ffe0U) == 0xd53b'e040U;
  return !load_store_allocation && !counter_read;
}

[[nodiscard]] bool block_links_enabled() noexcept {
  static const bool enabled = [] {
    const auto *disabled = std::getenv("UML_JIT_DISABLE_BLOCK_LINKS");
    return disabled == nullptr || disabled[0] == '\0' || disabled[0] == '0';
  }();
  return enabled;
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

enum class JitCacheValidationResult {
  hit,
  mismatch,
  fault,
  sequence_race,
};

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

void activate_native_fp_environment(CpuState &cpu) noexcept {
  if (cpu.gadget_fp_active)
    return;
  __asm__ volatile("mrs %0, fpcr" : "=r"(cpu.gadget_host_fpcr) : : "memory");
  __asm__ volatile("mrs %0, fpsr" : "=r"(cpu.gadget_host_fpsr) : : "memory");
  cpu.gadget_fp_active = true;
  const std::uint64_t fpcr = arm64::detail::host_fpcr(cpu.fpcr);
  const std::uint64_t fpsr = cpu.fpsr;
  __asm__ volatile("msr fpcr, %0" : : "r"(fpcr) : "memory");
  __asm__ volatile("msr fpsr, %0" : : "r"(fpsr) : "memory");
}

void activate_native_fpmr_environment(CpuState &cpu) noexcept {
  if (cpu.gadget_fpmr_active)
    return;
  __asm__ volatile("mrs %0, S3_3_C4_C4_2"
                   : "=r"(cpu.gadget_host_fpmr)
                   :
                   : "memory");
  cpu.gadget_fpmr_active = true;
  const std::uint64_t guest_fpmr = cpu.fpmr;
  __asm__ volatile("msr S3_3_C4_C4_2, %0"
                   :
                   : "r"(guest_fpmr)
                   : "memory");
}

void materialize_semantic_cpu_state(CpuState &cpu) noexcept {
  /*
   * A terminal generated-code helper runs outside the native register
   * environment. Commit any retained guest status and restore the host
   * environment before the reference semantics can inspect CpuState.
   */
  arm64::gadgets::restore_native_fp_environment(cpu);
  cpu.access_fp_state();
}

extern "C" std::uint64_t
uml_arm64_jit_alignment_fault(CpuState *cpu, Tlb *tlb,
                              std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  const unsigned base = (instruction >> 5) & 31U;
  tlb->fault_address = cpu->read_register(base, true);
  const unsigned operation = (instruction >> 22) & 3U;
  const bool vector = (instruction & 0x0400'0000U) != 0;
  const bool authenticated_load =
      (instruction & 0xff20'0400U) == 0xf820'0400U;
  const bool load = authenticated_load ||
                    (vector ? (operation & 1U) != 0U : operation != 0U);
  tlb->fault_access = load ? Access::read : Access::write;
  return pack_jit_result(Event::memory_fault, 0);
}

extern "C" void *uml_arm64_jit_tlb_translate(Tlb *tlb, std::uint64_t address,
                                             unsigned access) noexcept {
  if (tlb == nullptr || access > 1)
    return nullptr;
  return tlb->translate(address, access == 0 ? Access::read : Access::write);
}

extern "C" void *
uml_arm64_jit_tlb_translate_preserving(Tlb *tlb, std::uint64_t address,
                                       unsigned access) noexcept;
extern "C" void *
uml_arm64_jit_tlb_translate_preserving_fpmr(Tlb *tlb,
                                            std::uint64_t address,
                                            unsigned access) noexcept;

[[nodiscard]] bool jit_pair_read(Tlb &tlb, std::uint64_t address,
                                 std::span<std::byte> bytes) noexcept {
  while (!bytes.empty()) {
    auto *source = tlb.translate(address, Access::read);
    if (source == nullptr)
      return false;
    const auto count = std::min(
        bytes.size(),
        static_cast<std::size_t>(page_size - (address & (page_size - 1U))));
    std::memcpy(bytes.data(), source, count);
    address += count;
    bytes = bytes.subspan(count);
  }
  return true;
}

[[nodiscard]] bool jit_pair_write(Tlb &tlb, std::uint64_t address,
                                  std::span<const std::byte> bytes) noexcept {
  /* Pair stores are architecturally one memory access. Translate every
   * page before the first host store so a second-page fault cannot expose a
   * partial guest update. A pair spans at most two supported guest pages.
   */
  std::array<std::byte *, 2> destinations{};
  std::array<std::size_t, 2> lengths{};
  std::size_t pages = 0;
  std::size_t completed = 0;
  while (completed != bytes.size()) {
    if (pages == destinations.size())
      return false;
    const auto current = address + completed;
    auto *destination = tlb.translate(current, Access::write);
    if (destination == nullptr)
      return false;
    const auto count = std::min(
        bytes.size() - completed,
        static_cast<std::size_t>(page_size - (current & (page_size - 1U))));
    destinations[pages] = destination;
    lengths[pages] = count;
    ++pages;
    completed += count;
  }
  completed = 0;
  for (std::size_t page = 0; page < pages; ++page) {
    std::memcpy(destinations[page], bytes.data() + completed, lengths[page]);
    completed += lengths[page];
  }
  return true;
}

class JitSemanticMemory final : public arm64::GuestMemory {
public:
  explicit JitSemanticMemory(Tlb &tlb) noexcept : tlb_{tlb} {}

  bool read(std::uint64_t address, std::span<std::byte> destination) override {
    if (jit_pair_read(tlb_, address, destination))
      return true;
    record_fault();
    return false;
  }

  bool write(std::uint64_t address,
             std::span<const std::byte> source) override {
    std::size_t completed = 0;
    while (completed < source.size()) {
      const auto current = address + completed;
      tlb_.discard(current);
      completed += std::min(
          source.size() - completed,
          static_cast<std::size_t>(page_size - (current & (page_size - 1U))));
    }
    if (jit_pair_write(tlb_, address, source))
      return true;
    record_fault();
    return false;
  }

  bool validate_access(std::uint64_t address, std::size_t size,
                       arm64::MemoryAccess access) override {
    const auto direction = access == arm64::MemoryAccess::write
                               ? Access::write
                               : Access::read;
    std::size_t completed = 0;
    while (completed < size) {
      const auto current = address + completed;
      if (tlb_.translate(current, direction) == nullptr) {
        record_fault();
        return false;
      }
      completed += std::min(
          size - completed,
          static_cast<std::size_t>(
              page_size - (current & (page_size - 1U))));
    }
    return true;
  }

  /*
   * Returning no direct pointer deliberately selects the interpreter's
   * ordinary-load/store atomic fallback. Host exclusive instructions cannot
   * be restarted safely after a Darwin shadow-map protection fault.
   */
  std::byte *atomic_pointer(std::uint64_t, std::size_t,
                            arm64::MemoryAccess) override {
    return nullptr;
  }

  std::byte *reservation_pointer(
      std::uint64_t address, std::size_t size,
      arm64::MemoryAccess access) override {
    if (size == 0 ||
        size > page_size - (address & (page_size - 1U)))
      return nullptr;
    return tlb_.translate(
        address, access == arm64::MemoryAccess::write ? Access::write
                                                      : Access::read);
  }

  bool read_allocation_tag(std::uint64_t address,
                           std::uint8_t &tag) override {
    if (tlb_.read_allocation_tag(address, tag))
      return true;
    record_fault();
    return false;
  }

  bool write_allocation_tag(std::uint64_t address,
                            std::uint8_t tag) override {
    if (tlb_.write_allocation_tag(address, tag))
      return true;
    record_fault();
    return false;
  }

  arm64::MemoryFault last_fault() const noexcept override {
    return fault_;
  }

  [[nodiscard]] bool has_fault() const noexcept { return fault_recorded_; }

private:
  void record_fault() noexcept {
    fault_ = {
        tlb_.fault_address,
        tlb_.fault_access == Access::write ? arm64::MemoryAccess::write
                                           : arm64::MemoryAccess::read,
    };
    fault_recorded_ = true;
  }

  Tlb &tlb_;
  arm64::MemoryFault fault_{};
  bool fault_recorded_{};
};

void record_semantic_fault(Tlb &tlb, const JitSemanticMemory &memory,
                           const arm64::RunResult &result) noexcept {
  tlb.fault_address =
      memory.has_fault() ? memory.last_fault().address : result.address;
  tlb.fault_access = result.write ? Access::write : Access::read;
}

extern "C" std::uint64_t
uml_arm64_jit_execute_exclusive(CpuState *cpu, Tlb *tlb,
                                std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  const bool establishes_reservation =
      (instruction & 0x00c0'0000U) == 0x0040'0000U;
  const bool uses_software_reservation =
      establishes_reservation || cpu->exclusive_size != 0U;
  const auto execute = [&]() noexcept {
    const bool registered =
        establishes_reservation && begin_jit_reservation(*cpu);
    JitSemanticMemory memory{*tlb};
    const auto result =
        arm64::Dispatcher{}.execute(*cpu, memory, instruction);
    if (establishes_reservation && cpu->exclusive_size != 0U &&
        current_jit_store_participant != nullptr)
      current_jit_store_participant->reservation_cpu.store(
          cpu, std::memory_order_seq_cst);
    if (registered && cpu->exclusive_size == 0) {
      if (current_jit_store_participant != nullptr &&
          current_jit_store_participant->reservation_cpu.load(
              std::memory_order_seq_cst) == cpu)
        current_jit_store_participant->reservation_cpu.store(
            nullptr, std::memory_order_seq_cst);
      end_jit_reservation(*cpu);
    }
    switch (result.reason) {
    case arm64::StopReason::continued:
    case arm64::StopReason::budget_exhausted:
      return pack_jit_result(Event::continued, result.instructions);
    case arm64::StopReason::memory_fault:
      record_semantic_fault(*tlb, memory, result);
      return pack_jit_result(Event::memory_fault, result.instructions);
    case arm64::StopReason::gcs_fault:
      return pack_jit_result(Event::gcs_fault, result.instructions);
    case arm64::StopReason::illegal_instruction:
      return pack_jit_result(Event::illegal_instruction,
                             result.instructions);
    case arm64::StopReason::breakpoint:
      return pack_jit_result(Event::breakpoint, result.instructions);
    case arm64::StopReason::system_call:
      return pack_jit_result(Event::system_call, result.instructions);
    case arm64::StopReason::floating_point_trap:
      return pack_jit_result(Event::floating_point_trap,
                             result.instructions);
    case arm64::StopReason::unsupported_instruction:
      return pack_jit_result(Event::illegal_instruction,
                             result.instructions);
    }
    return pack_jit_result(Event::fatal, 0);
  };
  /*
   * Native compare/exchange and atomic instructions already provide their
   * own inter-vCPU serialization. Only the semantic fallback which creates
   * or consumes CpuState's software reservation needs the publication gate.
   */
  if (uses_software_reservation) {
    JitExclusiveOperationScope operation;
    return execute();
  }
  return execute();
}

extern "C" std::uint64_t
uml_arm64_jit_execute_memory(CpuState *cpu, Tlb *tlb,
                             std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  JitSemanticMemory memory{*tlb};
  const auto result = arm64::Dispatcher{}.execute(*cpu, memory, instruction);
  switch (result.reason) {
  case arm64::StopReason::continued:
  case arm64::StopReason::budget_exhausted:
    return pack_jit_result(Event::continued, result.instructions);
  case arm64::StopReason::memory_fault:
    record_semantic_fault(*tlb, memory, result);
    return pack_jit_result(Event::memory_fault, result.instructions);
  case arm64::StopReason::gcs_fault:
    return pack_jit_result(Event::gcs_fault, result.instructions);
  case arm64::StopReason::illegal_instruction:
    return pack_jit_result(Event::illegal_instruction, result.instructions);
  case arm64::StopReason::breakpoint:
    return pack_jit_result(Event::breakpoint, result.instructions);
  case arm64::StopReason::system_call:
    return pack_jit_result(Event::system_call, result.instructions);
  case arm64::StopReason::floating_point_trap:
    return pack_jit_result(Event::floating_point_trap, result.instructions);
  case arm64::StopReason::unsupported_instruction:
    return pack_jit_result(Event::illegal_instruction, result.instructions);
  }
  return pack_jit_result(Event::fatal, 0);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_gcs(CpuState *cpu, Tlb *tlb,
                          std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  JitSemanticMemory memory{*tlb};
  const auto result = arm64::Dispatcher{}.execute(*cpu, memory, instruction);
  switch (result.reason) {
  case arm64::StopReason::continued:
  case arm64::StopReason::budget_exhausted:
    return pack_jit_result(Event::continued, result.instructions);
  case arm64::StopReason::memory_fault:
    record_semantic_fault(*tlb, memory, result);
    return pack_jit_result(Event::memory_fault, result.instructions);
  case arm64::StopReason::gcs_fault:
    return pack_jit_result(Event::gcs_fault, result.instructions);
  case arm64::StopReason::illegal_instruction:
    return pack_jit_result(Event::illegal_instruction, result.instructions);
  case arm64::StopReason::breakpoint:
    return pack_jit_result(Event::breakpoint, result.instructions);
  case arm64::StopReason::system_call:
    return pack_jit_result(Event::system_call, result.instructions);
  case arm64::StopReason::floating_point_trap:
    return pack_jit_result(Event::floating_point_trap, result.instructions);
  case arm64::StopReason::unsupported_instruction:
    return pack_jit_result(Event::illegal_instruction, result.instructions);
  }
  return pack_jit_result(Event::fatal, 0);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_pair(CpuState *cpu, Tlb *tlb,
                           std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);

  const bool vector = (instruction & 0x0400'0000U) != 0;
  const bool load = (instruction & 0x0040'0000U) != 0;
  const unsigned operation = instruction >> 30;
  const unsigned mode = (instruction >> 23) & 3U;
  const std::size_t size =
      vector            ? (operation >= 2U ? 16U : std::size_t{4} << operation)
      : operation >= 2U ? 8U
                        : 4U;
  const auto displacement = sign_extend((instruction >> 15) & 0x7fU, 7) *
                            static_cast<std::int64_t>(size);
  const unsigned base_register = (instruction >> 5) & 31U;
  const auto tagged_base = cpu->read_register(base_register, true);
  if (base_register == 31U && (tagged_base & 15U) != 0) {
    tlb->fault_address = tagged_base;
    tlb->fault_access = load ? Access::read : Access::write;
    return pack_jit_result(Event::memory_fault, 0);
  }
  const auto adjusted =
      tagged_base + static_cast<std::uint64_t>(displacement);
  const auto tagged_address = mode == 1U ? tagged_base : adjusted;
  const bool tagged = base_register != 31U && cpu->mte_enabled;
  const auto address =
      tagged ? tagged_address & 0x00ff'ffff'ffff'ffffULL : tagged_address;
  const unsigned first = instruction & 31U;
  const unsigned second = (instruction >> 10) & 31U;
  std::array<std::byte, 32> bytes{};
  const auto transfer = std::span{bytes}.first(size * 2U);
  const auto direction = load ? Access::read : Access::write;
  const bool tagchecked = mode == 1U || mode == 3U || base_register != 31U;
  if (tagchecked && cpu->mte_enabled && !cpu->pstate_tco) {
    /* A pair is one architectural memory access. Resolve every page and
     * allocation granule before either destination or memory can change. */
    std::size_t completed = 0;
    while (completed < transfer.size()) {
      const auto current = address + completed;
      if (tlb->translate(current, direction) == nullptr)
        return pack_jit_result(Event::memory_fault, 0);
      completed +=
          std::min(transfer.size() - completed,
                   static_cast<std::size_t>(
                       page_size - (current & (page_size - 1U))));
    }
    const auto logical_tag =
        tagged ? static_cast<std::uint8_t>(tagged_address >> 56) & 15U : 0U;
    for (std::size_t checked = 0; checked < transfer.size();) {
      const auto current = address + checked;
      std::uint8_t allocation_tag{};
      if (!tlb->read_allocation_tag(current, allocation_tag) ||
          allocation_tag != logical_tag) {
        tlb->fault_address = current;
        tlb->fault_access = direction;
        return pack_jit_result(Event::memory_fault, 0);
      }
      checked += std::min(
          transfer.size() - checked,
          static_cast<std::size_t>(16U - (current & 15U)));
    }
  }

  if (load) {
    if (!jit_pair_read(*tlb, address, transfer))
      return pack_jit_result(Event::memory_fault, 0);
    if (vector) {
      cpu->vectors[first].bytes.fill(std::byte{});
      cpu->vectors[second].bytes.fill(std::byte{});
      std::memcpy(cpu->vectors[first].bytes.data(), bytes.data(), size);
      std::memcpy(cpu->vectors[second].bytes.data(), bytes.data() + size, size);
    } else {
      std::uint64_t first_value{};
      std::uint64_t second_value{};
      std::memcpy(&first_value, bytes.data(), size);
      std::memcpy(&second_value, bytes.data() + size, size);
      if (operation == 1U) {
        first_value = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(static_cast<std::int32_t>(first_value)));
        second_value = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(static_cast<std::int32_t>(second_value)));
      }
      cpu->write_register(first, first_value, false);
      cpu->write_register(second, second_value, false);
    }
  } else {
    if (vector) {
      std::memcpy(bytes.data(), cpu->vectors[first].bytes.data(), size);
      std::memcpy(bytes.data() + size, cpu->vectors[second].bytes.data(), size);
    } else {
      const auto first_value = cpu->read_register(first, false);
      const auto second_value = cpu->read_register(second, false);
      std::memcpy(bytes.data(), &first_value, size);
      std::memcpy(bytes.data() + size, &second_value, size);
    }
    if (!jit_pair_write(*tlb, address, transfer))
      return pack_jit_result(Event::memory_fault, 0);
  }

  bool writeback = mode == 1U || mode == 3U;
  if (writeback && load && !vector && base_register != 31U &&
      (base_register == first || base_register == second))
    writeback = false;
  if (writeback)
    cpu->write_register(base_register, adjusted, true);
  cpu->pc += sizeof(instruction);
  return pack_jit_result(Event::continued, 1);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_ordered(CpuState *cpu, Tlb *tlb,
                              std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  const bool load = (instruction & 0x0040'0000U) != 0;
  const unsigned size_shift = instruction >> 30;
  const std::size_t size = std::size_t{1} << size_shift;
  const unsigned base = (instruction >> 5) & 31U;
  const unsigned transfer = instruction & 31U;
  const auto address = cpu->read_register(base, true);
  if ((address & (size - 1U)) != 0) {
    tlb->fault_address = address;
    tlb->fault_access = load ? Access::read : Access::write;
    return pack_jit_result(Event::memory_fault, 0);
  }

  if (load) {
    auto *source = tlb->translate(address, Access::read);
    if (source == nullptr)
      return pack_jit_result(Event::memory_fault, 0);
    std::uint64_t value = 0;
    std::memcpy(&value, source, size);
    std::atomic_thread_fence(std::memory_order_acquire);
    cpu->write_register(transfer, value, false);
  } else {
    /*
     * The dynamic loader can change COW permissions within the same
     * page between ordered stores. Revalidate the write translation at
     * this cold helper boundary instead of retaining a pre-transition
     * host pointer.
     */
    tlb->discard(address);
    auto *destination = tlb->translate(address, Access::write);
    if (destination == nullptr)
      return pack_jit_result(Event::memory_fault, 0);
    const auto value = cpu->read_register(transfer, false);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(destination, &value, size);
  }
  cpu->pc += sizeof(instruction);
  return pack_jit_result(Event::continued, 1);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_simd_structure(CpuState *cpu, Tlb *tlb,
                                     std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  const bool single = (instruction & 0xbf80'0000U) == 0x0d00'0000U ||
                      (instruction & 0xbf80'0000U) == 0x0d80'0000U;
  const bool load = (instruction & 0x0040'0000U) != 0;
  const bool post_index = (instruction & 0x0080'0000U) != 0;
  const unsigned offset_register = (instruction >> 16) & 31U;
  const unsigned base_register = (instruction >> 5) & 31U;
  const unsigned first_register = instruction & 31U;
  const auto tagged_address = cpu->read_register(base_register, true);
  if (base_register == 31U && (tagged_address & 15U) != 0) {
    tlb->fault_address = tagged_address;
    tlb->fault_access = load ? Access::read : Access::write;
    return pack_jit_result(Event::memory_fault, 0);
  }

  unsigned register_count = 0;
  std::size_t element_size = 0;
  std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ? 16U : 8U;
  std::size_t lane_offset = 0;
  bool interleaved = false;
  bool replicate = false;
  if (!single) {
    const unsigned opcode = (instruction >> 12) & 15U;
    interleaved = opcode == 8U || opcode == 4U || opcode == 0U;
    register_count = opcode == 7U    ? 1U
                     : opcode == 10U ? 2U
                     : opcode == 6U  ? 3U
                     : opcode == 2U  ? 4U
                     : opcode == 8U  ? 2U
                     : opcode == 4U  ? 3U
                     : opcode == 0U  ? 4U
                                     : 0U;
    const unsigned size = (instruction >> 10) & 3U;
    if (register_count == 0U || (!post_index && offset_register != 0U) ||
        (interleaved && vector_size == 8U && size == 3U))
      return pack_jit_result(Event::illegal_instruction, 0);
    element_size = std::size_t{1} << size;
  } else {
    const unsigned extra_register = (instruction >> 21) & 1U;
    const unsigned opcode = (instruction >> 13) & 7U;
    const unsigned scale_bit = (instruction >> 12) & 1U;
    const unsigned size = (instruction >> 10) & 3U;
    const unsigned scale = opcode >> 1;
    if (!post_index && offset_register != 0U)
      return pack_jit_result(Event::illegal_instruction, 0);
    if (scale == 3U) {
      if (!load || scale_bit != 0U)
        return pack_jit_result(Event::illegal_instruction, 0);
      replicate = true;
      register_count = opcode == 6U ? 1U + extra_register : 3U + extra_register;
      element_size = std::size_t{1} << size;
    } else {
      register_count =
          (opcode & 1U) != 0U ? 3U + extra_register : 1U + extra_register;
      std::size_t lane = 0;
      if (scale == 0U) {
        element_size = 1U;
        lane = (static_cast<std::size_t>((instruction >> 30) & 1U) << 3) |
               (scale_bit << 2) | size;
      } else if (scale == 1U) {
        if ((size & 1U) != 0U)
          return pack_jit_result(Event::illegal_instruction, 0);
        element_size = 2U;
        lane = (static_cast<std::size_t>((instruction >> 30) & 1U) << 2) |
               (scale_bit << 1) | (size >> 1);
      } else {
        if ((size & 2U) != 0U || ((size & 1U) != 0U && scale_bit != 0U))
          return pack_jit_result(Event::illegal_instruction, 0);
        element_size = (size & 1U) != 0U ? 8U : 4U;
        lane = (size & 1U) != 0U
                   ? static_cast<std::size_t>((instruction >> 30) & 1U)
                   : (static_cast<std::size_t>((instruction >> 30) & 1U) << 1) |
                         scale_bit;
      }
      lane_offset = lane * element_size;
    }
  }

  const auto transfer_size =
      single ? element_size * register_count : vector_size * register_count;
  std::array<std::byte, 64> bytes{};
  auto transfer = std::span{bytes}.first(transfer_size);
  const bool base_sp = base_register == 31U;
  const bool tagchecked = post_index || !base_sp;
  if (load) {
    if (uml_arm64_gadget_simd_structure_memory_transfer(
            cpu, tlb, tagged_address, transfer.data(), transfer.size(), 0,
            base_sp,
            static_cast<std::uint32_t>(
                (element_size << 8) | static_cast<std::size_t>(tagchecked))) ==
        0U)
      return pack_jit_result(Event::memory_fault, 0);
    for (unsigned index = 0; index < register_count; ++index) {
      auto &destination = cpu->vectors[(first_register + index) & 31U].bytes;
      if (!single) {
        destination.fill(std::byte{});
        if (interleaved) {
          for (std::size_t lane = 0; lane < vector_size / element_size; ++lane)
            std::memcpy(destination.data() + lane * element_size,
                        bytes.data() +
                            (lane * register_count + index) * element_size,
                        element_size);
        } else {
          std::memcpy(destination.data(), bytes.data() + index * vector_size,
                      vector_size);
        }
      } else if (replicate) {
        destination.fill(std::byte{});
        for (std::size_t offset = 0; offset < vector_size;
             offset += element_size)
          std::memcpy(destination.data() + offset,
                      bytes.data() + index * element_size, element_size);
      } else {
        std::memcpy(destination.data() + lane_offset,
                    bytes.data() + index * element_size, element_size);
      }
    }
  } else {
    for (unsigned index = 0; index < register_count; ++index) {
      const auto &source = cpu->vectors[(first_register + index) & 31U].bytes;
      if (!single) {
        if (interleaved) {
          for (std::size_t lane = 0; lane < vector_size / element_size; ++lane)
            std::memcpy(bytes.data() +
                            (lane * register_count + index) * element_size,
                        source.data() + lane * element_size, element_size);
        } else {
          std::memcpy(bytes.data() + index * vector_size, source.data(),
                      vector_size);
        }
      } else {
        std::memcpy(bytes.data() + index * element_size,
                    source.data() + lane_offset, element_size);
      }
    }
    if (uml_arm64_gadget_simd_structure_memory_transfer(
            cpu, tlb, tagged_address, transfer.data(), transfer.size(), 1,
            base_sp,
            static_cast<std::uint32_t>(
                (element_size << 8) | static_cast<std::size_t>(tagchecked))) ==
        0U)
      return pack_jit_result(Event::memory_fault, 0);
  }
  if (post_index) {
    const auto offset = offset_register == 31U
                            ? transfer_size
                            : cpu->read_register(offset_register, false);
    cpu->write_register(base_register, tagged_address + offset, true);
  }
  cpu->pc += sizeof(instruction);
  return pack_jit_result(Event::continued, 1);
}

class OverlayMemory final : public arm64::GuestMemory {
public:
  explicit OverlayMemory(StaticGuestMemory &memory) : memory_{memory} {}

  bool read(std::uint64_t address, std::span<std::byte> destination) override {
    if (!memory_.read(address, destination))
      return false;
    apply(address, destination);
    return true;
  }

  bool fetch(std::uint64_t address, std::span<std::byte> destination) override {
    if (!memory_.fetch(address, destination))
      return false;
    apply(address, destination);
    return true;
  }

  bool write(std::uint64_t address,
             std::span<const std::byte> source) override {
    if (!memory_.validate_write(address, source.size()))
      return false;
    for (std::size_t index = 0; index < source.size(); ++index)
      writes_.insert_or_assign(address + index, source[index]);
    return true;
  }

  bool validate_access(std::uint64_t address, std::size_t size,
                       arm64::MemoryAccess access) override {
    return memory_.validate_access(address, size, access);
  }

  arm64::MemoryFault last_fault() const noexcept override {
    return memory_.last_fault();
  }

  [[nodiscard]] bool matches_guest() const {
    for (const auto &[address, expected] : writes_) {
      std::byte actual{};
      if (!memory_.read(address, std::span{&actual, 1}) || actual != expected) {
        std::fprintf(stderr,
                     "Darwin JIT verify memory mismatch address=%#llx "
                     "expected=%#x actual=%#x\n",
                     static_cast<unsigned long long>(address),
                     std::to_integer<unsigned>(expected),
                     std::to_integer<unsigned>(actual));
        return false;
      }
    }
    return true;
  }

private:
  void apply(std::uint64_t address, std::span<std::byte> destination) const {
    for (std::size_t index = 0; index < destination.size(); ++index)
      if (const auto found = writes_.find(address + index);
          found != writes_.end())
        destination[index] = found->second;
  }

  StaticGuestMemory &memory_;
  std::unordered_map<std::uint64_t, std::byte> writes_;
};

[[nodiscard]] Event reference_event(const arm64::RunResult &result) noexcept {
  switch (result.reason) {
  case arm64::StopReason::continued:
  case arm64::StopReason::budget_exhausted:
    return Event::continued;
  case arm64::StopReason::system_call:
    return Event::system_call;
  case arm64::StopReason::memory_fault:
    return Event::memory_fault;
  case arm64::StopReason::gcs_fault:
    return Event::gcs_fault;
  case arm64::StopReason::illegal_instruction:
    return Event::illegal_instruction;
  case arm64::StopReason::breakpoint:
    return Event::breakpoint;
  case arm64::StopReason::floating_point_trap:
    return Event::floating_point_trap;
  case arm64::StopReason::unsupported_instruction:
    return Event::illegal_instruction;
  }
  return Event::fatal;
}

[[nodiscard]] bool same_cpu(const CpuState &expected, const CpuState &actual,
                            std::uint64_t pc) {
  bool same = true;
  for (std::size_t index = 0; index < expected.registers.size(); ++index) {
    if (expected.registers[index] == actual.registers[index])
      continue;
    std::fprintf(
        stderr,
        "Darwin JIT verify register mismatch block=%#llx x%zu=%#llx/%#llx\n",
        static_cast<unsigned long long>(pc), index,
        static_cast<unsigned long long>(expected.registers[index]),
        static_cast<unsigned long long>(actual.registers[index]));
    same = false;
  }
  const bool extended_state_mismatch =
      expected.fpmr != actual.fpmr ||
      expected.mte_enabled != actual.mte_enabled ||
      expected.mte_exclude != actual.mte_exclude ||
      expected.mte_tag_choices != actual.mte_tag_choices ||
      expected.mte_random_tags != actual.mte_random_tags ||
      expected.mte_seed != actual.mte_seed ||
      expected.mte_tag != actual.mte_tag ||
      expected.mte_random_state != actual.mte_random_state ||
      expected.cpa_enabled != actual.cpa_enabled ||
      expected.cpa_multiply_enabled != actual.cpa_multiply_enabled ||
      expected.accdata != actual.accdata ||
      expected.pstate_ssbs != actual.pstate_ssbs ||
      expected.pstate_dit != actual.pstate_dit ||
      expected.pstate_tco != actual.pstate_tco ||
      expected.pstate_sm != actual.pstate_sm ||
      expected.pstate_za != actual.pstate_za ||
      expected.pstate_daif_access != actual.pstate_daif_access ||
      expected.pstate_daif != actual.pstate_daif ||
      expected.pointer_auth_keys != actual.pointer_auth_keys;
  if (expected.sp != actual.sp || expected.pc != actual.pc ||
      expected.nzcv != actual.nzcv || expected.tls != actual.tls ||
      expected.fpcr != actual.fpcr ||
      expected.gcs_pointer != actual.gcs_pointer ||
      expected.gcs_implemented != actual.gcs_implemented ||
      expected.gcs_push_enabled != actual.gcs_push_enabled ||
      expected.gcs_store_enabled != actual.gcs_store_enabled ||
      extended_state_mismatch ||
      ((expected.fpsr ^ actual.fpsr) & ~0x9fU) != 0) {
    std::fprintf(stderr,
                 "Darwin JIT verify state mismatch block=%#llx sp=%#llx/%#llx "
                 "pc=%#llx/%#llx nzcv=%#x/%#x tls=%#llx/%#llx fpcr=%#llx/%#llx "
                 "fpsr=%#llx/%#llx\n",
                 static_cast<unsigned long long>(pc),
                 static_cast<unsigned long long>(expected.sp),
                 static_cast<unsigned long long>(actual.sp),
                 static_cast<unsigned long long>(expected.pc),
                 static_cast<unsigned long long>(actual.pc), expected.nzcv,
                 actual.nzcv, static_cast<unsigned long long>(expected.tls),
                 static_cast<unsigned long long>(actual.tls),
                 static_cast<unsigned long long>(expected.fpcr),
                 static_cast<unsigned long long>(actual.fpcr),
                 static_cast<unsigned long long>(expected.fpsr),
                 static_cast<unsigned long long>(actual.fpsr));
    if (extended_state_mismatch)
      std::fprintf(stderr,
                   "Darwin JIT verify extended architectural state mismatch "
                   "block=%#llx\n",
                   static_cast<unsigned long long>(pc));
    same = false;
  }
  for (std::size_t index = 0; index < expected.vectors.size(); ++index) {
    if (std::memcmp(&expected.vectors[index], &actual.vectors[index],
                    sizeof(expected.vectors[index])) != 0) {
      std::uint64_t exp_words[2]{}, act_words[2]{};
      std::memcpy(exp_words, &expected.vectors[index], 16);
      std::memcpy(act_words, &actual.vectors[index], 16);
      std::fprintf(stderr,
                   "Darwin JIT verify vector mismatch block=%#llx v%zu=\n"
                   "  exp: %016llx_%016llx\n"
                   "  act: %016llx_%016llx\n",
                   static_cast<unsigned long long>(pc), index,
                   static_cast<unsigned long long>(exp_words[1]),
                   static_cast<unsigned long long>(exp_words[0]),
                   static_cast<unsigned long long>(act_words[1]),
                   static_cast<unsigned long long>(act_words[0]));
      same = false;
    }
  }
  return same;
}

[[nodiscard]] std::uintptr_t callable_address(const void *pointer) noexcept {
#if __has_feature(ptrauth_calls)
  return reinterpret_cast<std::uintptr_t>(
      ptrauth_strip(pointer, ptrauth_key_function_pointer));
#else
  return reinterpret_cast<std::uintptr_t>(pointer);
#endif
}

template <typename Function>
[[nodiscard]] Function sign_jit_function(void *pointer) noexcept {
#if __has_feature(ptrauth_calls)
  return reinterpret_cast<Function>(
      ptrauth_sign_unauthenticated(pointer, ptrauth_key_function_pointer, 0));
#else
  return reinterpret_cast<Function>(pointer);
#endif
}

class JitArena final {
public:
  explicit JitArena(std::size_t size = arena_size) noexcept : size_{size} {
    mapping_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (mapping_ == MAP_FAILED)
      mapping_ = nullptr;
  }

  ~JitArena() {
    if (mapping_ != nullptr)
      (void)munmap(mapping_, size_);
  }

  JitArena(const JitArena &) = delete;
  JitArena &operator=(const JitArena &) = delete;

  [[nodiscard]] bool valid() const noexcept { return mapping_ != nullptr; }

  [[nodiscard]] void *store(std::span<const std::uint32_t> words) noexcept {
    const auto bytes = words.size_bytes();
    const auto aligned = (used_ + 15U) & ~std::size_t{15};
    if (mapping_ == nullptr || bytes == 0 || aligned > size_ ||
        bytes > size_ - aligned)
      return nullptr;
    auto *destination = static_cast<std::byte *>(mapping_) + aligned;
    pthread_jit_write_protect_np(0);
    std::memcpy(destination, words.data(), bytes);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(destination, bytes);
    used_ = aligned + bytes;
    return destination;
  }

  void reset() noexcept { used_ = 0; }

private:
  void *mapping_{};
  std::size_t used_{};
  std::size_t size_{};
};

class Arm64Emitter final {
public:
  using Label = std::size_t;
  using ExitFlush = void (*)(void *);
  using FlagClobber = void (*)(void *);

  enum class BranchKind {
    unconditional,
    conditional,
    compare_zero,
    compare_nonzero,
    test_zero,
    test_nonzero,
  };

  struct Patch final {
    std::size_t word;
    Label label;
    BranchKind kind;
  };

  struct StablePointerPatch final {
    std::size_t word;
    std::uint64_t value;
    unsigned destination;
  };

  explicit Arm64Emitter(
      bool persistent_linked_block_abi =
          persistent_linked_block_abi_default,
      std::size_t literal_range_words = std::size_t{1} << 18U) noexcept
      : literal_range_words_{
            std::min(literal_range_words, std::size_t{1} << 18U)},
        persistent_linked_block_abi_{persistent_linked_block_abi} {}

  [[nodiscard]] bool persistent_linked_block_abi() const noexcept {
    return persistent_linked_block_abi_;
  }

  [[nodiscard]] Label make_label() {
    labels_.push_back(unbound);
    return labels_.size() - 1;
  }

  void bind(Label label) { labels_.at(label) = words_.size(); }

  void emit(std::uint32_t instruction) { words_.push_back(instruction); }

  void attach_exit_flush(void *context, ExitFlush flush) noexcept {
    exit_context_ = context;
    exit_flush_ = flush;
  }

  void attach_flag_clobber(void *context, FlagClobber clobber) noexcept {
    flag_context_ = context;
    flag_clobber_ = clobber;
  }

  [[nodiscard]] std::size_t size() const noexcept { return words_.size(); }

  void branch(Label label) {
    patch(label, BranchKind::unconditional, 0x1400'0000U);
  }

  void branch_condition(Label label, unsigned condition) {
    patch(label, BranchKind::conditional, 0x5400'0000U | (condition & 15U));
  }

  void compare_zero(Label label, unsigned reg, bool is_64_bit, bool nonzero) {
    const auto base = is_64_bit ? (nonzero ? 0xb500'0000U : 0xb400'0000U)
                                : (nonzero ? 0x3500'0000U : 0x3400'0000U);
    patch(label,
          nonzero ? BranchKind::compare_nonzero : BranchKind::compare_zero,
          base | (reg & 31U));
  }

  void test_zero(Label label, unsigned reg, unsigned bit, bool nonzero) {
    const auto base = nonzero ? 0x3700'0000U : 0x3600'0000U;
    const auto encoded_bit = ((bit & 32U) << 26) | ((bit & 31U) << 19);
    patch(label, nonzero ? BranchKind::test_nonzero : BranchKind::test_zero,
          base | encoded_bit | (reg & 31U));
  }

  void load_x(unsigned destination, unsigned base, std::size_t offset) {
    emit(0xf940'0000U | (static_cast<std::uint32_t>(offset / 8U) << 10) |
         ((base & 31U) << 5) | (destination & 31U));
  }

  void load_x_indexed_8(unsigned destination, unsigned base, unsigned index) {
    /*
     * LDR Xd, [Xn, Xm, LSL #3]. The caller supplies a zero-extended
     * host-owned selector for the contiguous 64-bit entry table.
     */
    emit(0xf860'7800U | ((index & 31U) << 16U) | ((base & 31U) << 5U) |
         (destination & 31U));
  }

  void load_pair_x(unsigned first, unsigned second, unsigned base,
                   std::ptrdiff_t offset) {
    if ((offset % 8) != 0 || offset < -512 || offset > 504)
      std::abort();
    const auto scaled =
        static_cast<std::uint32_t>(offset / 8) & 0x7fU;
    emit(0xa940'0000U | (scaled << 15U) | ((second & 31U) << 10U) |
         ((base & 31U) << 5U) | (first & 31U));
  }

  void store_x(unsigned source, unsigned base, std::size_t offset) {
    emit(0xf900'0000U | (static_cast<std::uint32_t>(offset / 8U) << 10) |
         ((base & 31U) << 5) | (source & 31U));
  }

  void load_w(unsigned destination, unsigned base, std::size_t offset) {
    emit(0xb940'0000U | (static_cast<std::uint32_t>(offset / 4U) << 10) |
         ((base & 31U) << 5) | (destination & 31U));
  }

  void store_w(unsigned source, unsigned base, std::size_t offset) {
    emit(0xb900'0000U | (static_cast<std::uint32_t>(offset / 4U) << 10) |
         ((base & 31U) << 5) | (source & 31U));
  }

  void load_b(unsigned destination, unsigned base, std::size_t offset) {
    emit(0x3940'0000U | (static_cast<std::uint32_t>(offset) << 10) |
         ((base & 31U) << 5) | (destination & 31U));
  }

  void store_b(unsigned source, unsigned base, std::size_t offset) {
    emit(0x3900'0000U | (static_cast<std::uint32_t>(offset) << 10) |
         ((base & 31U) << 5) | (source & 31U));
  }

  void load_q(unsigned destination, unsigned base, std::size_t offset) {
    emit(0x3dc0'0000U | (static_cast<std::uint32_t>(offset / 16U) << 10) |
         ((base & 31U) << 5) | (destination & 31U));
  }

  void store_q(unsigned source, unsigned base, std::size_t offset) {
    emit(0x3d80'0000U | (static_cast<std::uint32_t>(offset / 16U) << 10) |
         ((base & 31U) << 5) | (source & 31U));
  }

  void move_q(unsigned destination, unsigned source) {
    emit(0x4ea0'1c00U | ((source & 31U) << 16) |
         ((source & 31U) << 5) | (destination & 31U));
  }

  void move_x(unsigned destination, unsigned source) {
    emit(0xaa00'03e0U | ((source & 31U) << 16) | (destination & 31U));
  }

  void move_w(unsigned destination, unsigned source) {
    emit(0x2a00'03e0U | ((source & 31U) << 16) | (destination & 31U));
  }

  void load_constant(unsigned destination, std::uint64_t value) {
    unsigned move_zero_words = 0;
    unsigned move_not_words = 0;
    for (unsigned half = 0; half < 4; ++half) {
      const auto part = static_cast<std::uint16_t>(value >> (half * 16U));
      move_zero_words += part != 0;
      move_not_words += part != 0xffffU;
    }
    if (move_zero_words == 0)
      move_zero_words = 1;
    if (move_not_words == 0)
      move_not_words = 1;
    /*
     * MOVZ leaves every other half zero; MOVN leaves every other half one.
     * Select MOVN only when that default removes at least one instruction.
     * Ties deliberately retain the stable MOVZ-first shape.
     */
    const bool inverted = move_not_words < move_zero_words;
    bool emitted = false;
    for (unsigned half = 0; half < 4; ++half) {
      const auto part = static_cast<std::uint16_t>(value >> (half * 16U));
      if (part == (inverted ? 0xffffU : 0U))
        continue;
      const auto immediate =
          inverted && !emitted ? static_cast<std::uint16_t>(~part) : part;
      const auto opcode = emitted ? 0xf280'0000U
                                  : inverted ? 0x9280'0000U : 0xd280'0000U;
      emit(opcode | (half << 21) |
           (static_cast<std::uint32_t>(immediate) << 5) |
           (destination & 31U));
      emitted = true;
    }
    if (!emitted)
      emit((inverted ? 0x9280'0000U : 0xd280'0000U) |
           (destination & 31U));
  }

  void load_literal_constant(unsigned destination, std::uint64_t value) {
    /*
     * This placeholder becomes one LDR (literal) after every executable
     * path has been emitted. Translation-resolved guest constants and
     * host-owned link pointers share the immutable tail pool. If an unusually
     * large translation puts the data outside imm19 reach, finish() changes
     * this slot to an immediate branch through a cold MOV-wide
     * materialization stub.
     */
    literal_patches_.push_back(
        {words_.size(), value, destination & 31U});
    emit(0x5800'0000U | (destination & 31U));
  }

  void load_stable_pointer(unsigned destination, std::uintptr_t value) {
    load_literal_constant(destination, static_cast<std::uint64_t>(value));
  }

  void add_immediate(unsigned destination, unsigned source, unsigned immediate,
                     bool shift_12 = false) {
    emit(0x9100'0000U | (shift_12 ? 1U << 22 : 0U) |
         ((immediate & 0xfffU) << 10) | ((source & 31U) << 5) |
         (destination & 31U));
  }

  void subtract_immediate(unsigned destination, unsigned source,
                          unsigned immediate, bool shift_12 = false) {
    emit(0xd100'0000U | (shift_12 ? 1U << 22 : 0U) |
         ((immediate & 0xfffU) << 10) | ((source & 31U) << 5) |
         (destination & 31U));
  }

  void add_shifted(unsigned destination, unsigned left, unsigned right,
                   unsigned shift) {
    emit(0x8b00'0000U | ((right & 31U) << 16) | ((shift & 63U) << 10) |
         ((left & 31U) << 5) | (destination & 31U));
  }

  void ubfm(unsigned destination, unsigned source, unsigned rotate,
            unsigned mask) {
    emit(0xd340'0000U | ((rotate & 63U) << 16) | ((mask & 63U) << 10) |
         ((source & 31U) << 5) | (destination & 31U));
  }

  void compare_register(unsigned left, unsigned right) {
    if (flag_clobber_ != nullptr)
      flag_clobber_(flag_context_);
    emit(0xeb00'001fU | ((right & 31U) << 16) |
         ((left & 31U) << 5));
  }

  void compare_immediate(unsigned source, unsigned immediate,
                         bool shift_12 = false) {
    if (flag_clobber_ != nullptr)
      flag_clobber_(flag_context_);
    emit(0xf100'001fU | (shift_12 ? 1U << 22 : 0U) |
         ((immediate & 0xfffU) << 10) | ((source & 31U) << 5));
  }

  void emit_internal_flag_clobber(std::uint32_t instruction) {
    /*
     * This callback runs while translating. The generated instruction remains
     * branch-free; any required guest-NZCV publication is emitted before it.
     */
    if (flag_clobber_ != nullptr)
      flag_clobber_(flag_context_);
    emit(instruction);
  }

  void call(const void *function) {
    /* A host ABI call owns NZCV. Canonicalize guest flags at translation. */
    if (flag_clobber_ != nullptr)
      flag_clobber_(flag_context_);
    load_stable_pointer(16, callable_address(function));
    emit(0xd63f'0200U);
  }

  void call_preserving_flags(const void *function) {
    /*
     * This is reserved for narrow assembly bridges whose complete contract
     * includes saving and restoring NZCV. Ordinary host calls must continue
     * through call() so a compiler bug cannot silently expose host flags.
     */
    load_stable_pointer(16, callable_address(function));
    emit(0xd63f'0200U);
  }

  void branch_register(unsigned reg) {
    emit(0xd61f'0000U | ((reg & 31U) << 5));
  }

  void prologue() {
    if (persistent_linked_block_abi_) {
      /*
       * Preserve the original argument slots, especially x2 at +32: tracked
       * store lowering consumes that stable stack ABI. x19-x21 remain private
       * to the outer generated invocation and survive every host helper.
       */
      emit(0xa9bc'7bfdU); // stp x29, x30, [sp, #-64]!
      emit(0x9100'03fdU); // mov x29, sp
      emit(0xa901'07e0U); // stp x0, x1, [sp, #16]
      emit(0xa902'57e2U); // stp x2, x21, [sp, #32]
      emit(0xa903'53f3U); // stp x19, x20, [sp, #48]
      move_x(15, 0);
      move_x(14, 1);
      move_x(19, 2);
      static_assert(maximum_chain_instructions <= 0xffffU);
      load_constant(20, maximum_chain_instructions);
      load_b(21, 19, offsetof(JitChainState, link_selector));
      return;
    }
    emit(0xa9bd'7bfdU); // stp x29, x30, [sp, #-48]!
    emit(0x9100'03fdU); // mov x29, sp
    emit(0xa901'07e0U); // stp x0, x1, [sp, #16]
    emit(0xf900'13e2U); // str x2, [sp, #32]
    move_x(15, 0);
    move_x(14, 1);
  }

  void epilogue() {
    if (persistent_linked_block_abi_) {
      /*
       * x20 contains the remaining budget before the current translated
       * block. Publish it exactly once when the outer generated invocation
       * returns, then restore every host callee-saved register.
       */
      store_x(20, 19, offsetof(JitChainState, remaining_instructions));
      emit(0xa943'53f3U); // ldp x19, x20, [sp, #48]
      emit(0xf940'17f5U); // ldr x21, [sp, #40]
      emit(0xa8c4'7bfdU); // ldp x29, x30, [sp], #64
      emit(0xd65f'03c0U);
      return;
    }
    emit(0xa8c3'7bfdU); // ldp x29, x30, [sp], #48
    emit(0xd65f'03c0U);
  }

  void reload_context() {
    load_x(15, 31, 16);
    load_x(14, 31, 24);
  }

  void return_constant(Event event, std::size_t instructions) {
    if (exit_flush_ != nullptr)
      exit_flush_(exit_context_);
    if (terminal_does_not_retire(event)) {
      if (instructions == 0)
        std::abort();
      --instructions;
    }
    load_constant(0, pack_jit_result(event, instructions));
    epilogue();
  }

  void return_dynamic(std::size_t instructions) {
    if (exit_flush_ != nullptr)
      exit_flush_(exit_context_);
    if (instructions == 0)
      std::abort();
    const auto retired_prefix = instructions - 1U;
    if (retired_prefix != 0U) {
      load_constant(1, static_cast<std::uint64_t>(retired_prefix) << 32U);
      /*
       * The helper returns the current instruction's exact 0/1 retirement
       * delta. Addition, unlike a bitwise merge, composes an odd prefix with
       * the successful +1 carry into the upper 32-bit count.
       */
      emit(0x8b01'0000U); // add x0, x0, x1
    }
    epilogue();
  }

  [[nodiscard]] std::span<const std::uint32_t> finish() {
    for (const auto &entry : patches_) {
      if (entry.label >= labels_.size() || labels_[entry.label] == unbound)
        return {};
      const auto target = static_cast<std::int64_t>(labels_[entry.label]);
      const auto source = static_cast<std::int64_t>(entry.word);
      const auto displacement = target - source;
      auto &instruction = words_[entry.word];
      switch (entry.kind) {
      case BranchKind::unconditional:
        if (displacement < -(1 << 25) || displacement >= (1 << 25))
          return {};
        instruction |= static_cast<std::uint32_t>(displacement) & 0x03ff'ffffU;
        break;
      case BranchKind::conditional:
      case BranchKind::compare_zero:
      case BranchKind::compare_nonzero:
        if (displacement < -(1 << 18) || displacement >= (1 << 18))
          return {};
        instruction |= (static_cast<std::uint32_t>(displacement) & 0x7ffffU)
                       << 5;
        break;
      case BranchKind::test_zero:
      case BranchKind::test_nonzero:
        if (displacement < -(1 << 13) || displacement >= (1 << 13))
          return {};
        instruction |= (static_cast<std::uint32_t>(displacement) & 0x3fffU)
                       << 5;
        break;
      }
    }

    if (literal_patches_.empty())
      return words_;

    /*
     * Reserve the worst possible cold fallback footprint when deciding
     * whether a literal is reachable. This makes the choice conservative and
     * independent of which other pointers need a fallback.
     */
    constexpr std::size_t maximum_fallback_words = 5;
    const auto conservative_pool_start =
        (words_.size() +
         literal_patches_.size() * maximum_fallback_words + 1U) &
        ~std::size_t{1};
    std::vector<bool> literal_mask(literal_patches_.size());
    std::size_t literal_count = 0;
    for (std::size_t index = 0; index < literal_patches_.size(); ++index) {
      const auto source = literal_patches_[index].word;
      const auto target = conservative_pool_start + index * 2U;
      const bool use_literal =
          target >= source && target - source < literal_range_words_;
      literal_mask[index] = use_literal;
      literal_count += use_literal;
    }

    const auto encode_unconditional_branch =
        [](std::size_t source, std::size_t target,
           std::uint32_t &instruction) noexcept {
          const auto displacement = static_cast<std::int64_t>(target) -
                                    static_cast<std::int64_t>(source);
          if (displacement < -(std::int64_t{1} << 25U) ||
              displacement >= (std::int64_t{1} << 25U))
            return false;
          instruction =
              0x1400'0000U |
              (static_cast<std::uint32_t>(displacement) & 0x03ff'ffffU);
          return true;
        };

    /*
     * Emit out-of-range materializations as executable cold stubs before
     * the data tail. Both branches are immediate and translation-owned.
     */
    for (std::size_t index = 0; index < literal_patches_.size(); ++index) {
      if (literal_mask[index])
        continue;
      const auto patch = literal_patches_[index];
      const auto fallback = words_.size();
      if (!encode_unconditional_branch(patch.word, fallback,
                                       words_[patch.word]))
        return {};
      load_constant(patch.destination, patch.value);
      std::uint32_t resume{};
      if (!encode_unconditional_branch(words_.size(), patch.word + 1U,
                                       resume))
        return {};
      emit(resume);
    }
    if (literal_count == 0)
      return words_;

    /*
     * Code storage starts at a 16-byte boundary, so an even word index gives
     * every 64-bit literal its required alignment. Padding and literals are
     * deliberately appended after the final cold branch/return and are never
     * executable branch targets.
     */
    if ((words_.size() & 1U) != 0U)
      emit(0);
    for (std::size_t index = 0; index < literal_patches_.size(); ++index) {
      if (!literal_mask[index])
        continue;
      const auto patch = literal_patches_[index];
      const auto target = words_.size();
      const auto displacement = static_cast<std::int64_t>(target) -
                                static_cast<std::int64_t>(patch.word);
      if (displacement < -(std::int64_t{1} << 18U) ||
          displacement >= (std::int64_t{1} << 18U) ||
          static_cast<std::size_t>(displacement) >= literal_range_words_)
        return {};
      words_[patch.word] =
          0x5800'0000U |
          ((static_cast<std::uint32_t>(displacement) & 0x7ffffU) << 5U) |
          patch.destination;
      emit(static_cast<std::uint32_t>(patch.value));
      emit(static_cast<std::uint32_t>(patch.value >> 32U));
    }
    return words_;
  }

private:
  static constexpr std::size_t unbound =
      std::numeric_limits<std::size_t>::max();

  void patch(Label label, BranchKind kind, std::uint32_t instruction) {
    patches_.push_back({words_.size(), label, kind});
    emit(instruction);
  }

  std::vector<std::uint32_t> words_;
  std::vector<std::size_t> labels_;
  std::vector<Patch> patches_;
  std::vector<StablePointerPatch> literal_patches_;
  std::size_t literal_range_words_{};
  bool persistent_linked_block_abi_{};
  void *exit_context_{};
  ExitFlush exit_flush_{};
  void *flag_context_{};
  FlagClobber flag_clobber_{};
};

[[nodiscard]] constexpr std::size_t
register_offset(unsigned reg, bool stack_pointer) noexcept {
  return reg == 31U && stack_pointer ? offsetof(CpuState, sp)
                                     : reg * sizeof(std::uint64_t);
}

} // namespace

namespace jit {

void JitCompilerFamilies::attach_family_emitter(void *emitter,
                                                bool *native_fp,
                                                bool *native_fpmr,
                                                bool enable_gpr_cache) noexcept {
  family_emitter_ = emitter;
  family_native_fp_ = native_fp;
  family_native_fpmr_ = native_fpmr;
  gpr_cache_enabled_ = enable_gpr_cache;
}

void JitCompilerFamilies::attach_link_resolver(
    void *context, LinkResolver resolver) noexcept {
  link_context_ = context;
  link_resolver_ = resolver;
}

void JitCompilerFamilies::attach_indirect_link_resolver(
    void *context, IndirectLinkResolver resolver) noexcept {
  indirect_link_context_ = context;
  indirect_link_resolver_ = resolver;
}

void JitCompilerFamilies::begin_gpr_residency_observation(
    bool enabled) noexcept {
  gpr_residency_observation_enabled_ = enabled && gpr_cache_enabled_;
  gpr_residency_next_access_ = 0;
  gpr_residency_guests_.fill(GprResidencyGuest{});
}

void JitCompilerFamilies::observe_gpr_access(
    unsigned guest, bool stack_pointer, bool reads, bool writes) noexcept {
  if (!gpr_residency_observation_enabled_ || guest > 31U ||
      (guest == 31U && !stack_pointer) || (!reads && !writes))
    return;

  auto &observation = gpr_residency_guests_[guest];
  if (observation.first_access == GprResidencyGuest::no_first_access)
    observation.first_access = gpr_residency_next_access_;
  ++gpr_residency_next_access_;
  if (reads) {
    ++observation.reads;
    if (!observation.written)
      observation.read_before_write = true;
  }
  if (writes) {
    ++observation.writes;
    observation.written = true;
  }
}

std::size_t JitCompilerFamilies::residency_benefit(
    const GprResidencyGuest &guest) noexcept {
  if (guest.first_access == GprResidencyGuest::no_first_access)
    return 0;
  const auto fallback_accesses = guest.reads + guest.writes;
  const auto resident_accesses =
      (guest.read_before_write ? 1U : 0U) +
      (guest.written ? 1U : 0U);
  return fallback_accesses > resident_accesses
             ? fallback_accesses - resident_accesses
             : 0U;
}

JitCompilerFamilies::GprResidencyPlan
JitCompilerFamilies::finish_gpr_residency_observation() const noexcept {
  GprResidencyPlan result{};
  if (!gpr_residency_observation_enabled_)
    return result;

  std::size_t observed_guests = 0;
  for (const auto &guest : gpr_residency_guests_)
    observed_guests +=
        guest.first_access != GprResidencyGuest::no_first_access;
  if (observed_guests <= resident_gpr_hosts.size())
    return result;

  std::array<bool, 32> selected{};
  std::size_t selected_benefit = 0;
  for (std::size_t slot = 0; slot < resident_gpr_hosts.size(); ++slot) {
    unsigned best = unbound_guest;
    for (unsigned guest = 0; guest < gpr_residency_guests_.size(); ++guest) {
      const auto &candidate = gpr_residency_guests_[guest];
      if (selected[guest] ||
          candidate.first_access == GprResidencyGuest::no_first_access)
        continue;
      if (best == unbound_guest ||
          residency_benefit(candidate) >
              residency_benefit(gpr_residency_guests_[best]) ||
          (residency_benefit(candidate) ==
               residency_benefit(gpr_residency_guests_[best]) &&
           candidate.first_access <
               gpr_residency_guests_[best].first_access))
        best = guest;
    }
    if (best == unbound_guest)
      break;
    selected[best] = true;
    selected_benefit += residency_benefit(gpr_residency_guests_[best]);
    ++result.count;
  }

  if (result.count != resident_gpr_hosts.size())
    return {};

  std::size_t current_benefit = 0;
  for (const auto &slot : gpr_slots_)
    if (slot.guest != unbound_guest)
      current_benefit +=
          residency_benefit(gpr_residency_guests_[slot.guest]);
  if (selected_benefit <= current_benefit)
    return {};

  /*
   * Assign the selected identities in encounter order. This keeps the host
   * register numbering deterministic and preserves the existing first-use
   * shape among identities that survive the pressure decision.
   */
  for (std::size_t index = 0; index < result.count; ++index) {
    unsigned first = unbound_guest;
    for (unsigned guest = 0; guest < selected.size(); ++guest) {
      if (!selected[guest])
        continue;
      if (first == unbound_guest ||
          gpr_residency_guests_[guest].first_access <
              gpr_residency_guests_[first].first_access)
        first = guest;
    }
    result.guests[index] = first;
    selected[first] = false;
  }
  result.estimated_avoided_accesses =
      selected_benefit - current_benefit;
  return result;
}

void JitCompilerFamilies::apply_gpr_residency_plan(
    const GprResidencyPlan &plan) noexcept {
  if (!gpr_cache_enabled_ || plan.count != resident_gpr_hosts.size())
    return;
  std::array<bool, 32> seen{};
  for (std::size_t index = 0; index < plan.count; ++index) {
    if (plan.guests[index] >= seen.size() || seen[plan.guests[index]])
      return;
    seen[plan.guests[index]] = true;
  }
  for (std::size_t index = 0; index < plan.count; ++index) {
    gpr_slots_[index].guest = plan.guests[index];
    gpr_slots_[index].stack_pointer = plan.guests[index] == 31U;
  }
}

bool JitCompilerFamilies::gpr_slots_unbound() const noexcept {
  return gpr_cache_enabled_ &&
         std::ranges::all_of(gpr_slots_, [](const auto &slot) {
           return slot.guest == unbound_guest;
         });
}

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
void JitCompilerFamilies::attach_gpr_link_profile(
    void *context, DirectLinkProfileResolver direct_resolver,
    IndirectLinkProfileResolver indirect_resolver) noexcept {
  gpr_link_profile_context_ = context;
  direct_link_profile_resolver_ = direct_resolver;
  indirect_link_profile_resolver_ = indirect_resolver;
}

void JitCompilerFamilies::begin_gpr_link_profile_block(
    std::uint64_t pc) noexcept {
  profile_block_pc_ = pc;
  profile_gpr_read_before_write_ = 0;
  profile_gpr_written_ = 0;
}

std::uint32_t
JitCompilerFamilies::gpr_profile_read_before_write_mask() const noexcept {
  return profile_gpr_read_before_write_;
}

std::uint32_t
JitCompilerFamilies::gpr_profile_written_mask() const noexcept {
  return profile_gpr_written_;
}

std::uint32_t
JitCompilerFamilies::gpr_profile_dirty_live_out_mask() const noexcept {
  std::uint32_t result{};
  for (const auto &slot : gpr_slots_)
    if (slot.dirty &&
        (slot.guest < 31U ||
         (slot.guest == 31U && slot.stack_pointer)))
      result |= std::uint32_t{1} << slot.guest;
  return result;
}

void JitCompilerFamilies::profile_gpr_read(unsigned guest,
                                           bool stack_pointer) noexcept {
  if (guest > 31U || (guest == 31U && !stack_pointer))
    return;
  const auto bit = std::uint32_t{1} << guest;
  if ((profile_gpr_written_ & bit) == 0)
    profile_gpr_read_before_write_ |= bit;
}

void JitCompilerFamilies::profile_gpr_write(unsigned guest,
                                            bool stack_pointer) noexcept {
  if (guest > 31U || (guest == 31U && !stack_pointer))
    return;
  profile_gpr_written_ |= std::uint32_t{1} << guest;
}
#endif

#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
void JitCompilerFamilies::attach_gpr_pressure_profile(bool enabled) noexcept {
  gpr_pressure_profile_enabled_ = enabled;
}

void JitCompilerFamilies::begin_gpr_pressure_profile_block() noexcept {
  gpr_pressure_next_rank_ = 0;
  gpr_pressure_profile_ = {};
  gpr_pressure_guests_.fill(GprPressureGuest{});
}

void JitCompilerFamilies::profile_gpr_slot_allocation(
    std::size_t slots) noexcept {
  if (!gpr_pressure_profile_enabled_)
    return;
  gpr_pressure_profile_.slot_high_water =
      std::max(gpr_pressure_profile_.slot_high_water, slots);
}

JitCompilerFamilies::GprPressureGuest *
JitCompilerFamilies::find_profiled_overflow_guest(
    unsigned guest, bool stack_pointer) noexcept {
  if (!gpr_pressure_profile_enabled_ || guest > 31U ||
      (guest == 31U && !stack_pointer))
    return nullptr;
  auto &profile = gpr_pressure_guests_[guest];
  return profile.rank == GprPressureGuest::no_rank ? nullptr : &profile;
}

void JitCompilerFamilies::profile_gpr_overflow_native_access(
    unsigned guest, bool stack_pointer, bool reads_old,
    bool writes) noexcept {
  if (!gpr_pressure_profile_enabled_ || !gpr_cache_enabled_ ||
      guest > 31U ||
      (guest == 31U && !stack_pointer))
    return;
  auto &profile = gpr_pressure_guests_[guest];
  if (profile.rank == GprPressureGuest::no_rank) {
    profile.rank = static_cast<unsigned>(gpr_pressure_next_rank_++);
  } else {
    ++gpr_pressure_profile_.repeated_overflow_accesses;
  }
  ++gpr_pressure_profile_.allocation_refusals;
  if (reads_old) {
    ++profile.fallback_read_loads;
    ++gpr_pressure_profile_.fallback_read_loads;
    if (!profile.resident_value_available)
      ++profile.required_resident_accesses;
    profile.resident_value_available = true;
  }
  if (writes) {
    profile.resident_value_available = true;
    profile.dirty = true;
  }
}

void JitCompilerFamilies::profile_gpr_overflow_nonallocating_access(
    unsigned guest, bool stack_pointer, bool reads_old,
    bool writes) noexcept {
  if (!gpr_pressure_profile_enabled_ || !gpr_cache_enabled_ ||
      guest > 31U || (guest == 31U && !stack_pointer))
    return;
  auto &profile = gpr_pressure_guests_[guest];
  if (profile.rank == GprPressureGuest::no_rank) {
    const bool bank_full = std::ranges::all_of(
        gpr_slots_, [](const auto &slot) {
          return slot.guest != unbound_guest;
        });
    if (!bank_full)
      return;
    profile.rank = static_cast<unsigned>(gpr_pressure_next_rank_++);
  } else {
    ++gpr_pressure_profile_.repeated_overflow_accesses;
  }
  if (reads_old) {
    ++profile.fallback_read_loads;
    ++gpr_pressure_profile_.fallback_read_loads;
    if (!profile.resident_value_available)
      ++profile.required_resident_accesses;
    profile.resident_value_available = true;
  }
  if (writes) {
    ++profile.fallback_write_stores;
    ++gpr_pressure_profile_.fallback_write_stores;
    profile.resident_value_available = true;
    profile.dirty = true;
  }
}

void JitCompilerFamilies::profile_gpr_overflow_fallback_store(
    unsigned guest, bool stack_pointer) noexcept {
  auto *profile =
      find_profiled_overflow_guest(guest, stack_pointer);
  if (profile == nullptr)
    return;
  ++profile->fallback_write_stores;
  ++gpr_pressure_profile_.fallback_write_stores;
}

JitGprPressureProfile
JitCompilerFamilies::finish_gpr_pressure_profile_block() const noexcept {
  auto result = gpr_pressure_profile_;
  if (!gpr_pressure_profile_enabled_)
    return result;
  for (const auto &guest : gpr_pressure_guests_) {
    if (guest.rank == GprPressureGuest::no_rank)
      continue;
    const auto actual =
        guest.fallback_read_loads + guest.fallback_write_stores;
    const auto required =
        guest.required_resident_accesses + (guest.dirty ? 1U : 0U);
    const auto avoided = actual > required ? actual - required : 0U;
    for (std::size_t index = 0;
         index < jit_gpr_pressure_candidate_capacities.size(); ++index) {
      const auto extra_slots =
          jit_gpr_pressure_candidate_capacities[index] -
          resident_gpr_hosts.size();
      if (guest.rank < extra_slots)
        result.estimated_avoided_accesses[index] += avoided;
    }
  }
  return result;
}
#endif

void JitCompilerFamilies::emit(std::uint32_t instruction) {
  static_cast<Arm64Emitter *>(family_emitter_)->emit(instruction);
}

JitCompilerFamilies::Label JitCompilerFamilies::make_label() {
  return static_cast<Arm64Emitter *>(family_emitter_)->make_label();
}

void JitCompilerFamilies::bind(Label label) {
  static_cast<Arm64Emitter *>(family_emitter_)->bind(label);
}

void JitCompilerFamilies::branch_condition(Label label, unsigned condition) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->branch_condition(label, condition);
}

void JitCompilerFamilies::compare_zero(Label label, unsigned reg,
                                       bool is_64_bit, bool nonzero) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->compare_zero(label, reg, is_64_bit, nonzero);
}

void JitCompilerFamilies::test_zero(Label label, unsigned reg, unsigned bit,
                                    bool nonzero) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->test_zero(label, reg, bit, nonzero);
}

void JitCompilerFamilies::load_constant(unsigned destination,
                                        std::uint64_t value) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->load_constant(destination, value);
}

void JitCompilerFamilies::load_literal_constant(unsigned destination,
                                                std::uint64_t value) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->load_literal_constant(destination, value);
}

void JitCompilerFamilies::load_x(unsigned destination, unsigned base,
                                 std::size_t offset) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->load_x(destination, base, offset);
}

void JitCompilerFamilies::store_x(unsigned source, unsigned base,
                                  std::size_t offset) {
  static_cast<Arm64Emitter *>(family_emitter_)->store_x(source, base, offset);
}

void JitCompilerFamilies::load_q(unsigned destination, unsigned base,
                                 std::size_t offset) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  constexpr auto vectors = offsetof(arm64::CpuState, vectors);
  constexpr auto vector_bytes =
      sizeof(arm64::CpuState::vectors);
  if (gpr_cache_enabled_ && base == 15U && offset >= vectors &&
      offset < vectors + vector_bytes &&
      ((offset - vectors) % sizeof(arm64::VectorRegister)) == 0U) {
    const auto guest = static_cast<unsigned>(
        (offset - vectors) / sizeof(arm64::VectorRegister));
    const auto slot = find_vector_slot(guest);
    if (slot) {
      auto &binding = vector_slots_[*slot];
      if (!binding.loaded) {
        emitter.load_q(resident_vector_hosts[*slot], 15, offset);
        binding.loaded = true;
      }
      if (resident_vector_hosts[*slot] != destination)
        emitter.move_q(destination, resident_vector_hosts[*slot]);
      return;
    }
  }
  emitter.load_q(destination, base, offset);
}

void JitCompilerFamilies::store_q(unsigned source, unsigned base,
                                  std::size_t offset) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  constexpr auto vectors = offsetof(arm64::CpuState, vectors);
  constexpr auto vector_bytes =
      sizeof(arm64::CpuState::vectors);
  if (gpr_cache_enabled_ && base == 15U && offset >= vectors &&
      offset < vectors + vector_bytes &&
      ((offset - vectors) % sizeof(arm64::VectorRegister)) == 0U) {
    const auto guest = static_cast<unsigned>(
        (offset - vectors) / sizeof(arm64::VectorRegister));
    const auto slot = find_vector_slot(guest);
    if (slot) {
      if (resident_vector_hosts[*slot] != source)
        emitter.move_q(resident_vector_hosts[*slot], source);
      vector_slots_[*slot].loaded = true;
      vector_slots_[*slot].dirty = true;
      return;
    }
  }
  emitter.store_q(source, base, offset);
}

void JitCompilerFamilies::return_constant(Event event,
                                          std::size_t instructions) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->return_constant(event, instructions);
}

void JitCompilerFamilies::return_dynamic(std::size_t instructions) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->return_dynamic(instructions);
}

std::optional<std::size_t>
JitCompilerFamilies::find_gpr_slot(unsigned guest,
                                   bool stack_pointer) const noexcept {
  if (!gpr_cache_enabled_ || (guest == 31U && !stack_pointer))
    return std::nullopt;
  stack_pointer = guest == 31U && stack_pointer;
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index) {
    const auto &slot = gpr_slots_[index];
    if (slot.guest == guest && slot.stack_pointer == stack_pointer)
      return index;
  }
  return std::nullopt;
}

std::optional<std::size_t>
JitCompilerFamilies::allocate_gpr_slot(unsigned guest,
                                       bool stack_pointer) {
  if (!gpr_cache_enabled_ || (guest == 31U && !stack_pointer))
    return std::nullopt;
  stack_pointer = guest == 31U && stack_pointer;
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index) {
    auto &slot = gpr_slots_[index];
    if (slot.guest != unbound_guest)
      continue;
    slot.guest = guest;
    slot.stack_pointer = stack_pointer;
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
    profile_gpr_slot_allocation(index + 1U);
#endif
    return index;
  }
  return std::nullopt;
}

JitCompilerFamilies::NativeGpr
JitCompilerFamilies::read_native_gpr(unsigned guest, bool stack_pointer,
                                     unsigned fallback) {
  observe_gpr_access(guest, stack_pointer, /*reads=*/true,
                     /*writes=*/false);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  profile_gpr_read(guest, stack_pointer);
#endif
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  if (guest == 31U && !stack_pointer)
    return {31U, true};
  auto slot = find_gpr_slot(guest, stack_pointer);
  if (!slot)
    slot = allocate_gpr_slot(guest, stack_pointer);
  if (slot) {
    auto &binding = gpr_slots_[*slot];
    if (!binding.loaded) {
      emitter.load_x(resident_gpr_hosts[*slot], 15,
                     register_offset(guest, stack_pointer));
      binding.loaded = true;
    }
    return {resident_gpr_hosts[*slot], true};
  }
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  profile_gpr_overflow_native_access(guest, stack_pointer,
                                     /*reads_old=*/true,
                                     /*writes=*/false);
#endif
  emitter.load_x(fallback, 15, register_offset(guest, stack_pointer));
  return {fallback, false};
}

JitCompilerFamilies::NativeGpr
JitCompilerFamilies::write_native_gpr(unsigned guest, bool stack_pointer,
                                      bool reads_old, unsigned fallback) {
  observe_gpr_access(guest, stack_pointer, reads_old, /*writes=*/true);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  if (reads_old)
    profile_gpr_read(guest, stack_pointer);
#endif
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  if (guest == 31U && !stack_pointer)
    return {31U, true};
  auto slot = find_gpr_slot(guest, stack_pointer);
  if (!slot)
    slot = allocate_gpr_slot(guest, stack_pointer);
  if (slot) {
    auto &binding = gpr_slots_[*slot];
    if (reads_old && !binding.loaded)
      emitter.load_x(resident_gpr_hosts[*slot], 15,
                     register_offset(guest, stack_pointer));
    binding.loaded = true;
    binding.dirty = true;
    return {resident_gpr_hosts[*slot], true};
  }
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  profile_gpr_overflow_native_access(guest, stack_pointer, reads_old,
                                     /*writes=*/true);
#endif
  if (reads_old)
    emitter.load_x(fallback, 15, register_offset(guest, stack_pointer));
  return {fallback, false};
}

void JitCompilerFamilies::commit_native_gpr(NativeGpr value, unsigned guest,
                                            bool stack_pointer) {
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  profile_gpr_write(guest, stack_pointer);
#endif
  if (guest == 31U && !stack_pointer)
    return;
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  if (!value.resident)
    profile_gpr_overflow_fallback_store(guest, stack_pointer);
#endif
  if (!value.resident)
    static_cast<Arm64Emitter *>(family_emitter_)
        ->store_x(value.host, 15, register_offset(guest, stack_pointer));
}

std::optional<std::size_t>
JitCompilerFamilies::find_vector_slot(unsigned guest) const noexcept {
  if (!gpr_cache_enabled_)
    return std::nullopt;
  for (std::size_t index = 0; index < vector_slots_.size(); ++index)
    if (vector_slots_[index].guest == guest)
      return index;
  return std::nullopt;
}

std::optional<unsigned> JitCompilerFamilies::constrained_vector_source(
    std::uint32_t instruction) const noexcept {
  if (!floating_or_vector_primary_class(instruction))
    return std::nullopt;

  const auto &host = host_arm_features();

  const auto half_long =
      arm64::detail::decode_half_fused_long(instruction);
  if (half_long.recognized && half_long.by_element &&
      host.has(ArmFeature::fhm) && host.has(ArmFeature::afp))
    return (instruction >> 16U) & 15U;

  const auto bfloat_long =
      arm64::detail::decode_bfloat_multiply_long(instruction);
  if (bfloat_long.recognized && bfloat_long.by_element &&
      host.has(ArmFeature::bf16))
    return (instruction >> 16U) & 15U;

  const auto fp8 = arm64::detail::decode_fp8(instruction);
  if (fp8.recognized && fp8.valid && fp8.by_element) {
    using Operation = arm64::detail::Fp8Operation;
    if (fp8.operation == Operation::dot_half &&
        host.has(ArmFeature::fp8dot2))
      return fp8.second;
    if ((fp8.operation == Operation::multiply_half ||
         fp8.operation == Operation::multiply_single) &&
        host.has(ArmFeature::fp8fma))
      return fp8.second;
  }

  const unsigned size = (instruction >> 22U) & 3U;
  if (size == 1U && valid_simd_integer_indexed(instruction) &&
      host.advanced_simd) {
    const bool unsigned_group =
        (instruction & 0x2000'0000U) != 0U;
    const unsigned operation = (instruction >> 12U) & 15U;
    const bool requires_rdm =
        unsigned_group && (operation == 13U || operation == 15U);
    if (!requires_rdm || host.has(ArmFeature::rdm))
      return (instruction >> 16U) & 15U;
  }

  if (size == 0U && valid_simd_floating_indexed(instruction) &&
      host.floating_point && host.advanced_simd && host.fp16)
    return (instruction >> 16U) & 15U;

  return std::nullopt;
}

void JitCompilerFamilies::pin_constrained_vector(unsigned guest) noexcept {
  if (!gpr_cache_enabled_ || guest >= 32U)
    return;
  if (find_vector_slot(guest))
    return;
  for (std::size_t index = 0; index < pinned_vector_slot_count; ++index) {
    auto &slot = vector_slots_[index];
    if (slot.guest != unbound_guest)
      continue;
    slot.guest = guest;
    return;
  }
}

std::optional<JitCompilerFamilies::ConsecutiveVectorList>
JitCompilerFamilies::consecutive_vector_list(
    std::uint32_t instruction) const noexcept {
  const auto &host = host_arm_features();
  if (!host.advanced_simd)
    return std::nullopt;

  if ((instruction & 0xff20'8c00U) == 0x4e00'0000U &&
      (instruction & 0x00c0'0000U) != 0U) {
    if (!host.has(ArmFeature::lut))
      return std::nullopt;
    const unsigned op2 = (instruction >> 22U) & 3U;
    const unsigned len = (instruction >> 13U) & 3U;
    const unsigned op = (instruction >> 12U) & 1U;
    const bool valid =
        op2 == 3U || (op2 == 2U && op != 0U) ||
        (op2 == 1U && (op != 0U || (len & 1U) != 0U));
    if (!valid || op2 != 1U || op == 0U)
      return std::nullopt;
    return ConsecutiveVectorList{
        .first = (instruction >> 5U) & 31U,
        .count = 2U,
    };
  }

  if ((instruction & 0xbfe0'8c00U) != 0x0e00'0000U)
    return std::nullopt;
  const unsigned count = ((instruction >> 13U) & 3U) + 1U;
  if (count == 1U)
    return std::nullopt;
  return ConsecutiveVectorList{
      .first = (instruction >> 5U) & 31U,
      .count = count,
  };
}

bool JitCompilerFamilies::pin_consecutive_vector_list(
    unsigned first, unsigned count) noexcept {
  if (!gpr_cache_enabled_ || first >= 32U || count < 2U ||
      count > 4U || count > vector_slots_.size() - pinned_vector_slot_count)
    return false;

  /*
   * q6/q7 remain dedicated to the more restrictive indexed encodings.
   * Search q16-q31 from the high end so ordinary first-use allocation keeps
   * its existing q16-up order. Every compatible binding is resolved here;
   * the generated table operation contains no staging decision or branch.
   */
  const auto first_start = vector_slots_.size() - count;
  for (std::size_t start = first_start + 1U;
       start-- > pinned_vector_slot_count;) {
    bool compatible = true;
    for (unsigned member = 0; member < count; ++member) {
      const unsigned guest = (first + member) & 31U;
      const auto existing = find_vector_slot(guest);
      if ((existing && *existing != start + member) ||
          (vector_slots_[start + member].guest != unbound_guest &&
           vector_slots_[start + member].guest != guest)) {
        compatible = false;
        break;
      }
      if (member != 0U &&
          resident_vector_hosts[start + member] !=
              resident_vector_hosts[start] + member) {
        compatible = false;
        break;
      }
    }
    if (!compatible)
      continue;
    for (unsigned member = 0; member < count; ++member)
      vector_slots_[start + member].guest = (first + member) & 31U;
    return true;
  }
  return false;
}

std::optional<std::size_t>
JitCompilerFamilies::allocate_vector_slot(unsigned guest) {
  if (!gpr_cache_enabled_)
    return std::nullopt;
  /*
   * Keep the unrestricted low-pressure shape stable by consuming q16-q31
   * first.  q6/q7 are prebound to constrained identities before emission;
   * any low slot left unbound after that planning pass is ordinary resident
   * capacity once the high bank is full.
   */
  for (std::size_t index = pinned_vector_slot_count;
       index < vector_slots_.size(); ++index) {
    auto &slot = vector_slots_[index];
    if (slot.guest != unbound_guest)
      continue;
    slot.guest = guest;
    return index;
  }
  for (std::size_t index = 0; index < pinned_vector_slot_count; ++index) {
    auto &slot = vector_slots_[index];
    if (slot.guest != unbound_guest)
      continue;
    slot.guest = guest;
    return index;
  }
  return std::nullopt;
}

JitCompilerFamilies::NativeVector
JitCompilerFamilies::read_native_vector(unsigned guest,
                                        unsigned fallback) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  auto slot = find_vector_slot(guest);
  if (!slot)
    slot = allocate_vector_slot(guest);
  const auto offset =
      offsetof(arm64::CpuState, vectors) +
      guest * sizeof(arm64::VectorRegister);
  if (slot) {
    auto &binding = vector_slots_[*slot];
    if (!binding.loaded) {
      emitter.load_q(resident_vector_hosts[*slot], 15, offset);
      binding.loaded = true;
    }
    return {resident_vector_hosts[*slot], true};
  }
  emitter.load_q(fallback, 15, offset);
  return {fallback, false};
}

JitCompilerFamilies::NativeVector
JitCompilerFamilies::read_constrained_native_vector(unsigned guest,
                                                    unsigned fallback) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  const auto offset =
      offsetof(arm64::CpuState, vectors) +
      guest * sizeof(arm64::VectorRegister);
  const auto slot = find_vector_slot(guest);
  if (slot && *slot < pinned_vector_slot_count) {
    auto &binding = vector_slots_[*slot];
    if (!binding.loaded) {
      emitter.load_q(resident_vector_hosts[*slot], 15, offset);
      binding.loaded = true;
    }
    return {resident_vector_hosts[*slot], true};
  }

  /*
   * A third constrained identity deliberately retains the established v2
   * fallback rather than shortening the block. load_q() is cache-aware, so
   * an earlier resident write is copied from its live host binding instead
   * of reloading stale CpuState.
   */
  load_q(fallback, 15, offset);
  return {fallback, false};
}

JitCompilerFamilies::NativeVector
JitCompilerFamilies::write_native_vector(unsigned guest, bool reads_old,
                                         unsigned fallback) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  auto slot = find_vector_slot(guest);
  if (!slot)
    slot = allocate_vector_slot(guest);
  const auto offset =
      offsetof(arm64::CpuState, vectors) +
      guest * sizeof(arm64::VectorRegister);
  if (slot) {
    auto &binding = vector_slots_[*slot];
    if (reads_old && !binding.loaded)
      emitter.load_q(resident_vector_hosts[*slot], 15, offset);
    binding.loaded = true;
    binding.dirty = true;
    return {resident_vector_hosts[*slot], true};
  }
  if (reads_old)
    emitter.load_q(fallback, 15, offset);
  return {fallback, false};
}

void JitCompilerFamilies::commit_native_vector(NativeVector value,
                                               unsigned guest) {
  if (value.resident)
    return;
  static_cast<Arm64Emitter *>(family_emitter_)
      ->store_q(value.host, 15,
                offsetof(arm64::CpuState, vectors) +
                    guest * sizeof(arm64::VectorRegister));
}

void JitCompilerFamilies::load_gpr(unsigned host, unsigned guest,
                                   bool stack_pointer) {
  observe_gpr_access(guest, stack_pointer, /*reads=*/true,
                     /*writes=*/false);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  profile_gpr_read(guest, stack_pointer);
#endif
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  if (guest == 31U && !stack_pointer) {
    emitter.move_x(host, 31);
    return;
  }
  const auto slot = find_gpr_slot(guest, stack_pointer);
  if (slot) {
    if (!gpr_slots_[*slot].loaded) {
      emitter.load_x(resident_gpr_hosts[*slot], 15,
                     register_offset(guest, stack_pointer));
      gpr_slots_[*slot].loaded = true;
    }
    if (resident_gpr_hosts[*slot] != host)
      emitter.move_x(host, resident_gpr_hosts[*slot]);
    return;
  }
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  profile_gpr_overflow_nonallocating_access(
      guest, stack_pointer, /*reads_old=*/true, /*writes=*/false);
#endif
  emitter.load_x(host, 15, register_offset(guest, stack_pointer));
}

void JitCompilerFamilies::store_gpr(unsigned host, unsigned guest,
                                    bool stack_pointer) {
  observe_gpr_access(guest, stack_pointer, /*reads=*/false,
                     /*writes=*/true);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  profile_gpr_write(guest, stack_pointer);
#endif
  if (guest == 31U && !stack_pointer)
    return;
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  const auto slot = find_gpr_slot(guest, stack_pointer);
  if (slot) {
    if (resident_gpr_hosts[*slot] != host)
      emitter.move_x(resident_gpr_hosts[*slot], host);
    gpr_slots_[*slot].loaded = true;
    gpr_slots_[*slot].dirty = true;
    return;
  }
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  profile_gpr_overflow_nonallocating_access(
      guest, stack_pointer, /*reads_old=*/false, /*writes=*/true);
#endif
  emitter.store_x(host, 15, register_offset(guest, stack_pointer));
}

std::uint16_t JitCompilerFamilies::dirty_gpr_mask() const noexcept {
  std::uint16_t mask = 0;
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index)
    if (gpr_slots_[index].dirty)
      mask |= static_cast<std::uint16_t>(1U << index);
  return mask;
}

std::uint32_t JitCompilerFamilies::dirty_vector_mask() const noexcept {
  std::uint32_t mask = 0;
  for (std::size_t index = 0; index < vector_slots_.size(); ++index)
    if (vector_slots_[index].dirty)
      mask |= std::uint32_t{1} << index;
  return mask;
}

void JitCompilerFamilies::flush_gprs(std::uint16_t mask) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index) {
    const auto &slot = gpr_slots_[index];
    if ((mask & (1U << index)) == 0U || slot.guest == unbound_guest)
      continue;
    emitter.store_x(resident_gpr_hosts[index], 15,
                    register_offset(slot.guest, slot.stack_pointer));
  }
}

void JitCompilerFamilies::flush_vectors(std::uint32_t mask) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  for (std::size_t index = 0; index < vector_slots_.size(); ++index) {
    const auto &slot = vector_slots_[index];
    if ((mask & (1U << index)) == 0U || slot.guest == unbound_guest)
      continue;
    emitter.store_q(
        resident_vector_hosts[index], 15,
        offsetof(arm64::CpuState, vectors) +
            slot.guest * sizeof(arm64::VectorRegister));
  }
}

void JitCompilerFamilies::set_exit_gpr_mask(std::uint16_t mask) noexcept {
  exit_gpr_mask_ = mask;
  exit_gpr_mask_set_ = true;
}

void JitCompilerFamilies::set_exit_vector_mask(
    std::uint32_t mask) noexcept {
  exit_vector_mask_ = mask;
  exit_vector_mask_set_ = true;
}

void JitCompilerFamilies::flush_exit_gprs() {
  const auto gpr_mask =
      exit_gpr_mask_set_ ? exit_gpr_mask_ : dirty_gpr_mask();
  const auto vector_mask =
      exit_vector_mask_set_ ? exit_vector_mask_ : dirty_vector_mask();
  flush_gprs(gpr_mask);
  flush_vectors(vector_mask);
  /*
   * Exit code can be emitted on one arm of a generated branch.  Publish a
   * dirty host NZCV for that arm, but deliberately retain the translation
   * state: the other arm did not execute this store and may continue using
   * the live guest flags.
  */
  flush_flags_for_exit();
  exit_gpr_mask_set_ = false;
  exit_vector_mask_set_ = false;
}

void JitCompilerFamilies::spill_gprs_for_returning_call() {
  flush_gprs(dirty_gpr_mask());
  flush_vectors(dirty_vector_mask());
  spill_flags_for_internal_clobber();
}

void JitCompilerFamilies::reload_gprs_after_call() {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index) {
    const auto &slot = gpr_slots_[index];
    if (slot.guest == unbound_guest)
      continue;
    emitter.load_x(resident_gpr_hosts[index], 15,
                   register_offset(slot.guest, slot.stack_pointer));
  }
  for (std::size_t index = 0; index < vector_slots_.size(); ++index) {
    const auto &slot = vector_slots_[index];
    if (slot.guest == unbound_guest || !slot.loaded)
      continue;
    emitter.load_q(
        resident_vector_hosts[index], 15,
        offsetof(arm64::CpuState, vectors) +
            slot.guest * sizeof(arm64::VectorRegister));
  }
}

void JitCompilerFamilies::spill_gprs_for_terminal_call() {
  const auto gpr_mask =
      exit_gpr_mask_set_ ? exit_gpr_mask_ : dirty_gpr_mask();
  const auto vector_mask =
      exit_vector_mask_set_ ? exit_vector_mask_ : dirty_vector_mask();
  flush_gprs(gpr_mask);
  flush_vectors(vector_mask);
  spill_flags_for_internal_clobber();
  /*
   * The return emitter still invokes the centralized exit hook. Leave an
   * explicit empty mask so it cannot flush bindings allocated on a later hot
   * path into an earlier cold exit.
   */
  exit_gpr_mask_ = 0;
  exit_gpr_mask_set_ = true;
  exit_vector_mask_ = 0;
  exit_vector_mask_set_ = true;
}

void JitCompilerFamilies::flush_and_invalidate_gprs() {
  flush_gprs(dirty_gpr_mask());
  /*
   * Keep every slot's architectural identity stable for cold exits recorded
   * before this point. The constrained native sequence may overwrite the host
   * value, so subsequent hot-path users must reload it from the state just
   * published above.
   */
  for (auto &slot : gpr_slots_) {
    slot.loaded = false;
    slot.dirty = false;
  }
}

void JitCompilerFamilies::store_pc(std::uint64_t pc) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  emitter.load_literal_constant(8, pc);
  emitter.store_x(8, 15, offsetof(arm64::CpuState, pc));
}

void JitCompilerFamilies::load_flags() {
  if (guest_flags_state_ != GuestFlagsState::memory)
    return;
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  emitter.load_w(11, 15, offsetof(arm64::CpuState, nzcv));
  emitter.emit(0xd51b'420bU); // msr nzcv, x11
  guest_flags_state_ = GuestFlagsState::synchronized;
}

void JitCompilerFamilies::store_flags() {
  /* A native guest instruction has just produced the complete guest NZCV. */
  guest_flags_state_ = GuestFlagsState::host_dirty;
}

void JitCompilerFamilies::flush_flags_for_exit() {
  if (guest_flags_state_ != GuestFlagsState::host_dirty)
    return;
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  emitter.emit(0xd53b'420bU); // mrs x11, nzcv
  emitter.store_w(11, 15, offsetof(arm64::CpuState, nzcv));
}

void JitCompilerFamilies::synchronize_flags() {
  flush_flags_for_exit();
  if (guest_flags_state_ == GuestFlagsState::host_dirty)
    guest_flags_state_ = GuestFlagsState::synchronized;
}

void JitCompilerFamilies::spill_flags_for_internal_clobber() {
  flush_flags_for_exit();
  guest_flags_state_ = GuestFlagsState::memory;
}

void JitCompilerFamilies::assume_flags_in_memory() noexcept {
  guest_flags_state_ = GuestFlagsState::memory;
}

JitCompilerFamilies::GuestFlagsState
JitCompilerFamilies::snapshot_flags() const noexcept {
  return guest_flags_state_;
}

void JitCompilerFamilies::publish_cold_flags(GuestFlagsState state) {
  if (state != GuestFlagsState::host_dirty)
    return;
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  emitter.emit(0xd53b'420bU); // mrs x11, nzcv
  emitter.store_w(11, 15, offsetof(arm64::CpuState, nzcv));
}

void JitCompilerFamilies::read_flags(unsigned destination) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  if (guest_flags_state_ == GuestFlagsState::memory) {
    emitter.load_w(destination, 15, offsetof(arm64::CpuState, nzcv));
    return;
  }
  emitter.emit(0xd53b'4200U | (destination & 31U)); // mrs Xd, nzcv
}

void JitCompilerFamilies::mark_native_fp() noexcept {
  *family_native_fp_ = true;
}

void JitCompilerFamilies::mark_native_fpmr() noexcept {
  *family_native_fpmr_ = true;
}

void JitCompilerFamilies::emit_linked_exit(std::uint64_t target,
                                           std::size_t count) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  std::uint64_t *profile_counter{};
  const auto profile_dirty_live_out = gpr_profile_dirty_live_out_mask();
#endif
  flush_exit_gprs();
  /*
   * Unlike an exit emitted on only one branch arm, every path entering this
   * linked-exit sequence executes the flush above.  CpuState is therefore
   * canonical before the publication/fairness comparisons take ownership of
   * host NZCV.
   */
  assume_flags_in_memory();
  set_exit_gpr_mask(0);
  set_exit_vector_mask(0);
  const auto *link =
      link_resolver_ == nullptr
          ? nullptr
          : static_cast<const JitBlockLink *>(
                link_resolver_(link_context_, target));
  if (link == nullptr) {
    store_pc(target);
    emitter.return_constant(Event::continued, count);
    return;
  }
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  if (direct_link_profile_resolver_ != nullptr) {
    const auto source_pc =
        profile_block_pc_ +
        (count == 0 ? 0 : (count - 1) * sizeof(std::uint32_t));
    profile_counter = direct_link_profile_resolver_(
        gpr_link_profile_context_, link, source_pc, profile_dirty_live_out);
  }
#endif

  const auto fallback = emitter.make_label();
  emitter.load_stable_pointer(
      12, reinterpret_cast<std::uintptr_t>(link->entries.data()));
  if (emitter.persistent_linked_block_abi()) {
    /*
     * Subtract the completed source while the descriptor pointer load is in
     * flight. Both a missing capability and a full fairness window take the
     * shared cold undo below, so only successful links commit the decrement.
     * x21 is initialized once from the host-owned capability selector; guest
     * data never contributes to the indexed native branch target.
     */
    emitter.subtract_immediate(20, 20, static_cast<unsigned>(count));
    emitter.load_x_indexed_8(13, 12, 21);
  } else {
    emitter.load_x(11, 31, 32); // Saved JitChainState *.
    emitter.load_b(9, 11, offsetof(JitChainState, link_selector));
    emitter.load_x_indexed_8(13, 12, 9);
  }
  emitter.load_pair_x(
      16, 9, 12,
      -static_cast<std::ptrdiff_t>(offsetof(JitBlockLink, entries)));
  emitter.compare_zero(fallback, 13, true, false);

  if (emitter.persistent_linked_block_abi()) {
    emitter.compare_register(20, 9);
  } else {
    emitter.load_x(10, 11, offsetof(JitChainState, remaining_instructions));
    emitter.subtract_immediate(10, 10, static_cast<unsigned>(count));
    emitter.compare_register(10, 9);
  }
  emitter.branch_condition(fallback, 3); // LO

  if (!emitter.persistent_linked_block_abi())
    emitter.store_x(10, 11, offsetof(JitChainState, remaining_instructions));
  /*
   * x15/x14 still hold CpuState/TLB, and the outer frame owns all chain state
   * for both generated ABI variants. Enter the successor after its external
   * prologue so a successful link executes no argument reload and creates no
   * extra native frame. Every successor exit therefore returns through the
   * one frame established by the initial C++ entry.
   */
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  if (profile_counter != nullptr) {
    emitter.load_stable_pointer(
        9, reinterpret_cast<std::uintptr_t>(profile_counter));
    emitter.load_x(8, 9, 0);
    emitter.add_immediate(8, 8, 1);
    emitter.store_x(8, 9, 0);
  }
#endif
  emitter.branch_register(13);

  emitter.bind(fallback);
  if (emitter.persistent_linked_block_abi())
    emitter.add_immediate(20, 20, static_cast<unsigned>(count));
  store_pc(target);
  emitter.return_constant(Event::continued, count);
}

void JitCompilerFamilies::emit_indirect_linked_exit(
    unsigned target_host, bool writes_link, std::uint64_t source_pc,
    std::size_t count) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  /*
   * Preserve the architectural target before BLR can replace a resident X30
   * binding. x8 is executor-owned scratch and is never a native branch
   * operand on this path.
   */
  if (target_host != 8U)
    emitter.move_x(8, target_host);
  if (writes_link) {
    const auto link_register =
        write_native_gpr(30, false, false, 9);
    emitter.load_literal_constant(link_register.host,
                                  source_pc + sizeof(std::uint32_t));
    commit_native_gpr(link_register, 30, false);
  }

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  const auto profile_dirty_live_out = gpr_profile_dirty_live_out_mask();
#endif
  flush_exit_gprs();
  /*
   * CpuState is canonical before host-only comparisons take ownership of
   * NZCV. Internal successors reload guest flags on demand.
   */
  assume_flags_in_memory();
  set_exit_gpr_mask(0);
  set_exit_vector_mask(0);

  const auto *site =
      indirect_link_resolver_ == nullptr
          ? nullptr
          : static_cast<const JitIndirectLink *>(
                indirect_link_resolver_(indirect_link_context_, source_pc));
  if (site == nullptr) {
    emitter.store_x(8, 15, offsetof(CpuState, pc));
    emitter.return_constant(Event::continued, count);
    return;
  }

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  JitIndirectLinkProfileCounters profile_counters{};
  if (indirect_link_profile_resolver_ != nullptr)
    profile_counters = indirect_link_profile_resolver_(
        gpr_link_profile_context_, site, source_pc, profile_dirty_live_out);
  const bool profile_link =
      profile_counters.first != nullptr && profile_counters.second != nullptr;
#endif

  const auto matched = emitter.make_label();
  const auto miss = emitter.make_label();
  const auto fallback = emitter.make_label();
  const auto undo_and_return_to_cpp = emitter.make_label();
  emitter.load_stable_pointer(13, reinterpret_cast<std::uintptr_t>(site));
  /*
   * Hide the independent budget update under the dependent PIC descriptor
   * load. Every miss or rejected matched entry rejoins at the one cold undo.
   */
  if (emitter.persistent_linked_block_abi())
    emitter.subtract_immediate(20, 20, static_cast<unsigned>(count));
  const auto load_entry = [&](std::size_t offset) {
    /* ldp x9, x12, [x13, #offset]: {guest target, host entry table}. */
    emitter.load_pair_x(9, 12, 13,
                        static_cast<std::ptrdiff_t>(offset));
  };
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  if (profile_link) {
    const auto first_matched = emitter.make_label();
    load_entry(offsetof(JitIndirectLink, entries));
    emitter.compare_register(8, 9);
    emitter.branch_condition(first_matched, 0); // EQ
    load_entry(offsetof(JitIndirectLink, entries) +
               sizeof(JitIndirectLinkEntry));
    emitter.compare_register(8, 9);
    emitter.branch_condition(miss, 1); // NE
    emitter.load_stable_pointer(
        17, reinterpret_cast<std::uintptr_t>(profile_counters.second));
    emitter.branch(matched);
    emitter.bind(first_matched);
    emitter.load_stable_pointer(
        17, reinterpret_cast<std::uintptr_t>(profile_counters.first));
    emitter.bind(matched);
  } else {
#endif
  load_entry(offsetof(JitIndirectLink, entries));
  emitter.compare_register(8, 9);
  emitter.branch_condition(matched, 0); // EQ
  load_entry(offsetof(JitIndirectLink, entries) +
             sizeof(JitIndirectLinkEntry));
  emitter.compare_register(8, 9);
  emitter.branch_condition(miss, 1); // NE
  emitter.bind(matched);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  }
#endif

  /*
   * x12 is a stable host entry table selected by a guest-target equality
   * test. Only its host-published internal entry can reach BR below.
   */
  if (emitter.persistent_linked_block_abi()) {
    emitter.load_x_indexed_8(13, 12, 21);
  } else {
    emitter.load_x(11, 31, 32);
    emitter.load_b(9, 11, offsetof(JitChainState, link_selector));
    emitter.load_x_indexed_8(13, 12, 9);
  }
  emitter.load_pair_x(
      16, 9, 12,
      -static_cast<std::ptrdiff_t>(offsetof(JitBlockLink, entries)));
  emitter.compare_zero(fallback, 13, true, false);

  if (emitter.persistent_linked_block_abi()) {
    emitter.compare_register(20, 9);
  } else {
    emitter.load_x(10, 11, offsetof(JitChainState, remaining_instructions));
    emitter.subtract_immediate(10, 10, static_cast<unsigned>(count));
    emitter.compare_register(10, 9);
  }
  emitter.branch_condition(fallback, 3); // LO

  if (!emitter.persistent_linked_block_abi())
    emitter.store_x(10, 11, offsetof(JitChainState, remaining_instructions));
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  if (profile_link) {
    emitter.load_x(9, 17, 0);
    emitter.add_immediate(9, 9, 1);
    emitter.store_x(9, 17, 0);
  }
#endif
  emitter.branch_register(13);

  emitter.bind(miss);
  emitter.store_x(8, 15, offsetof(CpuState, pc));
  const unsigned miss_chain_state =
      emitter.persistent_linked_block_abi() ? 19U : 11U;
  if (!emitter.persistent_linked_block_abi())
    emitter.load_x(11, 31, 32);
  emitter.store_x(13, miss_chain_state,
                  offsetof(JitChainState, indirect_miss));
  emitter.load_literal_constant(9, source_pc);
  emitter.store_x(9, miss_chain_state,
                  offsetof(JitChainState, indirect_source_pc));
  emitter.branch(undo_and_return_to_cpp);

  emitter.bind(fallback);
  emitter.store_x(16, 15, offsetof(CpuState, pc));
  emitter.bind(undo_and_return_to_cpp);
  if (emitter.persistent_linked_block_abi())
    emitter.add_immediate(20, 20, static_cast<unsigned>(count));
  emitter.return_constant(Event::continued, count);
}

} // namespace jit

namespace {

class JitCompiler final : private jit::JitCompilerFamilies {
public:
  explicit JitCompiler(bool enable_guarded_control_flow,
                       void *link_context = nullptr,
                       LinkResolver link_resolver = nullptr,
                       bool track_direct_stores = false,
                       IndirectLinkResolver indirect_link_resolver =
                           nullptr,
                       bool persistent_linked_block_abi =
                           persistent_linked_block_abi_default
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
                       ,
                       void *gpr_link_profile_context = nullptr,
                       DirectLinkProfileResolver direct_profile_resolver =
                           nullptr,
                       IndirectLinkProfileResolver indirect_profile_resolver =
                           nullptr
#endif
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
                       ,
                       bool profile_gpr_pressure = true
#endif
                       ) noexcept
      : emitter_{persistent_linked_block_abi}, cold_{this},
        configured_link_context_{link_context},
        configured_link_resolver_{link_resolver},
        configured_indirect_link_resolver_{indirect_link_resolver},
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
        configured_gpr_link_profile_context_{gpr_link_profile_context},
        configured_direct_profile_resolver_{direct_profile_resolver},
        configured_indirect_profile_resolver_{indirect_profile_resolver},
#endif
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
        configured_profile_gpr_pressure_{profile_gpr_pressure},
#endif
        configured_persistent_linked_block_abi_{persistent_linked_block_abi},
        track_direct_stores_{track_direct_stores},
        enable_guarded_control_flow_{enable_guarded_control_flow} {
    attach_family_emitter(&emitter_, &native_fp_, &native_fpmr_,
                          !track_direct_stores_);
    attach_link_resolver(link_context, link_resolver);
    attach_indirect_link_resolver(link_context, indirect_link_resolver);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    attach_gpr_link_profile(gpr_link_profile_context, direct_profile_resolver,
                            indirect_profile_resolver);
#endif
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
    attach_gpr_pressure_profile(profile_gpr_pressure);
#endif
    emitter_.attach_exit_flush(this, [](void *context) {
      static_cast<JitCompiler *>(context)->flush_exit_gprs();
    });
    emitter_.attach_flag_clobber(this, [](void *context) {
      static_cast<JitCompiler *>(context)
          ->spill_flags_for_internal_clobber();
    });
  }

  JitCompiler(bool enable_guarded_control_flow,
              const ArmFeatureSet &translation_features) noexcept
      : JitCompiler{enable_guarded_control_flow} {
    translation_features_ = &translation_features;
  }

#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  void set_gpr_pressure_profile_enabled(bool enabled) noexcept {
    configured_profile_gpr_pressure_ = enabled;
    attach_gpr_pressure_profile(enabled);
  }
#endif
  void set_gpr_residency_planning_enabled(bool enabled) noexcept {
    gpr_residency_planning_enabled_ = enabled;
  }
  void set_table_list_planning_enabled(bool enabled) noexcept {
    table_list_planning_enabled_ = enabled;
  }
  void set_pointer_authentication_enabled(bool enabled) noexcept {
    enable_pointer_authentication_ = enabled;
  }

  struct Result final {
    std::span<const std::uint32_t> words;
    std::size_t guest_instructions{};
    std::size_t hot_words{};
    std::size_t internal_entry_word{};
    std::uint64_t memory_write_mask{};
    std::uint64_t direct_memory_write_mask{};
    bool accesses_fp_state{};
    bool link_target_safe{true};
    bool native_fp{};
    bool native_fpmr{};
    bool guest_illegal{};
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    std::uint32_t profile_read_before_write{};
    std::uint32_t profile_written{};
#endif
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
    jit::JitGprPressureProfile gpr_pressure{};
#endif
    GprResidencyPlan gpr_residency_plan{};
  };

  Result compile(std::uint64_t guest_pc,
                 std::span<const std::uint32_t> instructions) {
    const bool can_plan_gpr_residency =
        gpr_residency_planning_enabled_
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
        && configured_direct_profile_resolver_ == nullptr &&
        configured_indirect_profile_resolver_ == nullptr
#endif
        ;
    auto selected =
        compile_once(guest_pc, instructions, can_plan_gpr_residency);
    bool retained_gpr_plan = false;
    const auto plan = selected.gpr_residency_plan;
    if (can_plan_gpr_residency &&
        plan.count != 0U && !selected.words.empty()) {
      JitCompiler candidate_compiler{
          enable_guarded_control_flow_, configured_link_context_,
          configured_link_resolver_, track_direct_stores_,
          configured_indirect_link_resolver_,
          configured_persistent_linked_block_abi_
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
          ,
          configured_gpr_link_profile_context_,
          configured_direct_profile_resolver_,
          configured_indirect_profile_resolver_
#endif
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
          ,
          configured_profile_gpr_pressure_
#endif
      };
      candidate_compiler.translation_features_ = translation_features_;
      candidate_compiler.table_list_planning_enabled_ =
          table_list_planning_enabled_;
      candidate_compiler.enable_pointer_authentication_ =
          enable_pointer_authentication_;
      candidate_compiler.gpr_residency_planning_enabled_ = false;
      candidate_compiler.apply_gpr_residency_plan(plan);
      auto candidate =
          candidate_compiler.compile_once(guest_pc, instructions, false);
      const bool same_metadata =
          candidate.guest_instructions == selected.guest_instructions &&
          candidate.internal_entry_word == selected.internal_entry_word &&
          candidate.memory_write_mask == selected.memory_write_mask &&
          candidate.direct_memory_write_mask ==
              selected.direct_memory_write_mask &&
          candidate.accesses_fp_state == selected.accesses_fp_state &&
          candidate.link_target_safe == selected.link_target_safe &&
          candidate.native_fp == selected.native_fp &&
          candidate.native_fpmr == selected.native_fpmr &&
          candidate.guest_illegal == selected.guest_illegal
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
          &&
          candidate.profile_read_before_write ==
              selected.profile_read_before_write &&
          candidate.profile_written == selected.profile_written
#endif
          ;
      if (same_metadata && !candidate.words.empty() &&
          candidate.hot_words < selected.hot_words &&
          candidate.words.size() <= selected.words.size()) {
        selected_words_.assign(candidate.words.begin(),
                               candidate.words.end());
        candidate.words = selected_words_;
        candidate.gpr_residency_plan = plan;
        selected = candidate;
        retained_gpr_plan = true;
      }
    }
    if (!retained_gpr_plan)
      selected.gpr_residency_plan = {};

    profile_jit_compilation(
        guest_pc,
        instructions.first(
            std::min(selected.guest_instructions, instructions.size())),
        selected.words, selected.hot_words, track_direct_stores_
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
        ,
        selected.gpr_pressure
#endif
    );
    return selected;
  }

  Result compile_once(std::uint64_t guest_pc,
                      std::span<const std::uint32_t> instructions,
                      bool observe_gpr_residency) {
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    begin_gpr_link_profile_block(guest_pc);
#endif
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
    begin_gpr_pressure_profile_block();
#endif
    begin_gpr_residency_observation(observe_gpr_residency);
    select_constrained_vector_pins(instructions);
    if (table_list_planning_enabled_)
      select_consecutive_vector_lists(instructions);
    emitter_.prologue();
    const auto internal_entry_word = emitter_.size();
    while (count_ < instructions.size()) {
      const auto pc = guest_pc + count_ * sizeof(std::uint32_t);
      if (const auto run = try_emit_simd_unsigned_run(
              instructions.subspan(count_), pc);
          run != 0) {
        accesses_fp_state_ = true;
        count_ += run;
        continue;
      }
      if (const auto run = try_emit_sp_unsigned_run(
              instructions.subspan(count_), pc);
          run != 0) {
        count_ += run;
        continue;
      }
      if (const auto run = try_emit_gpr_unsigned_run(
              instructions.subspan(count_), pc);
          run != 0) {
        count_ += run;
        continue;
      }
      if (try_emit_native_exclusive_run(instructions.subspan(count_), pc))
        continue;
      const auto instruction = instructions[count_];
      if (reservation_sensitive_allocation(instruction)) {
        /*
         * A reservation-establishing helper waits for every other vCPU's
         * published direct-store block.  It therefore cannot execute inside
         * a publication inherited from an earlier instruction or linked
         * predecessor: two such vCPUs could otherwise wait on one another.
         *
         * Split before the allocation and mark its one-instruction block as
         * an ineligible chain target.  Both decisions happen while
         * translating, leaving ordinary generated blocks branch-free.
         */
        if (count_ != 0) {
          store_pc(pc);
          emitter_.return_constant(Event::continued, count_);
          terminated_ = true;
          break;
        }
        link_target_safe_ = false;
      }
      const auto action = emit_instruction(instruction, pc);
      if (action == Action::unsupported) {
        /*
         * Keep a supported native prefix instead of discarding it
         * merely because the following instruction is unsupported.
         * A block beginning with an unsupported or unavailable
         * instruction becomes a precise guest illegal-instruction
         * terminal. The generated block contains no runtime decode or
         * feature check, and the faulting instruction does not retire.
         */
        if (count_ == 0) {
          guest_illegal_ = true;
          store_pc(pc);
          emitter_.return_constant(Event::illegal_instruction, count_ + 1U);
          ++count_;
          terminated_ = true;
        } else {
          store_pc(pc);
          emitter_.return_constant(Event::continued, count_);
          terminated_ = true;
        }
        break;
      }
      accesses_fp_state_ =
          accesses_fp_state_ ||
          arm64::detail::instruction_accesses_fp_state(instruction);
      ++count_;
      if (action == Action::terminate) {
        terminated_ = true;
        break;
      }
    }
    if (!terminated_) {
      store_pc(guest_pc + count_ * sizeof(std::uint32_t));
      emitter_.return_constant(Event::continued, count_);
    }
    const auto hot_words = emitter_.size();
    const auto gpr_residency_plan =
        finish_gpr_residency_observation();
    begin_gpr_residency_observation(false);
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
    const auto gpr_pressure = finish_gpr_pressure_profile_block();
#endif
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    const auto profile_read_before_write =
        gpr_profile_read_before_write_mask();
    const auto profile_written = gpr_profile_written_mask();
#endif
    for (const auto &translation : translation_cold_)
      emit_translation_cold(translation);
    for (const auto &cold : cold_) {
      set_exit_gpr_mask(cold.dirty_gprs);
      set_exit_vector_mask(cold.dirty_vectors);
      emitter_.bind(cold.label);
      /*
       * Every edge leading here is flag-neutral. Publish the translation-time
       * snapshot only on the cold arm, then let the existing terminal helpers
       * consume CpuState as their canonical source.
       */
      publish_cold_flags(cold.flags);
      assume_flags_in_memory();
      if (cold.action == ColdPath::Action::memory_fault) {
        store_pc(cold.pc);
        emitter_.return_constant(Event::memory_fault, cold.count);
      } else if (cold.action == ColdPath::Action::pair_slow) {
        emit_pair_slow(cold.instruction, cold.pc, cold.count);
      } else if (cold.action == ColdPath::Action::simd_structure_slow) {
        emit_simd_structure_slow(cold.instruction, cold.pc, cold.count);
      } else if (cold.action == ColdPath::Action::alignment_fault) {
        emit_alignment_fault(cold.instruction, cold.pc, cold.count);
      } else if (cold.action == ColdPath::Action::ordered_slow) {
        emit_ordered_slow(cold.instruction, cold.pc, cold.count);
      } else if (cold.action == ColdPath::Action::exclusive_slow) {
        emit_exclusive_slow(cold.instruction, cold.pc, cold.count);
      } else if (cold.action ==
                 ColdPath::Action::deferred_exclusive_slow) {
        emit_deferred_exclusive_slow(cold.pc, cold.count);
      } else if (cold.action == ColdPath::Action::memory_slow) {
        emit_memory_slow(cold.instruction, cold.pc, cold.count);
      }
    }
    const auto words = emitter_.finish();
    Result result{
        .words = words,
        .guest_instructions = count_,
        .hot_words = hot_words,
        .internal_entry_word = internal_entry_word,
        .memory_write_mask = memory_write_mask_,
        .direct_memory_write_mask = direct_memory_write_mask_,
        .accesses_fp_state = accesses_fp_state_,
        .link_target_safe = link_target_safe_,
        .native_fp = native_fp_,
        .native_fpmr = native_fpmr_,
        .guest_illegal = guest_illegal_,
        .gpr_residency_plan = gpr_residency_plan,
    };
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    result.profile_read_before_write = profile_read_before_write;
    result.profile_written = profile_written;
#endif
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
    result.gpr_pressure = gpr_pressure;
#endif
    return result;
  }

private:
  enum class Action {
    keep_going,
    terminate,
    unsupported,
  };

  void select_constrained_vector_pins(
      std::span<const std::uint32_t> instructions) noexcept {
    std::array<unsigned, 32> frequency{};
    std::array<std::size_t, 32> first_use{};
    first_use.fill(std::numeric_limits<std::size_t>::max());
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const bool reservation_boundary =
          reservation_sensitive_allocation(instructions[index]);
      if (index != 0U && reservation_boundary)
        break;
      const auto guest = constrained_vector_source(instructions[index]);
      if (guest) {
        ++frequency[*guest];
        first_use[*guest] = std::min(first_use[*guest], index);
      }
      if (reservation_boundary ||
          obvious_translated_block_terminator(instructions[index]))
        break;
    }

    for (std::size_t pin = 0; pin < 2U; ++pin) {
      unsigned selected = 32U;
      for (unsigned guest = 0; guest < 32U; ++guest) {
        if (frequency[guest] == 0U)
          continue;
        if (selected == 32U ||
            frequency[guest] > frequency[selected] ||
            (frequency[guest] == frequency[selected] &&
             first_use[guest] < first_use[selected]))
          selected = guest;
      }
      if (selected == 32U)
        break;
      pin_constrained_vector(selected);
      frequency[selected] = 0U;
    }
  }

  void select_consecutive_vector_lists(
      std::span<const std::uint32_t> instructions) noexcept {
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const bool reservation_boundary =
          reservation_sensitive_allocation(instructions[index]);
      if (index != 0U && reservation_boundary)
        break;
      if (const auto list =
              consecutive_vector_list(instructions[index]))
        static_cast<void>(
            pin_consecutive_vector_list(list->first, list->count));
      if (reservation_boundary ||
          obvious_translated_block_terminator(instructions[index]))
        break;
    }
  }

  [[nodiscard]] bool
  translation_supports(ArmFeature feature) const noexcept {
    if (translation_features_ != nullptr)
      return translation_features_->test(
          static_cast<std::size_t>(feature));
    return host_arm_features().has(feature);
  }

  struct ColdPath final {
    enum class Action {
      memory_fault,
      pair_slow,
      simd_structure_slow,
      alignment_fault,
      ordered_slow,
      exclusive_slow,
      deferred_exclusive_slow,
      memory_slow,
    };

    Arm64Emitter::Label label;
    std::uint32_t instruction;
    std::uint64_t pc;
    std::size_t count;
    Action action;
    std::uint16_t dirty_gprs{};
    std::uint32_t dirty_vectors{};
    GuestFlagsState flags{GuestFlagsState::memory};
  };

  struct TranslationColdPath final {
    Arm64Emitter::Label label;
    Arm64Emitter::Label continuation;
    Arm64Emitter::Label fault;
    unsigned address;
    Access access;
  };

  class ColdPaths final {
  public:
    explicit ColdPaths(JitCompiler *compiler) noexcept
        : compiler_{compiler} {}

    void push_back(ColdPath path) {
      /*
       * Cold labels are emitted after the complete hot block, when later
       * guest instructions may have changed the compiler's flag state. Keep
       * the state at the edge with the label; flag-neutral guards allow the
       * rare arm to publish it without adding work to the hot path.
       */
      path.flags = compiler_->snapshot_flags();
      path.dirty_gprs = compiler_->dirty_gpr_mask();
      path.dirty_vectors = compiler_->dirty_vector_mask();
      paths_.push_back(path);
    }

    [[nodiscard]] std::vector<ColdPath>::const_iterator
    begin() const noexcept {
      return paths_.begin();
    }
    [[nodiscard]] std::vector<ColdPath>::const_iterator
    end() const noexcept {
      return paths_.end();
    }

  private:
    JitCompiler *compiler_;
    std::vector<ColdPath> paths_;
  };

  [[nodiscard]] static bool
  reservation_sensitive_allocation(std::uint32_t instruction) noexcept {
    const bool native_scalar_lse =
        (instruction & 0x0100'0000U) == 0U &&
        host_arm_features().has(ArmFeature::lse) &&
        ((instruction & 0x3f20'0c00U) == 0x3820'0000U ||
         (instruction & 0x3fa0'0000U) == 0x08a0'0000U);
    if (native_scalar_lse)
      return false;
    const bool read_check_write =
        (instruction & 0xbf20'fc00U) == 0x1920'0800U ||
        ((instruction & 0xbf20'8c00U) == 0x3820'8000U &&
         ((instruction >> 12) & 7U) >= 1U &&
         ((instruction >> 12) & 7U) <= 3U) ||
        (instruction & 0xbf20'fc00U) == 0x1920'0c00U ||
        ((instruction & 0xbf20'8c00U) == 0x1920'8000U &&
         (((instruction >> 12) & 7U) != 0U ||
          (instruction & 0x4000'0000U) != 0U));
    const bool pair_atomic =
        (instruction & 0xbf20'0c00U) == 0x1920'0000U;
    const bool scalar_atomic =
        (instruction & 0x3f20'0c00U) == 0x3820'0000U ||
        (instruction & 0x3f20'0c00U) == 0x1920'0400U;
    const bool exclusive_or_ordered =
        (instruction & 0x3f00'0000U) == 0x0800'0000U ||
        (instruction & 0x3f00'0000U) == 0x0900'0000U;
    return read_check_write || pair_atomic || scalar_atomic ||
           exclusive_or_ordered;
  }

  void mark_memory_write() noexcept {
    if (count_ < maximum_block_instructions)
      memory_write_mask_ |= std::uint64_t{1} << count_;
  }

  void mark_direct_memory_write() noexcept {
    mark_memory_write();
    if (count_ < maximum_block_instructions)
      direct_memory_write_mask_ |= std::uint64_t{1} << count_;
  }

  void track_direct_store(unsigned address, std::size_t size) {
    track_direct_store(address, size, count_);
  }

  void track_direct_store(unsigned address, std::size_t size,
                          std::size_t instruction_index) {
    if (!track_direct_stores_ ||
        instruction_index >= maximum_block_instructions)
      return;
    /* Every caller records after emit_translate(), so address is the
     * permission-checked host backing pointer rather than a guest VA.
     */
    const auto offset =
        instruction_index * sizeof(JitStoreRange);
    emitter_.load_x(3, 31, 32);
    emitter_.load_x(3, 3, offsetof(JitChainState, stores));
    emitter_.store_x(address, 3, offset + offsetof(JitStoreRange, address));
    emitter_.load_constant(4, size);
    emitter_.store_x(4, 3, offset + offsetof(JitStoreRange, size));
  }

  [[nodiscard]] bool emit_primary_hint(std::uint32_t instruction) {
    if ((instruction & 0xffff'f01fU) != 0xd503'201fU)
      return false;

    /*
     * Stateful aliases name fixed architectural registers. Rewrite them to
     * ordinary register forms over scratch registers so x15 remains the
     * CpuState base and only the specified guest destination is committed.
     */
    const unsigned immediate = (instruction >> 5) & 0x7fU;
    if (immediate == 40U) {
      if (!host_arm_features().has(ArmFeature::chk))
        return true;
      load_gpr(16, 16, false);
      emitter_.emit(instruction);
      store_gpr(16, 16, false);
      return true;
    }
    /*
     * These hints directly affect the host execution carrying the guest
     * block. Optional HINT-space encodings are architecturally NOP on older
     * hosts, so no runtime feature branch or fallback sequence is needed.
     */
    if (immediate == 1U || immediate == 6U || immediate == 20U ||
        immediate == 22U) {
      emitter_.emit(instruction);
      return true;
    }
    return true;
  }

  void mask_register(unsigned reg, std::uint64_t mask, bool wide) {
    emitter_.load_constant(9, mask);
    emitter_.emit((wide ? 0x8a00'0000U : 0x0a00'0000U) | (9U << 16) |
                  ((reg & 31U) << 5) | (reg & 31U));
  }

  [[nodiscard]] bool emit_system_register(std::uint32_t instruction) {
    constexpr std::uint32_t fpcr_mask = 0x07f7'9f07U;
    constexpr std::uint32_t fpsr_mask = 0xf800'009fU;
    constexpr std::uint64_t fpmr_mask = 0x0000'003f'ff7f'c1ffULL;
    const auto system = instruction & 0xffff'ffe0U;
    const unsigned transfer = instruction & 31U;
    const auto write_destination = [&](auto emit_value) {
      if (transfer == 31U)
        return;
      const auto native_destination =
          write_native_gpr(transfer, false, false, 8);
      emit_value(native_destination.host);
      commit_native_gpr(native_destination, transfer, false);
    };
    const auto copy_source_to_scratch = [&] {
      const auto native_source =
          read_native_gpr(transfer, false, 8);
      if (native_source.host != 8U)
        emitter_.move_x(8, native_source.host);
    };
    switch (system) {
    case 0xd53b'd040U: // TPIDR_EL0
      write_destination([&](unsigned destination) {
        emitter_.load_x(destination, 15, offsetof(CpuState, tls));
      });
      return true;
    case 0xd51b'd040U: {
      const auto native_source =
          read_native_gpr(transfer, false, 8);
      emitter_.store_x(native_source.host, 15, offsetof(CpuState, tls));
      return true;
    }
    case 0xd53b'd060U: // TPIDRRO_EL0
      write_destination(
          [&](unsigned destination) {
            emitter_.load_constant(destination, 0);
          });
      return true;
    case 0xd53b'00e0U: // DCZID_EL0
      write_destination(
          [&](unsigned destination) {
            emitter_.load_constant(destination, 4U);
          });
      return true;
    case 0xd53b'0020U: // CTR_EL0
      write_destination([&](unsigned destination) {
        emitter_.load_constant(destination, 0x8444'8004U);
      });
      return true;
    case 0xd53b'4400U: // FPCR
      write_destination([&](unsigned destination) {
        emitter_.load_w(destination, 15, offsetof(CpuState, fpcr));
        mask_register(destination, fpcr_mask, false);
      });
      return true;
    case 0xd51b'4400U:
      copy_source_to_scratch();
      mask_register(8, fpcr_mask, false);
      emitter_.store_w(8, 15, offsetof(CpuState, fpcr));
      mask_register(
          8, fpcr_mask & ~arm64::detail::fpcr_exception_enable_mask, false);
      emitter_.emit(0xd51b'4408U); // msr fpcr, x8
      native_fp_ = true;
      return true;
    case 0xd53b'4420U:             // FPSR
      write_destination([&](unsigned destination) {
        emitter_.emit(0xd53b'4420U | destination);
        mask_register(destination, native_fpsr_mutable_mask, false);
        emitter_.load_w(10, 15, offsetof(CpuState, fpsr));
        mask_register(10, 0xf000'0000U, false);
        emitter_.emit(0x2a00'0000U | (10U << 16) |
                      (destination << 5) | destination);
      });
      native_fp_ = true;
      return true;
    case 0xd51b'4420U:
      copy_source_to_scratch();
      mask_register(8, fpsr_mask, false);
      emitter_.store_w(8, 15, offsetof(CpuState, fpsr));
      emitter_.emit(0xd51b'4428U); // msr fpsr, x8
      native_fp_ = true;
      return true;
    case 0xd53b'4440U: // FPMR
      write_destination([&](unsigned destination) {
        emitter_.load_x(destination, 15, offsetof(CpuState, fpmr));
      });
      return true;
    case 0xd51b'4440U:
      copy_source_to_scratch();
      mask_register(8, fpmr_mask, true);
      emitter_.store_x(8, 15, offsetof(CpuState, fpmr));
      if (translation_supports(ArmFeature::fp8)) {
        emitter_.emit(0xd51b'4448U); // msr FPMR, x8
        native_fpmr_ = true;
      }
      return true;
    case 0xd53b'4200U: // NZCV
      write_destination([&](unsigned destination) {
        read_flags(destination);
      });
      return true;
    case 0xd51b'4200U:
      copy_source_to_scratch();
      mask_register(8, 0xf000'0000U, false);
      emitter_.store_w(8, 15, offsetof(CpuState, nzcv));
      /* The guest write supersedes any formerly resident host value. */
      assume_flags_in_memory();
      return true;
    case 0xd53b'e000U: // CNTFRQ_EL0
      write_destination([&](unsigned destination) {
        emitter_.emit(0xd53b'e000U | destination);
      });
      return true;
    case 0xd53b'e040U: // CNTVCT_EL0
      write_destination([&](unsigned destination) {
        emitter_.emit(0xd53b'e040U | destination);
      });
      return true;
    default:
      return false;
    }
  }

  void emit_cache_zero(std::uint32_t instruction, std::uint64_t pc) {
    mark_direct_memory_write();
    const unsigned source = instruction & 31U;
    load_gpr(8, source, false);
    mask_register(8, ~std::uint64_t{63}, true);
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    /* The advertised 64-byte block size divides every supported guest
     * page, so an aligned DC ZVA block needs exactly one translation.
     */
    emit_translate(8, Access::write, fault, fault, 1);
    track_direct_store(8, 64);
    for (unsigned offset = 0; offset < 64; offset += 16)
      emitter_.emit(0xa900'0000U | ((offset / 8U) << 15) | (31U << 10) |
                    (8U << 5) | 31U);
    clear_local_exclusive_monitor();
  }

  void emit_code_synchronize(std::uint32_t instruction, std::uint64_t pc,
                             std::size_t count) {
    /*
     * This instruction is an architectural translated-code boundary. Publish
     * the complete native register cache before the cold helper, then return
     * to C++ at the known fallthrough so the changed code sequence is
     * observed before another translated instruction executes.
     */
    store_pc(pc + sizeof(std::uint32_t));
    spill_gprs_for_terminal_call();
    const unsigned source = instruction & 31U;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    profile_gpr_read(source, false);
#endif
    if (source == 31U)
      emitter_.move_x(0, 31);
    else
      emitter_.load_x(0, 15, register_offset(source, false));
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_guest_code_synchronize));
    emitter_.return_constant(Event::continued, count);
  }

  void emit_pair_slow(std::uint32_t instruction, std::uint64_t pc,
                      std::size_t count) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.call(reinterpret_cast<const void *>(&uml_arm64_jit_execute_pair));
    emitter_.return_dynamic(count);
  }

  void emit_simd_structure_slow(std::uint32_t instruction, std::uint64_t pc,
                                std::size_t count) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_jit_execute_simd_structure));
    emitter_.return_dynamic(count);
  }

  void emit_alignment_fault(std::uint32_t instruction, std::uint64_t pc,
                            std::size_t count) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_jit_alignment_fault));
    emitter_.return_dynamic(count);
  }

  void emit_ordered_slow(std::uint32_t instruction, std::uint64_t pc,
                         std::size_t count) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_jit_execute_ordered));
    emitter_.return_dynamic(count);
  }

  void emit_exclusive_slow(std::uint32_t instruction, std::uint64_t pc,
                           std::size_t count) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_jit_execute_exclusive));
    emitter_.return_dynamic(count);
  }

  void emit_deferred_exclusive_slow(std::uint64_t pc,
                                    std::size_t count) {
    if (count == 0U)
      std::abort();
    store_pc(pc);
    emitter_.load_constant(0, 1U);
    emitter_.store_b(
        0, 19, offsetof(JitChainState, deferred_semantic_instruction));
    /*
     * The instruction at pc has not executed. return_constant() flushes the
     * live guest prefix before unwinding the one outer generated frame.
     */
    emitter_.return_constant(Event::continued, count - 1U);
  }

  void emit_memory_slow(std::uint32_t instruction, std::uint64_t pc,
                        std::size_t count) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_jit_execute_memory));
    emitter_.return_dynamic(count);
  }

  void emit_gcs_slow(std::uint32_t instruction, std::uint64_t pc,
                     std::size_t count) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.call(reinterpret_cast<const void *>(&uml_arm64_jit_execute_gcs));
    emitter_.return_dynamic(count);
  }

  void emit_simd_modified_immediate(std::uint32_t instruction,
                                    bool reads_destination) {
    const unsigned destination = instruction & 31U;
    const auto native_destination =
        write_native_vector(destination, reads_destination, 0);
    emitter_.emit((instruction & ~31U) | native_destination.host);
    commit_native_vector(native_destination, destination);
  }

  void emit_simd_duplicate_general(std::uint32_t instruction) {
    const unsigned source = (instruction >> 5) & 31U;
    const unsigned destination = instruction & 31U;
    const auto native_source = read_native_gpr(source, false, 8);
    const auto native_destination =
        write_native_vector(destination, false, 0);
    emitter_.emit((instruction & ~((31U << 5) | 31U)) |
                  (native_source.host << 5) | native_destination.host);
    commit_native_vector(native_destination, destination);
  }

  [[nodiscard]] bool emit_simd_insert(std::uint32_t instruction) {
    if (!host_arm_features().advanced_simd)
      return false;
    const bool from_general = (instruction & 0xffe0'fc00U) == 0x4e00'1c00U;
    const bool from_element = (instruction & 0xffe0'8400U) == 0x6e00'0400U;
    if (!from_general && !from_element)
      return false;
    if (!arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U))
      return false;
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    unsigned native_source{};
    if (from_general)
      native_source = read_native_gpr(source, false, 8).host;
    else
      native_source = read_native_vector(source, 1).host;
    const auto native_destination =
        write_native_vector(destination, true, 0);
    emitter_.emit((instruction & ~0x3ffU) | (native_source << 5) |
                  native_destination.host);
    commit_native_vector(native_destination, destination);
    return true;
  }

  [[nodiscard]] bool emit_simd_duplicate_element(std::uint32_t instruction) {
    if (!host_arm_features().advanced_simd)
      return false;
    const bool vector = (instruction & 0xbf20'fc00U) == 0x0e00'0400U;
    const bool scalar = (instruction & 0xff20'fc00U) == 0x5e00'0400U;
    if (!vector && !scalar)
      return false;
    const auto lane =
        arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U);
    const bool q = (instruction & 0x4000'0000U) != 0;
    if (!lane || (vector && !q && lane->element_shift == 3U))
      return false;
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    const auto native_source = read_native_vector(source, 1);
    const auto native_destination =
        write_native_vector(destination, false, 0);
    emitter_.emit((instruction & ~0x3ffU) | (native_source.host << 5) |
                  native_destination.host);
    commit_native_vector(native_destination, destination);
    return true;
  }

  void emit_simd_unary_native(std::uint32_t instruction) {
    const unsigned source = (instruction >> 5) & 31U;
    const unsigned destination = instruction & 31U;
    const auto native_source = read_native_vector(source, 1);
    const auto native_destination =
        write_native_vector(destination, false, 0);
    emitter_.emit((instruction & ~((31U << 5) | 31U)) |
                  (native_source.host << 5) | native_destination.host);
    commit_native_vector(native_destination, destination);
  }

  [[nodiscard]] bool emit_simd_bit_unary(std::uint32_t instruction) {
    if (!host_arm_features().advanced_simd)
      return false;
    const auto operation = instruction & 0x9f20'fc00U;
    if (operation != 0x0e20'5800U && operation != 0x0e20'0800U &&
        operation != 0x0e20'1800U)
      return false;
    const unsigned opcode = (instruction >> 10) & 63U;
    const bool alternate = (instruction & 0x2000'0000U) != 0;
    const unsigned size = (instruction >> 22) & 3U;
    const bool valid =
        opcode == 22U
            ? ((!alternate && size == 0U) || (alternate && size <= 1U))
        : opcode == 2U ? ((!alternate && size < 3U) || (alternate && size < 2U))
                       : opcode == 6U && !alternate && size == 0U;
    if (!valid)
      return false;
    emit_simd_unary_native(instruction);
    return true;
  }

  [[nodiscard]] bool emit_simd_integer_reduce(std::uint32_t instruction) {
    if (!host_arm_features().advanced_simd ||
        (instruction & 0x9f30'0c00U) != 0x0e30'0800U)
      return false;
    const unsigned operation = (instruction >> 12) & 31U;
    const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
    if (operation != 3U && operation != 10U && operation != 26U &&
        (operation != 27U || unsigned_group))
      return false;
    const bool q = (instruction & 0x4000'0000U) != 0;
    const unsigned size = (instruction >> 22) & 3U;
    if (size > 2U || (!q && size == 2U))
      return false;
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    const auto native_source = read_native_vector(source, 1);
    const auto native_destination =
        write_native_vector(destination, false, 0);
    emitter_.emit((instruction & ~0x3ffU) | (native_source.host << 5) |
                  native_destination.host);
    commit_native_vector(native_destination, destination);
    return true;
  }

  [[nodiscard]] bool emit_simd_scalar_pairwise_add(std::uint32_t instruction) {
    if (!host_arm_features().advanced_simd ||
        (instruction & 0xffff'fc00U) != 0x5ef1'b800U)
      return false;
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    const auto native_source = read_native_vector(source, 1);
    const auto native_destination =
        write_native_vector(destination, false, 0);
    emitter_.emit((instruction & ~0x3ffU) | (native_source.host << 5) |
                  native_destination.host);
    commit_native_vector(native_destination, destination);
    return true;
  }

  [[nodiscard]] bool emit_simd_shift_immediate(std::uint32_t instruction) {
    if (!host_arm_features().advanced_simd)
      return false;
    const bool vector = (instruction & 0x9f00'0400U) == 0x0f00'0400U;
    const bool scalar = (instruction & 0xdf00'0400U) == 0x5f00'0400U;
    if (!vector && !scalar)
      return false;
    const unsigned opcode = (instruction >> 11) & 31U;
    const bool alternate = (instruction & 0x2000'0000U) != 0;
    if (opcode != 0U && opcode != 2U && opcode != 4U && opcode != 6U &&
        opcode != 8U && opcode != 10U && opcode != 12U && opcode != 14U &&
        opcode != 16U && opcode != 17U && opcode != 18U && opcode != 19U)
      return false;
    if ((opcode == 8U || opcode == 12U) && !alternate)
      return false;
    const unsigned encoded = (instruction >> 16) & 0x7fU;
    if (encoded < 8U)
      return false;
    const unsigned lane_bits = std::bit_floor(encoded);
    if (lane_bits > 64U)
      return false;
    const bool q = (instruction & 0x4000'0000U) != 0;
    const bool narrow = opcode >= 16U;
    const bool saturating_left =
        opcode == 12U || opcode == 14U;
    if (narrow) {
      if (encoded >= 64U || (scalar && opcode <= 17U && !alternate))
        return false;
    } else {
      if (scalar && !saturating_left && lane_bits != 64U)
        return false;
      const unsigned vector_bits = scalar ? lane_bits : q ? 128U : 64U;
      if (lane_bits > vector_bits || (!scalar && lane_bits == 64U && !q))
        return false;
      const bool shift_left =
          opcode == 10U || opcode == 12U || opcode == 14U;
      const unsigned shift =
          shift_left ? encoded - lane_bits : 2U * lane_bits - encoded;
      if ((shift_left && shift >= lane_bits) ||
          (!shift_left && (shift == 0U || shift > lane_bits)))
        return false;
    }
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    const bool reads_destination =
        opcode == 2U || opcode == 6U || opcode == 8U ||
        (opcode == 10U && alternate) || (narrow && !scalar && q);
    const auto native_source = read_native_vector(source, 1);
    const auto native_destination =
        write_native_vector(destination, reads_destination, 0);
    emitter_.emit((instruction & ~0x3ffU) | (native_source.host << 5) |
                  native_destination.host);
    commit_native_vector(native_destination, destination);
    const bool saturating =
        saturating_left || opcode >= 18U ||
        ((opcode == 16U || opcode == 17U) && alternate);
    if (saturating)
      native_fp_ = true;
    return true;
  }

  [[nodiscard]] bool emit_simd_shift_left_long(std::uint32_t instruction) {
    if (!host_arm_features().advanced_simd ||
        (instruction & 0x9f80'fc00U) != 0x0f00'a400U)
      return false;
    const unsigned encoded = (instruction >> 16) & 0x7fU;
    const unsigned source_bits = std::bit_floor(encoded);
    if (source_bits < 8U || source_bits > 32U ||
        encoded - source_bits >= source_bits)
      return false;
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    const auto native_source = read_native_vector(source, 1);
    const auto native_destination =
        write_native_vector(destination, false, 0);
    emitter_.emit((instruction & ~0x3ffU) | (native_source.host << 5) |
                  native_destination.host);
    commit_native_vector(native_destination, destination);
    return true;
  }

  [[nodiscard]] bool emit_simd_general_move(std::uint32_t instruction) {
    const auto opcode = instruction & 0xffff'fc00U;
    bool vector_to_gpr = (instruction & 0xbfe0'fc00U) == 0x0e00'3c00U ||
                         (instruction & 0xbfe0'fc00U) == 0x0e00'2c00U;
    if (vector_to_gpr) {
      const bool signed_move = (instruction & 0x1000U) == 0;
      const auto lane =
          arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U);
      const bool wide = (instruction & 0x4000'0000U) != 0;
      if (!lane || (signed_move ? lane->element_shift >= (wide ? 3U : 2U)
                                : (wide ? lane->element_shift != 3U
                                        : lane->element_shift >= 3U)))
        return false;
    } else {
      switch (opcode) {
      case 0x1ee6'0000U:
      case 0x9ee6'0000U:
      case 0x9e66'0000U:
      case 0x1e26'0000U:
      case 0x9eae'0000U:
        vector_to_gpr = true;
        break;
      case 0x9e67'0000U:
      case 0x1ee7'0000U:
      case 0x9ee7'0000U:
      case 0x1e27'0000U:
      case 0x9eaf'0000U:
        break;
      default:
        return false;
      }
      if ((opcode == 0x1ee6'0000U || opcode == 0x9ee6'0000U ||
           opcode == 0x1ee7'0000U || opcode == 0x9ee7'0000U) &&
          !host_arm_features().fp16)
        return false;
    }
    const unsigned source = (instruction >> 5) & 31U;
    const unsigned destination = instruction & 31U;
    auto native = instruction & ~((31U << 5) | 31U);
    if (vector_to_gpr) {
      const auto native_source = read_native_vector(source, 0);
      const auto native_destination =
          write_native_gpr(destination, false, false, 8);
      native |= (native_source.host << 5) | native_destination.host;
      emitter_.emit(native);
      commit_native_gpr(native_destination, destination, false);
    } else {
      const auto native_source = read_native_gpr(source, false, 8);
      const bool reads_destination = opcode == 0x9eaf'0000U;
      const auto native_destination =
          write_native_vector(destination, reads_destination, 0);
      native |= (native_source.host << 5) | native_destination.host;
      emitter_.emit(native);
      commit_native_vector(native_destination, destination);
    }
    return true;
  }

  void emit_translate(unsigned address, Access access,
                      Arm64Emitter::Label fallback, Arm64Emitter::Label fault,
                      std::size_t bytes) {
    if (bytes > 1) {
      emitter_.ubfm(0, address, 0, page_shift - 1);
      emitter_.add_immediate(0, 0, static_cast<unsigned>(bytes - 1));
      /*
       * bytes is translation-time bounded below one page, so the sum is less
       * than two pages. Its page bit is therefore the complete crossing test.
       */
      emitter_.test_zero(fallback, 0, page_shift, true);
    }

    /*
     * The TLB tag is the page-aligned guest address. A single encodable
     * logical immediate is exactly equivalent to the former LSR+LSL pair and
     * saves one instruction on every translated memory access.
     */
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
    emitter_.emit(0x9272'c401U | ((address & 31U) << 5)); // and x1, Xn, #-16K
#else
    emitter_.emit(0x9274'cc01U | ((address & 31U) << 5)); // and x1, Xn, #-4K
#endif
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
    emitter_.ubfm(0, address, page_shift,
                  page_shift + arm64::gadgets::tlb_index_bits - 1U);
    emitter_.add_shifted(0, 14, 0, 6);
#else
    emitter_.ubfm(0, address, page_shift,
                  page_shift + arm64::gadgets::tlb_index_bits - 1U);
    emitter_.emit(0xca40'0000U | ((page_shift + 13U) << 10) |
                  ((address & 31U) << 16)); // eor x0, x0, address, lsr #25
    emitter_.ubfm(0, 0, 0, arm64::gadgets::tlb_index_bits - 1U);
    emitter_.add_shifted(0, 14, 0, 5);
#endif
    const unsigned page_offset = access == Access::write ? 16U : 0U;
    const auto slow = emitter_.make_label();
    const auto complete = emitter_.make_label();
    /*
     * The access-specific tag and backing delta are adjacent immutable fields.
     * Load both before matching the tag. The overwhelmingly common first-way
     * hit falls through one normally-not-taken CBNZ and directly applies the
     * host-owned delta. The second way and preserving miss bridge are emitted
     * after the complete hot block and branch back to the same continuation.
     */
    emitter_.load_pair_x(2, 9, 0, page_offset);
    emitter_.emit(0xca01'0042U); // eor x2, x2, x1
    emitter_.compare_zero(slow, 2, true, true);
    emitter_.add_shifted(address, address, 9, 0);
    emitter_.bind(complete);
    translation_cold_.push_back({
        .label = slow,
        .continuation = complete,
        .fault = fault,
        .address = address,
        .access = access,
    });
  }

  void emit_translation_cold(const TranslationColdPath &path) {
    emitter_.bind(path.label);
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
    const unsigned page_offset =
        path.access == Access::write ? 16U : 0U;
    const auto miss = emitter_.make_label();
    emitter_.load_pair_x(2, 9, 0, page_offset + 32U);
    emitter_.emit(0xca01'0042U); // eor x2, x2, x1
    emitter_.compare_zero(miss, 2, true, true);
    emitter_.add_shifted(path.address, path.address, 9, 0);
    emitter_.branch(path.continuation);
    emitter_.bind(miss);
#endif
    emitter_.move_x(0, 14);
    emitter_.move_x(1, path.address);
    emitter_.load_constant(
        2, path.access == Access::write ? 1U : 0U);
    /*
     * Select the complete preservation contract while translating. Hosts
     * without FPMR never execute its system-register encodings, and generated
     * code pays no runtime feature branch.
     */
    const auto *miss_bridge =
        host_arm_features().has(ArmFeature::fp8)
            ? reinterpret_cast<const void *>(
                  &uml_arm64_jit_tlb_translate_preserving_fpmr)
            : reinterpret_cast<const void *>(
                  &uml_arm64_jit_tlb_translate_preserving);
    emitter_.call_preserving_flags(miss_bridge);
    emitter_.move_x(path.address, 0);
    emitter_.compare_zero(path.fault, path.address, true, false);
    emitter_.branch(path.continuation);
  }

  void emit_misalignment_check(unsigned address, std::size_t alignment,
                               Arm64Emitter::Label misaligned) {
    if (alignment <= 1U)
      return;
    if (!std::has_single_bit(alignment) || alignment > 64U)
      std::abort();
    emitter_.ubfm(
        0, address, 0,
        static_cast<unsigned>(std::countr_zero(alignment) - 1U));
    emitter_.compare_zero(misaligned, 0, true, true);
  }

  void clear_local_exclusive_monitor() {
    /*
     * An ordinary store is permitted to clear the local monitor
     * unconditionally.  Keep this as one straight-line instruction after the
     * store: no opcode or monitor-state checks belong in the JIT hot path.
     */
    emitter_.store_b(31, 15, offsetof(CpuState, exclusive_size));
  }

  [[nodiscard]] std::size_t
  try_emit_native_exclusive_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    if (!emitter_.persistent_linked_block_abi() ||
        count_ != 0U || !gpr_slots_unbound() || instructions.size() < 2U)
      return 0;

    const auto load_instruction = instructions[0];
    const auto scalar_exclusive = [](std::uint32_t instruction) {
      return (instruction & 0x3fa0'0000U) == 0x0800'0000U;
    };
    if (!scalar_exclusive(load_instruction) ||
        (load_instruction & 0x0040'0000U) == 0U)
      return 0;

    std::size_t store_index = 1U;
    if (!scalar_exclusive(instructions[store_index]) ||
        (instructions[store_index] & 0x0040'0000U) != 0U) {
      if (instructions.size() < 3U)
        return 0;
      const auto middle = instructions[1];
      const bool add_sub_immediate =
          (middle & 0x1f80'0000U) == 0x1100'0000U;
      const bool add_sub_shifted =
          (middle & 0x1f20'0000U) == 0x0b00'0000U &&
          valid_shifted_register(middle, true);
      const bool logical_shifted =
          (middle & 0x1f00'0000U) == 0x0a00'0000U &&
          valid_shifted_register(middle, false);
      if (!add_sub_immediate && !add_sub_shifted && !logical_shifted)
        return 0;
      store_index = 2U;
    }

    const auto store_instruction = instructions[store_index];
    if (!scalar_exclusive(store_instruction) ||
        (store_instruction & 0x0040'0000U) != 0U)
      return 0;

    const unsigned size = load_instruction >> 30;
    const unsigned base = (load_instruction >> 5) & 31U;
    const unsigned load_destination = load_instruction & 31U;
    const unsigned store_source = store_instruction & 31U;
    const unsigned status = (store_instruction >> 16) & 31U;
    if ((store_instruction >> 30) != size ||
        ((store_instruction >> 5) & 31U) != base ||
        ((load_instruction >> 10) & 31U) != 31U ||
        ((load_instruction >> 16) & 31U) != 31U ||
        ((store_instruction >> 10) & 31U) != 31U ||
        load_destination == base)
      return 0;

    const bool has_middle = store_index == 2U;
    const auto middle_instruction =
        has_middle ? instructions[1] : std::uint32_t{};
    if (has_middle && (middle_instruction & 31U) == base)
      return 0;

    const std::size_t bytes = std::size_t{1} << size;
    const auto slow = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {slow, load_instruction, pc, count_ + 1,
         ColdPath::Action::deferred_exclusive_slow});
    cold_.push_back(
        {fault, load_instruction, pc, count_ + 1,
         ColdPath::Action::memory_fault});

    load_gpr(8, base, true);
    if (base == 31U) {
      emit_misalignment_check(8, 16, slow);
    } else {
      emitter_.load_b(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
    }
    emit_misalignment_check(8, bytes, slow);

    /*
     * Resolve both permissions before establishing the host monitor. x21 is
     * host-owned chain state, so it can retain the translated write address
     * across the one register-only middle instruction. The selector is
     * restored after the store, before the terminating branch can use it.
     */
    emit_translate(8, Access::write, slow, slow, bytes);
    emitter_.move_x(21, 8);
    /*
     * A native conditional store still participates in the publication
     * protocol. Record its possible range before establishing the host
     * monitor; invalidation on a failed attempt is conservative and avoids
     * putting helper work or a status branch inside the native interval.
     */
    track_direct_store(21, bytes, count_ + store_index);
    load_gpr(8, base, true);
    emit_translate(8, Access::read, slow, fault, bytes);
    emitter_.compare_register(8, 21);
    emitter_.branch_condition(slow, 1); // NE

    /*
     * Bind every register needed between the host exclusive load/store before
     * establishing the monitor. The emitted interval then contains only the
     * native load, an optional translation-selected integer operation, and
     * the native store; no CpuState access or helper call can intervene.
     */
    const auto preload = [&](unsigned guest, bool stack_pointer) {
      const auto value = read_native_gpr(guest, stack_pointer, 10);
      if (!value.resident && guest != 31U)
        std::abort();
    };
    if (has_middle) {
      const auto left = (middle_instruction >> 5) & 31U;
      const auto right = (middle_instruction >> 16) & 31U;
      const bool immediate =
          (middle_instruction & 0x1f80'0000U) == 0x1100'0000U;
      if (left != load_destination)
        preload(left, immediate);
      if (!immediate && right != load_destination)
        preload(right, false);
      const bool writes_flags =
          (middle_instruction & 0x2000'0000U) != 0U;
      const auto destination =
          write_native_gpr(middle_instruction & 31U,
                           immediate && !writes_flags, false, 10);
      if (!destination.resident &&
          (middle_instruction & 31U) != 31U)
        std::abort();
    }
    if (store_source != load_destination &&
        (!has_middle || store_source != (middle_instruction & 31U)))
      preload(store_source, false);
    const auto preallocated_status =
        write_native_gpr(status, false, false, 9);
    if (!preallocated_status.resident && status != 31U)
      std::abort();

    const auto native_destination =
        write_native_gpr(load_destination, false, false, 9);
    if (!native_destination.resident && load_destination != 31U)
      std::abort();
    auto native_load =
        load_instruction &
        ~((31U << 16) | (31U << 10) | (31U << 5) | 31U);
    native_load |= (31U << 16) | (31U << 10) | (21U << 5) |
                   native_destination.host;
    emitter_.emit(native_load);
    commit_native_gpr(native_destination, load_destination, false);

    ++count_;
    if (has_middle) {
      const auto middle_action =
          emit_instruction(middle_instruction, pc + sizeof(std::uint32_t));
      if (middle_action != Action::keep_going)
        std::abort();
      ++count_;
    }
    mark_direct_memory_write();
    auto native_source = read_native_gpr(store_source, false, 10);
    const auto native_status = preallocated_status;
    if (status != 31U && status == store_source) {
      emitter_.move_x(10, native_source.host);
      native_source = {10U, false};
    }
    auto native_store =
        store_instruction &
        ~((31U << 16) | (31U << 10) | (31U << 5) | 31U);
    native_store |= (native_status.host << 16) | (31U << 10) |
                    (21U << 5) | native_source.host;
    emitter_.emit(native_store);
    commit_native_gpr(native_status, status, false);
    clear_local_exclusive_monitor();
    emitter_.load_b(21, 19, offsetof(JitChainState, link_selector));

    ++count_;
    return store_index + 1U;
  }

  void emit_ls64(std::uint32_t instruction, std::uint64_t pc, bool load,
                 bool status, bool accdata) {
    const unsigned first = instruction & 31U;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned status_register = (instruction >> 16) & 31U;
    if (!load)
      mark_direct_memory_write();

    load_gpr(8, base, true);
    if (base != 31U) {
      const auto tag_slow = emitter_.make_label();
      cold_.push_back(
          {tag_slow, instruction, pc, count_ + 1,
           ColdPath::Action::memory_slow});
      emitter_.load_b(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(tag_slow, 0, false, true);
    }
    const auto alignment = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {alignment, instruction, pc, count_ + 1, ColdPath::Action::memory_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    /* A 64-byte-aligned transfer cannot cross any supported guest page.
     * Resolve alignment once, then use the single-page TLB fast path.
     */
    emitter_.ubfm(0, 8, 0, 5);
    emitter_.compare_zero(alignment, 0, true, true);
    emit_translate(8, load ? Access::read : Access::write, fault, fault, 1);
    if (!load)
      track_direct_store(8, 64);

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    for (unsigned index = 0; index < 8; ++index) {
      if (load)
        profile_gpr_write(first + index, false);
      else
        profile_gpr_read(first + index, false);
    }
#endif

    /*
     * The native 64-byte forms name x0-x7 implicitly. Some of those registers
     * are ordinary resident-cache hosts, so publish and invalidate their
     * values before entering this constrained-register sequence.
     */
    flush_and_invalidate_gprs();
    if (!load)
      for (unsigned index = 0; index < 8; ++index)
        emitter_.load_x(index, 15,
                        register_offset(first + index, false));
    if (accdata) {
      emitter_.load_w(9, 15, offsetof(CpuState, accdata));
      /* bfxil x0, x9, #0, #32: substitute ACCDATA_EL1[31:0] while
       * preserving the upper half of the first guest register.
       */
      emitter_.emit(0xb340'7d20U);
    }

    if (load) {
      emitter_.emit(0xf83f'd100U); // ld64b x0, [x8]
      for (unsigned index = 0; index < 8; ++index)
        emitter_.store_x(index, 15,
                         register_offset(first + index, false));
    } else if (status || accdata) {
      /* The accumulator form has already materialized its guest-specific
       * first word, so the native status-returning store is equivalent while
       * avoiding dependence on host privileged accumulator state.
      */
      emitter_.emit(0xf829'b100U); // st64bv x9, x0, [x8]
      /* Normal mapped emulator memory defines its successful status as zero,
       * independent of the host memory system's implementation-defined value.
       */
      if (status_register != 31U)
        store_gpr(31, status_register, false);
      clear_local_exclusive_monitor();
    } else {
      emitter_.emit(0xf83f'9100U); // st64b x0, [x8]
      clear_local_exclusive_monitor();
    }
  }

  [[nodiscard]] Action emit_exclusive_memory(std::uint32_t instruction,
                                             std::uint64_t pc) {
    const bool load = (instruction & 0x0040'0000U) != 0;
    const bool pair = (instruction & 0x0020'0000U) != 0;
    const bool exclusive = (instruction & 0x0080'0000U) == 0;
    const unsigned status = (instruction >> 16) & 31U;
    const unsigned second = (instruction >> 10) & 31U;
    const unsigned size = instruction >> 30;
    const bool unprivileged = (instruction & 0x0100'0000U) != 0;
    const bool pair_compare_swap =
        (instruction & 0xbfa0'0000U) == 0x0820'0000U ||
        (instruction & 0xbfa0'0000U) == 0x0980'0000U;
    const bool scalar_compare_swap =
        (instruction & 0x3fa0'0000U) == 0x08a0'0000U ||
        (instruction & 0xbfa0'0000U) == 0x8980'0000U;
    const bool pair_exclusive =
        (instruction & 0xbfa0'0000U) == 0x8820'0000U;
    const bool scalar_exclusive =
        (instruction & 0x3fa0'0000U) == 0x0800'0000U ||
        (instruction & 0xbfa0'0000U) == 0x8900'0000U;

    bool valid = false;
    if (pair_compare_swap) {
      const unsigned replacement = instruction & 31U;
      valid = second == 31U && (status & 1U) == 0U &&
              (replacement & 1U) == 0U && size <= 1U &&
              (!unprivileged || size == 1U);
    } else if (scalar_compare_swap) {
      valid = second == 31U && (!unprivileged || size == 3U);
    } else if (pair_exclusive) {
      const unsigned first = instruction & 31U;
      valid = size >= 2U &&
              (!load || (status == 31U && first != second));
    } else if (scalar_exclusive) {
      valid = second == 31U && (!load || status == 31U);
    } else {
      /* The remaining allocation is the non-exclusive ordered scalar
       * load/store family. Preserve its dedicated helper. */
      valid = !pair && second == 31U && status == 31U && !exclusive;
      if (!valid)
        return Action::unsupported;
      const auto slow = emitter_.make_label();
      cold_.push_back(
          {slow, instruction, pc, count_ + 1, ColdPath::Action::ordered_slow});
      emitter_.branch(slow);
      return Action::terminate;
    }
    if (!valid)
      return Action::unsupported;
    if (scalar_compare_swap && !unprivileged &&
        host_arm_features().has(ArmFeature::lse)) {
      emit_native_scalar_compare_swap(instruction, pc);
      return Action::keep_going;
    }
    if (pair_compare_swap || scalar_compare_swap || !load)
      mark_memory_write();
    const auto slow = emitter_.make_label();
    cold_.push_back(
        {slow, instruction, pc, count_ + 1,
         ColdPath::Action::exclusive_slow});
    emitter_.branch(slow);
    return Action::terminate;
  }

  [[nodiscard]] Action emit_atomic_memory(std::uint32_t instruction,
                                          std::uint64_t pc) {
    const bool unprivileged = (instruction & 0x0100'0000U) != 0;
    const unsigned size = instruction >> 30;
    const unsigned o3 = (instruction >> 15) & 1U;
    const unsigned opcode = (instruction >> 12) & 7U;
    const bool valid =
        unprivileged
            ? size <= 1U &&
                  ((!o3 && (opcode == 0U || opcode == 1U || opcode == 3U)) ||
                   (o3 && opcode == 0U))
            : !o3 || opcode == 0U;
    if (!valid)
      return Action::unsupported;
    if (!unprivileged &&
        host_arm_features().has(ArmFeature::lse)) {
      emit_native_scalar_atomic(instruction, pc);
      return Action::keep_going;
    }
    mark_memory_write();
    const auto slow = emitter_.make_label();
    cold_.push_back(
        {slow, instruction, pc, count_ + 1, ColdPath::Action::exclusive_slow});
    emitter_.branch(slow);
    return Action::terminate;
  }

  void emit_native_scalar_atomic(std::uint32_t instruction,
                                 std::uint64_t pc) {
    mark_direct_memory_write();
    const unsigned size = instruction >> 30;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned source = (instruction >> 16) & 31U;
    const unsigned destination = instruction & 31U;
    const std::size_t bytes = std::size_t{1} << size;
    load_gpr(8, base, true);

    const auto slow = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {slow, instruction, pc, count_ + 1,
         ColdPath::Action::exclusive_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1,
         ColdPath::Action::memory_fault});
    if (base == 31U) {
      emit_misalignment_check(8, 16, slow);
    } else {
      emitter_.load_b(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
      emit_misalignment_check(8, bytes, slow);
    }
    emit_translate(8, Access::write, slow, fault, 1);
    track_direct_store(8, bytes);

    /*
     * Rs is consumed before Rt is defined, so a guest register may legally
     * name both operands. Translation also keeps the guest base value away
     * from the host pointer in x8.
     */
    const auto native_source = read_native_gpr(source, false, 9);
    const auto native_destination =
        write_native_gpr(destination, false, false, 10);
    auto native =
        instruction & ~((31U << 16) | (31U << 5) | 31U);
    native |= (native_source.host << 16) | (8U << 5) |
              native_destination.host;
    emitter_.emit(native);
    commit_native_gpr(native_destination, destination, false);
    clear_local_exclusive_monitor();
  }

  void emit_native_scalar_compare_swap(std::uint32_t instruction,
                                       std::uint64_t pc) {
    mark_direct_memory_write();
    const unsigned size = instruction >> 30;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned compare = (instruction >> 16) & 31U;
    const unsigned replacement = instruction & 31U;
    const std::size_t bytes = std::size_t{1} << size;
    load_gpr(8, base, true);

    const auto slow = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {slow, instruction, pc, count_ + 1,
         ColdPath::Action::exclusive_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1,
         ColdPath::Action::memory_fault});
    if (base == 31U) {
      emit_misalignment_check(8, 16, slow);
    } else {
      emitter_.load_b(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
      emit_misalignment_check(8, bytes, slow);
    }
    emit_translate(8, Access::write, slow, fault, 1);
    track_direct_store(8, bytes);

    /*
     * CAS reads both guest operands before publishing the old memory value
     * through the compare register. Keep distinct nonresident scratch
     * registers, while preserving the valid compare==replacement case.
     */
    const auto native_compare =
        read_native_gpr(compare, false, 9);
    const auto native_replacement =
        replacement == compare
            ? native_compare
            : read_native_gpr(replacement, false, 10);
    const auto native_result =
        write_native_gpr(compare, false, false,
                         native_compare.host);
    auto native =
        instruction & ~((31U << 16) | (31U << 5) | 31U);
    native |= (native_result.host << 16) | (8U << 5) |
              native_replacement.host;
    emitter_.emit(native);
    commit_native_gpr(native_result, compare, false);
    clear_local_exclusive_monitor();
  }

  void emit_atomic_floating_memory(
      std::uint32_t instruction, std::uint64_t pc,
      const arm64::detail::AtomicFloating &decoded) {
    mark_direct_memory_write();
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned source = (instruction >> 16) & 31U;
    const unsigned destination = instruction & 31U;
    const std::size_t bytes = decoded.width / 8U;
    load_gpr(8, base, true);

    /*
     * Every dynamic exceptional condition terminates through the exact
     * one-instruction semantic bridge. The normal untagged path performs no
     * opcode decode and reaches one native FEAT_LSFE instruction after a
     * write-TLB lookup.
     */
    const auto slow = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {slow, instruction, pc, count_ + 1, ColdPath::Action::memory_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    if (base == 31U) {
      emit_misalignment_check(8, 16, slow);
    } else {
      emitter_.load_b(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
      emit_misalignment_check(8, bytes, slow);
    }
    /* Natural alignment and a page size divisible by eight prove that this
     * transfer cannot cross a guest-page boundary. */
    emit_translate(8, Access::write, slow, fault, 1);
    track_direct_store(8, bytes);
    /*
     * Resolve vector bindings only after every exceptional edge and its
     * architectural-state snapshot have been recorded. In particular, a
     * fault must not publish an unexecuted result register.
     *
     * Import the source before defining the destination: Rs==Rt is valid and
     * the native instruction must consume the old resident value before
     * replacing it with the returned memory value.
     */
    const auto native_source = read_native_vector(source, 0);
    const auto native_destination =
        decoded.return_old
            ? write_native_vector(destination, false, 1)
            : NativeVector{31U, true};
    auto native =
        instruction & ~((31U << 16) | (31U << 5) | 31U);
    native |= (native_source.host << 16) | (8U << 5) |
              native_destination.host;
    emitter_.emit(native);
    if (decoded.return_old)
      commit_native_vector(native_destination, destination);
    clear_local_exclusive_monitor();
    native_fp_ = true;
  }

  [[nodiscard]] bool emit_pair_memory(std::uint32_t instruction,
                                      std::uint64_t pc) {
    const bool vector = (instruction & 0x0400'0000U) != 0;
    const bool load = (instruction & 0x0040'0000U) != 0;
    const unsigned operation = instruction >> 30;
    if (vector && !host_arm_features().floating_point)
      return false;
    if (vector && operation == 3U &&
        !host_arm_features().has(ArmFeature::lsui))
      return false;
    if (!load)
      mark_direct_memory_write();

    const unsigned mode = (instruction >> 23) & 3U;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned first = instruction & 31U;
    const unsigned second = (instruction >> 10) & 31U;
    const std::size_t bytes =
        vector            ? (operation >= 2U ? 16U
                                             : std::size_t{4} << operation)
        : operation >= 2U ? 8U
                          : 4U;
    const auto displacement =
        sign_extend((instruction >> 15) & 0x7fU, 7) *
        static_cast<std::int64_t>(bytes);
    const auto emit_displacement = [&](unsigned destination,
                                       unsigned source) {
      if (displacement == 0)
        return;
      const auto magnitude = static_cast<unsigned>(
          displacement < 0 ? -displacement : displacement);
      emitter_.emit((displacement < 0 ? 0xd100'0000U : 0x9100'0000U) |
                    (magnitude << 10) | (source << 5) | destination);
    };

    const auto native_base = read_native_gpr(base, true, 8);
    if (native_base.host != 8U)
      emitter_.move_x(8, native_base.host);
    const auto slow = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {slow, instruction, pc, count_ + 1, ColdPath::Action::pair_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    if (base == 31U) {
      /* Pair accesses through SP require the architectural stack alignment
       * check before either memory or writeback can change. */
      emit_misalignment_check(8, 16, slow);
    }
    const bool tagchecked = mode == 1U || mode == 3U || base != 31U;
    if (vector && tagchecked) {
      /* Allocation-tag validation stays on the exact semantic bridge.
       * Untagged programs take the straight native path. */
      emitter_.load_b(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
    }
    if (mode != 1U)
      emit_displacement(8, 8);

    /* The helper remains responsible for cross-page atomicity. The common
     * same-page case needs one TLB lookup and one native pair instruction. */
    emit_translate(8, load ? Access::read : Access::write, slow, fault,
                   bytes * 2U);
    if (!load)
      track_direct_store(8, bytes * 2U);
    NativeGpr native_first_gpr{};
    NativeGpr native_second_gpr{};
    NativeVector native_first_vector{};
    NativeVector native_second_vector{};
    if (vector) {
      if (load) {
        native_first_vector = write_native_vector(first, false, 0);
        native_second_vector =
            second == first
                ? native_first_vector
                : write_native_vector(second, false, 1);
      } else {
        native_first_vector = read_native_vector(first, 0);
        native_second_vector =
            second == first ? native_first_vector
                            : read_native_vector(second, 1);
      }
    } else {
      if (load) {
        native_first_gpr = write_native_gpr(first, false, false, 9);
        native_second_gpr =
            second == first
                ? native_first_gpr
                : write_native_gpr(second, false, false, 10);
      } else {
        native_first_gpr = read_native_gpr(first, false, 9);
        native_second_gpr =
            second == first ? native_first_gpr
                            : read_native_gpr(second, false, 10);
      }
    }

    /* Address generation and writeback were resolved while translating.
     * Preserve the pair operation/mode, replace its immediate with zero, and
     * name the operands' resident host bindings directly. A full cache falls
     * back to the same dedicated scratch registers used by the old path. */
    auto native =
        instruction & ~((0x7fU << 15) | (31U << 10) | (31U << 5) | 31U);
    native |=
        (8U << 5) |
        ((vector ? native_second_vector.host : native_second_gpr.host) << 10) |
        (vector ? native_first_vector.host : native_first_gpr.host);
    emitter_.emit(native);
    if (load) {
      if (vector) {
        commit_native_vector(native_first_vector, first);
        if (second != first)
          commit_native_vector(native_second_vector, second);
      } else {
        commit_native_gpr(native_first_gpr, first, false);
        if (second != first)
          commit_native_gpr(native_second_gpr, second, false);
      }
    } else {
      clear_local_exclusive_monitor();
    }

    const bool writeback =
        (mode == 1U || mode == 3U) &&
        !(load && !vector && base != 31U &&
          (base == first || base == second));
    if (writeback && displacement != 0) {
      const auto native_writeback =
          write_native_gpr(base, true, true, 9);
      emit_displacement(native_writeback.host, native_writeback.host);
      commit_native_gpr(native_writeback, base, true);
    }
    return true;
  }

  [[nodiscard]] bool emit_simd_structure_memory(std::uint32_t instruction,
                                                std::uint64_t pc, bool single) {
    if (!host_arm_features().advanced_simd)
      return false;
    const bool load = (instruction & 0x0040'0000U) != 0;
    const bool post_index = (instruction & 0x0080'0000U) != 0;
    const unsigned offset_register = (instruction >> 16) & 31U;
    if (!post_index && offset_register != 0U)
      return false;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned first = instruction & 31U;
    const std::size_t vector_size =
        (instruction & 0x4000'0000U) != 0 ? 16U : 8U;
    unsigned register_count = 0;
    std::size_t element_size = 0;
    bool load_preserves_destination = false;
    if (!single) {
      const unsigned opcode = (instruction >> 12) & 15U;
      const bool interleaved = opcode == 8U || opcode == 4U || opcode == 0U;
      register_count = opcode == 7U    ? 1U
                       : opcode == 10U ? 2U
                       : opcode == 6U  ? 3U
                       : opcode == 2U  ? 4U
                       : opcode == 8U  ? 2U
                       : opcode == 4U  ? 3U
                       : opcode == 0U  ? 4U
                                       : 0U;
      const unsigned size = (instruction >> 10) & 3U;
      if (register_count == 0U ||
          (interleaved && vector_size == 8U && size == 3U))
        return false;
      element_size = std::size_t{1} << size;
    } else {
      const unsigned extra_register = (instruction >> 21) & 1U;
      const unsigned opcode = (instruction >> 13) & 7U;
      const unsigned scale_bit = (instruction >> 12) & 1U;
      const unsigned size = (instruction >> 10) & 3U;
      const unsigned scale = opcode >> 1;
      if (scale == 3U) {
        if (!load || scale_bit != 0U)
          return false;
        register_count =
            opcode == 6U ? 1U + extra_register : 3U + extra_register;
        element_size = std::size_t{1} << size;
      } else {
        load_preserves_destination = load;
        register_count =
            (opcode & 1U) != 0U ? 3U + extra_register : 1U + extra_register;
        if (scale == 0U) {
          element_size = 1U;
        } else if (scale == 1U) {
          if ((size & 1U) != 0U)
            return false;
          element_size = 2U;
        } else {
          if ((size & 2U) != 0U || ((size & 1U) != 0U && scale_bit != 0U))
            return false;
          element_size = (size & 1U) != 0U ? 8U : 4U;
        }
      }
    }
    const auto transfer_size =
        single ? element_size * register_count : vector_size * register_count;
    if (!load)
      mark_direct_memory_write();

    load_gpr(8, base, true);
    const auto slow = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back({slow, instruction, pc, count_ + 1,
                     ColdPath::Action::simd_structure_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    if (base == 31U) {
      emit_misalignment_check(8, 16, slow);
    }
    if (post_index || base != 31U) {
      /*
       * Untagged programs retain the direct native transfer. Effective MTE
       * state selects the checked cold bridge, which strips the logical tag
       * and validates every touched allocation granule before any transfer.
       */
      emitter_.load_b(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
    }
    emit_translate(8, load ? Access::read : Access::write, slow, fault,
                   transfer_size);
    if (!load)
      track_direct_store(8, transfer_size);
    NativeVector native_vector{};
    if (register_count == 1U) {
      /*
       * A one-register structure operation has an unrestricted Rt field.
       * Keep the guest vector in its resident v16-v31 binding and name that
       * binding in the original instruction.  Selection and validity were
       * resolved above while translating; the generated memory hot path
       * therefore gains no opcode checks or dispatch branches.
       *
       * Lane loads read the untouched lanes, while full and replicate loads
       * overwrite the architectural destination.  A full vector cache uses
       * v0 and the existing commit path exactly as the staged lowering did.
       */
      native_vector =
          load ? write_native_vector(first, load_preserves_destination, 0)
               : read_native_vector(first, 0);
    } else if (!load || load_preserves_destination) {
      for (unsigned index = 0; index < register_count; ++index)
        load_q(index, 15,
               offsetof(CpuState, vectors) +
                   ((first + index) & 31U) *
                       sizeof(arm64::VectorRegister));
    }
    const auto native =
        (instruction & ~(0x0080'0000U | (31U << 16) | (31U << 5) | 31U)) |
        (8U << 5) |
        (register_count == 1U ? native_vector.host : 0U);
    emitter_.emit(native);
    if (load) {
      if (register_count == 1U) {
        commit_native_vector(native_vector, first);
      } else {
        for (unsigned index = 0; index < register_count; ++index)
          store_q(index, 15,
                  offsetof(CpuState, vectors) +
                      ((first + index) & 31U) *
                          sizeof(arm64::VectorRegister));
      }
    } else {
      clear_local_exclusive_monitor();
    }
    if (post_index) {
      load_gpr(8, base, true);
      if (offset_register == 31U)
        emitter_.load_constant(9, transfer_size);
      else
        load_gpr(9, offset_register, false);
      emitter_.add_shifted(8, 8, 9, 0);
      store_gpr(8, base, true);
    }
    return true;
  }

  [[nodiscard]] bool emit_other_memory(std::uint32_t instruction,
                                       std::uint64_t pc) {
    const bool vector = (instruction & 0x0400'0000U) != 0;
    if (vector && !host_arm_features().floating_point)
      return false;
    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    const bool register_offset = (instruction & 0x0020'0000U) != 0;
    const unsigned mode = (instruction >> 10) & 3U;
    const unsigned option = (instruction >> 13) & 7U;
    if (!vector && size_shift == 3U && opc == 2U)
      /* PRFUM/register-offset PRFM are architecturally
       * non-faulting hints. All address work disappears at
       * translation time after validating their encoding. */
      return register_offset ? mode == 2U && (option & 2U) != 0U : mode == 0U;
    if (vector && opc >= 2U && size_shift != 0U)
      return false;
    const std::size_t bytes =
        vector && opc >= 2U ? 16U : std::size_t{1} << size_shift;
    if (!vector && ((bytes == 8U && opc >= 2U) || (bytes == 4U && opc == 3U)))
      return false;
    if ((register_offset && (mode != 2U || (option & 2U) == 0U)) ||
        (!register_offset && vector && mode == 2U))
      return false;
    const bool load = vector ? (opc & 1U) != 0U : opc != 0U;
    if (!load)
      mark_direct_memory_write();
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned transfer = instruction & 31U;
    const auto displacement = sign_extend((instruction >> 12) & 0x1ffU, 9);

    load_gpr(8, base, true);
    if (base == 31U) {
      const auto alignment = emitter_.make_label();
      cold_.push_back({alignment, instruction, pc, count_ + 1,
                       ColdPath::Action::alignment_fault});
      /* Only the translation-selected SP path pays this architectural
       * alignment branch. */
      emit_misalignment_check(8, 16, alignment);
    }
    if (register_offset) {
      const unsigned offset_register = (instruction >> 16) & 31U;
      const unsigned shift =
          (instruction & 0x1000U) != 0U
              ? static_cast<unsigned>(vector && opc >= 2U ? 4U : size_shift)
              : 0U;
      load_gpr(9, offset_register, false);
      emitter_.emit(0x8b20'0000U | (option << 13) | (shift << 10) | (9U << 16) |
                    (8U << 5) | 8U);
    } else if (mode != 1U && displacement != 0) {
      emitter_.load_constant(9, static_cast<std::uint64_t>(displacement));
      emitter_.add_shifted(8, 8, 9, 0);
    }

    const auto fallback = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {fallback, instruction, pc, count_ + 1, ColdPath::Action::memory_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    const bool tagchecked =
        register_offset || mode == 1U || mode == 3U || base != 31U;
    if (vector && tagchecked) {
      /* Tagged-state handling stays on the exact semantic cold path. The
       * ordinary untagged hot path pays one load and one predicted branch. */
      emitter_.load_b(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(fallback, 0, false, true);
    }
    emit_translate(8, load ? Access::read : Access::write, fallback, fault,
                   bytes);
    if (!load)
      track_direct_store(8, bytes);
    const auto native_vector =
        vector
            ? (load ? write_native_vector(transfer, false, 0)
                    : read_native_vector(transfer, 0))
            : NativeVector{};
    const auto native_gpr =
        !vector
            ? (load ? write_native_gpr(transfer, false, false, 9)
                    : read_native_gpr(transfer, false, 9))
            : NativeGpr{};
    const auto native =
        0x3900'0000U |
        (instruction & (0xc000'0000U | 0x0400'0000U | 0x00c0'0000U)) |
        (8U << 5) | (vector ? native_vector.host : native_gpr.host);
    emitter_.emit(native);
    if (load) {
      if (vector)
        commit_native_vector(native_vector, transfer);
      else
        commit_native_gpr(native_gpr, transfer, false);
    } else {
      clear_local_exclusive_monitor();
    }

    const bool writeback =
        !register_offset && (mode == 1U || mode == 3U) &&
        !(load && !vector && base == transfer && base != 31U);
    if (writeback) {
      load_gpr(8, base, true);
      if (displacement != 0) {
        emitter_.load_constant(9, static_cast<std::uint64_t>(displacement));
        emitter_.add_shifted(8, 8, 9, 0);
      }
      store_gpr(8, base, true);
    }
    return true;
  }

  [[nodiscard]] bool emit_literal_memory(std::uint32_t instruction,
                                         std::uint64_t pc) {
    if ((instruction & 0xff00'0000U) == 0xd800'0000U)
      /* Literal PRFM is an architecturally non-faulting hint. */
      return true;
    if ((instruction & 0x3b00'0000U) != 0x1800'0000U)
      return false;
    const bool vector = (instruction & 0x0400'0000U) != 0;
    const unsigned operation = instruction >> 30;
    if ((vector && !host_arm_features().floating_point) ||
        (vector && operation == 3U))
      return false;
    const std::size_t bytes = vector            ? std::size_t{4} << operation
                              : operation == 1U ? 8U
                                                : 4U;
    const auto displacement =
        sign_extend((instruction >> 5) & 0x7ffffU, 19) * 4;
    const auto address = pc + static_cast<std::uint64_t>(displacement);
    const unsigned destination = instruction & 31U;
    emitter_.load_constant(8, address);
    const auto fallback = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {fallback, instruction, pc, count_ + 1, ColdPath::Action::memory_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    emit_translate(8, Access::read, fallback, fault, bytes);
    if (vector) {
      static constexpr std::array<std::uint32_t, 3> loads{
          0xbd40'0100U, // ldr s0, [x8]
          0xfd40'0100U, // ldr d0, [x8]
          0x3dc0'0100U, // ldr q0, [x8]
      };
      const auto native_destination =
          write_native_vector(destination, false, 0);
      emitter_.emit(loads[operation] | native_destination.host);
      commit_native_vector(native_destination, destination);
    } else {
      static constexpr std::array<std::uint32_t, 3> loads{
          0xb940'0109U, // ldr w9, [x8]
          0xf940'0109U, // ldr x9, [x8]
          0xb980'0109U, // ldrsw x9, [x8]
      };
      const auto native_destination =
          write_native_gpr(destination, false, false, 9);
      emitter_.emit((loads[operation] & ~31U) | native_destination.host);
      commit_native_gpr(native_destination, destination, false);
    }
    return true;
  }

  struct SimdUnsignedMemoryOperation final {
    std::uint32_t instruction{};
    std::uint64_t offset{};
    std::size_t bytes{};
    unsigned base{};
    unsigned transfer{};
    bool load{};
  };

  [[nodiscard]] static bool decode_simd_unsigned_memory(
      std::uint32_t instruction,
      SimdUnsignedMemoryOperation &operation) noexcept {
    if ((instruction & 0x3b00'0000U) != 0x3900'0000U ||
        (instruction & 0x0400'0000U) == 0U)
      return false;

    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    if (opc >= 2U && size_shift != 0U)
      return false;

    const std::size_t bytes =
        opc >= 2U ? 16U : std::size_t{1} << size_shift;
    operation = {
        .instruction = instruction,
        .offset = static_cast<std::uint64_t>(
                      (instruction >> 10) & 0xfffU) *
                  bytes,
        .bytes = bytes,
        .base = (instruction >> 5) & 31U,
        .transfer = instruction & 31U,
        .load = (opc & 1U) != 0U,
    };
    return true;
  }

  [[nodiscard]] std::size_t try_emit_simd_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    constexpr std::size_t maximum_run = 8;
    if (track_direct_stores_ || !host_arm_features().floating_point ||
        instructions.size() < 2)
      return 0;

    std::array<SimdUnsignedMemoryOperation, maximum_run> operations{};
    std::array<std::uint64_t, maximum_run> maximum_ends{};
    std::size_t candidates = 0;
    unsigned base = 0;
    bool load = false;
    std::uint64_t maximum_end = 0;
    while (candidates < operations.size() &&
           candidates < instructions.size()) {
      SimdUnsignedMemoryOperation operation{};
      if (!decode_simd_unsigned_memory(instructions[candidates], operation))
        break;
      if (candidates == 0) {
        base = operation.base;
        load = operation.load;
      } else if (operation.base != base || operation.load != load) {
        break;
      }
      maximum_end =
          std::max(maximum_end, operation.offset + operation.bytes);
      operations[candidates] = operation;
      maximum_ends[candidates] = maximum_end;
      ++candidates;
    }

    std::size_t length = candidates;
    while (length >= 2 && maximum_ends[length - 1] > page_size)
      --length;
    if (length < 2)
      return 0;

    /*
     * Every guard and the permission lookup precedes the first architectural
     * memory operation.  A rejected proof therefore enters the ordinary
     * semantic path for operation zero with the complete dirty prefix still
     * visible, preserving its effective fault address and retirement count.
     */
    const auto slow = emitter_.make_label();
    cold_.push_back({slow, operations[0].instruction, pc, count_ + 1,
                     ColdPath::Action::memory_slow});

    Arm64Emitter::Label alignment{};
    if (base == 31U) {
      alignment = emitter_.make_label();
      cold_.push_back({alignment, operations[0].instruction, pc, count_ + 1,
                       ColdPath::Action::alignment_fault});
    }

    load_gpr(8, base, true);
    if (base == 31U) {
      /* All retained operations name SP, so its architectural alignment is
       * checked once before any destination or memory becomes observable. */
      emit_misalignment_check(8, 16, alignment);
    } else {
      /*
       * Tagged-address handling remains on the existing exact semantic path.
       * Consecutive operations share the immutable effective state, so one
       * translation-selected check is sufficient for the complete run.
       */
      emitter_.load_b(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(slow, 0, false, true);
    }

    maximum_end = maximum_ends[length - 1];
    if (base == 31U) {
      const auto last_unit =
          static_cast<unsigned>((maximum_end - 1U) >> 4U);
      if (last_unit != 0U) {
        /*
         * SP is 16-byte aligned.  Prove the furthest retained byte remains in
         * the page before translating SP itself once.
         */
        emitter_.ubfm(0, 8, 4, page_shift - 1);
        emitter_.add_immediate(0, 0, last_unit);
        emitter_.test_zero(slow, 0, page_shift - 4U, true);
      }
    } else {
      const auto last_delta = maximum_end - 1U;
      if (last_delta != 0U) {
        if (last_delta <= 0xfffU) {
          emitter_.add_immediate(0, 8,
                                 static_cast<unsigned>(last_delta));
        } else {
          emitter_.load_constant(0, last_delta);
          emitter_.add_shifted(0, 8, 0, 0);
        }
        emitter_.emit(0xca08'0000U); // eor x0, x0, x8
        emitter_.test_zero(slow, 0, page_shift,
                           true); // tbnz x0, #page_shift
      }
    }

    emit_translate(8, load ? Access::read : Access::write, slow, slow, 1);
    for (std::size_t index = 0; index < length; ++index) {
      const auto &operation = operations[index];
      const auto native_vector =
          load ? write_native_vector(operation.transfer, false, 0)
               : read_native_vector(operation.transfer, 0);
      const auto native =
          (operation.instruction & ~((31U << 5) | 31U)) | (8U << 5) |
          native_vector.host;
      emitter_.emit(native);
      if (load)
        commit_native_vector(native_vector, operation.transfer);
    }

    if (!load) {
      for (std::size_t index = 0; index < length; ++index) {
        const auto instruction_index = count_ + index;
        if (instruction_index >= maximum_block_instructions)
          break;
        const auto bit = std::uint64_t{1} << instruction_index;
        memory_write_mask_ |= bit;
        direct_memory_write_mask_ |= bit;
      }
      clear_local_exclusive_monitor();
    }
    return length;
  }

  struct SpUnsignedMemoryOperation final {
    std::uint32_t instruction{};
    std::uint64_t offset{};
    std::size_t bytes{};
    unsigned transfer{};
    bool load{};
  };

  [[nodiscard]] static bool decode_sp_unsigned_memory(
      std::uint32_t instruction,
      SpUnsignedMemoryOperation &operation) noexcept {
    if ((instruction & 0x3b00'0000U) != 0x3900'0000U ||
        (instruction & 0x0400'0000U) != 0U ||
        ((instruction >> 5) & 31U) != 31U)
      return false;

    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    if ((size_shift == 3U && opc >= 2U) ||
        (size_shift == 2U && opc == 3U))
      return false;

    operation = {
        .instruction = instruction,
        .offset = static_cast<std::uint64_t>(
                      (instruction >> 10) & 0xfffU)
                  << size_shift,
        .bytes = std::size_t{1} << size_shift,
        .transfer = instruction & 31U,
        .load = opc != 0U,
    };
    return true;
  }

  [[nodiscard]] std::size_t try_emit_sp_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    constexpr std::size_t maximum_run = 4;
    if (track_direct_stores_ || instructions.size() < 2)
      return 0;

    std::array<SpUnsignedMemoryOperation, maximum_run> operations{};
    std::array<std::uint64_t, maximum_run> maximum_ends{};
    std::size_t candidates = 0;
    bool load = false;
    std::uint64_t maximum_end = 0;
    while (candidates < operations.size() &&
           candidates < instructions.size()) {
      SpUnsignedMemoryOperation operation{};
      if (!decode_sp_unsigned_memory(instructions[candidates], operation))
        break;
      if (candidates == 0)
        load = operation.load;
      else if (operation.load != load)
        break;
      maximum_end =
          std::max(maximum_end, operation.offset + operation.bytes);
      operations[candidates] = operation;
      maximum_ends[candidates] = maximum_end;
      ++candidates;
    }

    std::size_t length = candidates;
    while (length >= 2 && maximum_ends[length - 1] > page_size)
      --length;
    if (length < 2)
      return 0;

    const auto alignment = emitter_.make_label();
    const auto slow = emitter_.make_label();
    cold_.push_back({alignment, operations[0].instruction, pc, count_ + 1,
                     ColdPath::Action::alignment_fault});
    cold_.push_back({slow, operations[0].instruction, pc, count_ + 1,
                     ColdPath::Action::memory_slow});

    load_gpr(8, 31, true);
    /* Every selected instruction names SP, so check its architectural
     * alignment once before observing any memory or destination register. */
    emit_misalignment_check(8, 16, alignment);

    maximum_end = maximum_ends[length - 1];
    const auto last_unit =
        static_cast<unsigned>((maximum_end - 1U) >> 4U);
    if (last_unit != 0U) {
      /*
       * SP is known to be 16-byte aligned. q+last_unit is below twice the
       * number of units in a page, so its page bit is the complete crossing
       * test.
       */
      emitter_.ubfm(0, 8, 4, page_shift - 1);
      emitter_.add_immediate(0, 0, last_unit);
      emitter_.test_zero(slow, 0, page_shift - 4U, true);
    }

    emit_translate(8, load ? Access::read : Access::write, slow, slow, 1);
    for (std::size_t index = 0; index < length; ++index) {
      const auto &operation = operations[index];
      const auto native_gpr =
          load ? write_native_gpr(operation.transfer, false, false, 9)
               : read_native_gpr(operation.transfer, false, 9);
      const auto native =
          (operation.instruction & ~((31U << 5) | 31U)) | (8U << 5) |
          native_gpr.host;
      emitter_.emit(native);
      if (load)
        commit_native_gpr(native_gpr, operation.transfer, false);
    }

    if (!load) {
      for (std::size_t index = 0; index < length; ++index) {
        const auto instruction_index = count_ + index;
        if (instruction_index >= maximum_block_instructions)
          break;
        const auto bit = std::uint64_t{1} << instruction_index;
        memory_write_mask_ |= bit;
        direct_memory_write_mask_ |= bit;
      }
      clear_local_exclusive_monitor();
    }
    return length;
  }

  struct GprUnsignedMemoryOperation final {
    std::uint32_t instruction{};
    std::uint64_t offset{};
    std::size_t bytes{};
    unsigned base{};
    unsigned transfer{};
    bool load{};
  };

  [[nodiscard]] static bool decode_gpr_unsigned_memory(
      std::uint32_t instruction,
      GprUnsignedMemoryOperation &operation) noexcept {
    if ((instruction & 0x3b00'0000U) != 0x3900'0000U ||
        (instruction & 0x0400'0000U) != 0U)
      return false;

    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    const unsigned base = (instruction >> 5) & 31U;
    if (base == 31U || (size_shift == 3U && opc >= 2U) ||
        (size_shift == 2U && opc == 3U))
      return false;

    operation = {
        .instruction = instruction,
        .offset = static_cast<std::uint64_t>(
                      (instruction >> 10) & 0xfffU)
                  << size_shift,
        .bytes = std::size_t{1} << size_shift,
        .base = base,
        .transfer = instruction & 31U,
        .load = opc != 0U,
    };
    return true;
  }

  [[nodiscard]] std::size_t try_emit_gpr_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    constexpr std::size_t maximum_run = 4;
    if (track_direct_stores_ || instructions.size() < 2)
      return 0;

    std::array<GprUnsignedMemoryOperation, maximum_run> operations{};
    std::array<std::uint64_t, maximum_run> maximum_ends{};
    std::size_t candidates = 0;
    unsigned base = 0;
    bool load = false;
    std::uint64_t maximum_end = 0;
    while (candidates < operations.size() &&
           candidates < instructions.size()) {
      GprUnsignedMemoryOperation operation{};
      if (!decode_gpr_unsigned_memory(instructions[candidates], operation))
        break;
      if (candidates == 0) {
        base = operation.base;
        load = operation.load;
      } else if (operation.base != base || operation.load != load) {
        break;
      }
      /*
       * A load into Rn changes the architectural base observed by every
       * following instruction. Keeping the translated host base in x8 would
       * therefore be stale. Store Rt==Rn is safe because stores do not change
       * the source register.
       */
      if (load && operation.transfer == base)
        break;
      maximum_end =
          std::max(maximum_end, operation.offset + operation.bytes);
      operations[candidates] = operation;
      maximum_ends[candidates] = maximum_end;
      ++candidates;
    }

    std::size_t length = candidates;
    while (length >= 2 && maximum_ends[length - 1] > page_size)
      --length;
    if (length < 2)
      return 0;

    /*
     * Record the semantic first-instruction edge before loading or allocating
     * any run register. If the page proof or permission translation fails,
     * the ordinary slow helper executes exactly operation zero with the
     * architectural prefix state and precise fault address.
     */
    const auto slow = emitter_.make_label();
    cold_.push_back({slow, operations[0].instruction, pc, count_ + 1,
                     ColdPath::Action::memory_slow});

    load_gpr(8, base, true);
    maximum_end = maximum_ends[length - 1];
    const auto last_delta = maximum_end - 1U;
    if (last_delta != 0U) {
      if (last_delta <= 0xfffU) {
        emitter_.add_immediate(0, 8,
                               static_cast<unsigned>(last_delta));
      } else {
        emitter_.load_constant(0, last_delta);
        emitter_.add_shifted(0, 8, 0, 0);
      }
      /*
       * last_delta is translation-time bounded below one guest page. The page
       * bit changes iff base..base+last_delta crosses a page boundary.
       */
      emitter_.emit(0xca08'0000U); // eor x0, x0, x8
      emitter_.test_zero(slow, 0, page_shift, true); // tbnz x0, #page_shift
    }

    emit_translate(8, load ? Access::read : Access::write, slow, slow, 1);
    for (std::size_t index = 0; index < length; ++index) {
      const auto &operation = operations[index];
      const auto native_gpr =
          load ? write_native_gpr(operation.transfer, false, false, 9)
               : read_native_gpr(operation.transfer, false, 9);
      const auto native =
          (operation.instruction & ~((31U << 5) | 31U)) | (8U << 5) |
          native_gpr.host;
      emitter_.emit(native);
      if (load)
        commit_native_gpr(native_gpr, operation.transfer, false);
    }

    if (!load) {
      for (std::size_t index = 0; index < length; ++index) {
        const auto instruction_index = count_ + index;
        if (instruction_index >= maximum_block_instructions)
          break;
        const auto bit = std::uint64_t{1} << instruction_index;
        memory_write_mask_ |= bit;
        direct_memory_write_mask_ |= bit;
      }
      clear_local_exclusive_monitor();
    }
    return length;
  }

  [[nodiscard]] bool emit_unsigned_memory(std::uint32_t instruction,
                                          std::uint64_t pc) {
    const bool vector = (instruction & 0x0400'0000U) != 0;
    if (vector && !host_arm_features().floating_point)
      return false;
    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    /*
     * PRFM is an architecturally non-faulting hint.  Its Rt field is a
     * prefetch operation, not a destination register.  Treating opc=2
     * uniformly as a signed load corrupts that register and can also
     * introduce a spurious memory fault.
     */
    if (!vector && size_shift == 3U && opc == 2U)
      return true;
    if (vector && opc >= 2U && size_shift != 0U)
      return false;
    const std::size_t bytes =
        vector && opc >= 2U ? 16U : std::size_t{1} << size_shift;
    if (!vector && ((bytes == 8U && opc >= 2U) || (bytes == 4U && opc == 3U)))
      return false;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned transfer = instruction & 31U;
    const auto offset =
        static_cast<std::uint64_t>((instruction >> 10) & 0xfffU) * bytes;
    /* opc==0 is the only scalar store allocation. Signed loads use
     * opc==2/3, so testing only the ordinary-load bit would silently
     * discard every LDRSB/LDRSH/LDRSW result.
     */
    const bool load = vector ? (opc & 1U) != 0 : opc != 0;
    if (!load)
      mark_direct_memory_write();
    load_gpr(8, base, true);
    if (base == 31U) {
      const auto alignment = emitter_.make_label();
      cold_.push_back({alignment, instruction, pc, count_ + 1,
                       ColdPath::Action::alignment_fault});
      emit_misalignment_check(8, 16, alignment);
    }
    if (offset != 0) {
      emitter_.load_constant(9, offset);
      emitter_.add_shifted(8, 8, 9, 0);
    }
    const auto fallback = emitter_.make_label();
    const auto fault = emitter_.make_label();
    cold_.push_back(
        {fallback, instruction, pc, count_ + 1, ColdPath::Action::memory_slow});
    cold_.push_back(
        {fault, instruction, pc, count_ + 1, ColdPath::Action::memory_fault});
    if (vector && base != 31U) {
      emitter_.load_b(0, 15, offsetof(CpuState, mte_enabled));
      emitter_.compare_zero(fallback, 0, false, true);
    }
    emit_translate(8, load ? Access::read : Access::write, fallback, fault,
                   bytes);
    if (!load)
      track_direct_store(8, bytes);
    const auto native_vector =
        vector
            ? (load ? write_native_vector(transfer, false, 0)
                    : read_native_vector(transfer, 0))
            : NativeVector{};
    const auto native_gpr =
        !vector
            ? (load ? write_native_gpr(transfer, false, false, 9)
                    : read_native_gpr(transfer, false, 9))
            : NativeGpr{};
    const auto native = (instruction & ~((0xfffU << 10) | (31U << 5) | 31U)) |
                        (8U << 5) |
                        (vector ? native_vector.host : native_gpr.host);
    emitter_.emit(native);
    if (load) {
      if (vector)
        commit_native_vector(native_vector, transfer);
      else
        commit_native_gpr(native_gpr, transfer, false);
    } else {
      clear_local_exclusive_monitor();
    }
    return true;
  }

  /*
   * These ordinary control-flow classes terminate almost every translated
   * block and do not overlap the system, ordered-memory, or extension
   * allocations handled below. Resolve them immediately after architectural
   * exception instructions so cold translation does not walk the complete
   * decoder for a branch already known to end the block.
   */
  bool emit_common_control_flow(std::uint32_t instruction, std::uint64_t pc,
                                std::size_t resulting_count) {
    if ((instruction & 0x7c00'0000U) == 0x1400'0000U) {
      const auto target =
          pc + sign_extend(instruction & 0x03ff'ffffU, 26) * 4;
      if ((instruction & 0x8000'0000U) != 0) {
        emitter_.load_literal_constant(8, pc + 4);
        store_gpr(8, 30, false);
      }
      emit_direct_branch(target, resulting_count);
      return true;
    }
    if ((instruction & 0xff00'0000U) == 0x5400'0000U) {
      const auto target =
          pc + sign_extend((instruction >> 5) & 0x7ffffU, 19) * 4;
      emit_conditional_pc(instruction & 15U, target, pc + 4,
                          resulting_count);
      return true;
    }
    if ((instruction & 0x7e00'0000U) == 0x3400'0000U) {
      emit_compare_branch(instruction, pc, resulting_count);
      return true;
    }
    if ((instruction & 0x7e00'0000U) == 0x3600'0000U) {
      emit_test_branch(instruction, pc, resulting_count);
      return true;
    }
    if ((instruction & 0xffff'fc1fU) == 0xd61f'0000U ||
        (instruction & 0xffff'fc1fU) == 0xd63f'0000U ||
        (instruction & 0xffff'fc1fU) == 0xd65f'0000U) {
      emit_register_branch(instruction, pc, resulting_count);
      return true;
    }
    return false;
  }

  Action emit_instruction(std::uint32_t instruction, std::uint64_t pc) {
    const auto resulting_count = count_ + 1;
    if ((instruction & 0xffff'0000U) == 0U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffe0'001fU) == 0xd400'0001U) {
      store_pc(pc + 4);
      emitter_.return_constant(Event::system_call, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffe0'001fU) == 0xd420'0000U) {
      store_pc(pc);
      emitter_.return_constant(Event::breakpoint, resulting_count);
      return Action::terminate;
    }
    if (emit_common_control_flow(instruction, pc, resulting_count))
      return Action::terminate;
    if ((instruction & 0xffe0'001fU) == 0xd400'0002U ||
        (instruction & 0xffe0'001fU) == 0xd400'0003U ||
        (instruction & 0xffe0'001fU) == 0xd440'0000U ||
        (instruction & 0xffe0'001fU) == 0xd4a0'0001U ||
        (instruction & 0xffe0'001fU) == 0xd4a0'0002U ||
        (instruction & 0xffe0'001fU) == 0xd4a0'0003U ||
        instruction == 0xd6bf'03e0U || instruction == 0xd69f'03e0U ||
        instruction == 0xd69f'0bffU || instruction == 0xd69f'0fffU) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (const auto gcs = arm64::detail::decode_gcs(instruction); gcs) {
      if (arm64::detail::gcs_is_privileged_exception_operation(
              gcs.operation)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_arm_features().has(ArmFeature::gcs))
        return Action::unsupported;
      if (arm64::detail::gcs_writes_memory(gcs.operation))
        mark_memory_write();
      emit_gcs_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    /*
     * Ordered-memory allocations overlap broader ordinary, structure, and
     * atomic load/store classes. Resolve the complete allocations here so
     * reserved encodings are rejected before any address is observed and so
     * valid native-JIT cases never fall through to gadget translation.
     *
     * The checked memory bridge is a cold-path execution boundary: register,
     * width, ordering, and addressing-mode decode remains at translation
     * time, while the bridge supplies the software-MMU address override.
     */
    const auto reject_ordered = [&] {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    };
    const auto ordered_memory = [&](bool store) {
      if (store)
        mark_memory_write();
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    };

    if ((instruction & 0xbfbf'fc00U) == 0x0d01'8400U) {
      if (!host_arm_features().has(ArmFeature::advanced_simd) ||
          !host_arm_features().has(ArmFeature::lrcpc3))
        return Action::unsupported;
      return ordered_memory((instruction & 0x0040'0000U) == 0U);
    }
    if ((instruction & 0x3f20'0c00U) == 0x1d00'0800U) {
      const unsigned size = instruction >> 30;
      const unsigned operation = (instruction >> 22) & 3U;
      if ((operation & 2U) != 0U && size != 0U)
        return reject_ordered();
      if (!host_arm_features().has(ArmFeature::floating_point) ||
          !host_arm_features().has(ArmFeature::lrcpc3))
        return Action::unsupported;
      return ordered_memory((operation & 1U) == 0U);
    }
    if ((instruction & 0x3fff'fc00U) == 0x38bf'c000U) {
      if (!host_arm_features().has(ArmFeature::lrcpc))
        return Action::unsupported;
      return ordered_memory(false);
    }
    if ((instruction & 0x3f20'0c00U) == 0x1900'0000U) {
      const unsigned size = instruction >> 30;
      const unsigned operation = (instruction >> 22) & 3U;
      if ((operation == 2U && size == 3U) ||
          (operation == 3U && size >= 2U))
        return reject_ordered();
      if (!host_arm_features().has(ArmFeature::lrcpc2))
        return Action::unsupported;
      return ordered_memory(operation == 0U);
    }
    if ((instruction & 0x3fbf'fc00U) == 0x1980'0800U) {
      if ((instruction >> 30) < 2U)
        return reject_ordered();
      if (!host_arm_features().has(ArmFeature::lrcpc3))
        return Action::unsupported;
      return ordered_memory((instruction & 0x0040'0000U) == 0U);
    }
    if ((instruction & 0x3f20'0c00U) == 0x1900'0800U) {
      const auto fixed = instruction & 0xffe0'fc00U;
      const bool lrcpc3 =
          fixed == 0x9900'0800U || fixed == 0xd900'0800U ||
          fixed == 0x9900'1800U || fixed == 0xd900'1800U ||
          fixed == 0x9940'0800U || fixed == 0xd940'0800U ||
          fixed == 0x9940'1800U || fixed == 0xd940'1800U;
      const bool lscp = fixed == 0xd900'5800U ||
                        fixed == 0xd940'5800U ||
                        fixed == 0xd940'7800U;
      if (!lrcpc3 && !lscp)
        return reject_ordered();
      const bool load =
          fixed != 0x9900'0800U && fixed != 0xd900'0800U &&
          fixed != 0x9900'1800U && fixed != 0xd900'1800U &&
          fixed != 0xd900'5800U;
      const unsigned first = instruction & 31U;
      const unsigned second = (instruction >> 16) & 31U;
      if (load && first == second && first != 31U)
        return reject_ordered();
      const auto required =
          lrcpc3 ? ArmFeature::lrcpc3 : ArmFeature::lscp;
      if (!host_arm_features().has(required))
        return Action::unsupported;
      return ordered_memory(!load);
    }
    /*
     * Resolve the complete tag-immediate allocation before the ordinary
     * immediate-arithmetic decoder. Only the two fully fixed forms are
     * allocated; native execution of those forms is host-feature gated by
     * the test harness and caller.
     */
    if ((instruction & 0x1f80'0000U) == 0x1180'0000U) {
      const bool allocated =
          (instruction & 0xffc0'c000U) == 0x9180'0000U ||
          (instruction & 0xffc0'c000U) == 0xd180'0000U;
      if (allocated)
        return Action::unsupported;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    /*
     * Tag-memory mode zero contains one ordinary load form. Its other
     * members are inaccessible at EL0 or unallocated. Other addressing
     * modes are valid extension instructions and remain host gated.
     */
    if ((instruction & 0xff20'0000U) == 0xd920'0000U) {
      const unsigned operation = (instruction >> 22) & 3U;
      const unsigned mode = (instruction >> 10) & 3U;
      if (mode != 0U || operation == 1U)
        return Action::unsupported;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    const auto system_register = instruction & 0xffff'ffe0U;
    if ((system_register == 0xd53b'4440U ||
         system_register == 0xd51b'4440U) &&
        !translation_supports(ArmFeature::fp8))
      return Action::unsupported;
    if (emit_system_register(instruction)) {
      /*
       * A register-sourced FPCR write may enable precise guest exception
       * reporting.  Keep ordinary control-only writes inside the native
       * block, but leave immediately when the newly stored value actually
       * contains an enable so the next instruction passes through the cold
       * gate.  The host write itself has already masked guest-only enables.
       */
      if ((instruction & 0xffff'ffe0U) == 0xd51b'4400U &&
          (instruction & 31U) != 31U) {
        const auto no_exception_enable = emitter_.make_label();
        emitter_.load_w(8, 15, offsetof(CpuState, fpcr));
        emitter_.load_constant(
            9, arm64::detail::fpcr_exception_enable_mask);
        emitter_.emit_internal_flag_clobber(
            0x6a00'001fU | (9U << 16) | (8U << 5));
        emitter_.branch_condition(no_exception_enable, 0); // EQ
        store_pc(pc + sizeof(std::uint32_t));
        emitter_.return_constant(Event::continued, resulting_count);
        emitter_.bind(no_exception_enable);
      }
      return Action::keep_going;
    }
    if ((instruction & 0xffd0'0000U) == 0xd510'0000U ||
        (instruction & 0xffd0'0000U) == 0xd550'0000U ||
        (instruction & 0xfff8'0000U) == 0xd548'0000U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffff'ffe0U) == 0xd50b'7420U) {
      emit_cache_zero(instruction, pc);
      return Action::keep_going;
    }
    if ((instruction & 0xffff'ffe0U) == 0xd50b'7b20U)
      return Action::keep_going;
    if ((instruction & 0xffff'ffe0U) == 0xd50b'7520U) {
      emit_code_synchronize(instruction, pc, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xfff8'0000U) == 0xd508'0000U ||
        (instruction & 0xfff8'0000U) == 0xd528'0000U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xfff8'f01fU) == 0xd500'401fU) {
      const unsigned op1 = (instruction >> 16) & 7U;
      const unsigned immediate = (instruction >> 8) & 15U;
      const unsigned op2 = (instruction >> 5) & 7U;
      if (op1 == 0U && immediate == 0U && op2 <= 2U) {
        if (emit_flag_manipulation(instruction))
          return Action::keep_going;
        return Action::unsupported;
      } else {
        const auto reject = [&] {
          store_pc(pc);
          emitter_.return_constant(Event::illegal_instruction,
                                   resulting_count);
          return Action::terminate;
        };
        if (op1 != 3U)
          return reject();

        const auto store_boolean = [&](std::size_t offset) {
          emitter_.load_constant(8, immediate & 1U);
          emitter_.store_b(8, 15, offset);
        };
        switch (op2) {
        case 1:
          if (!translation_supports(ArmFeature::ssbs))
            return Action::unsupported;
          store_boolean(offsetof(CpuState, pstate_ssbs));
          return Action::keep_going;
        case 2:
          if (!translation_supports(ArmFeature::dit))
            return Action::unsupported;
          store_boolean(offsetof(CpuState, pstate_dit));
          return Action::keep_going;
        case 3:
          if ((immediate & 8U) != 0)
            return reject();
          switch ((immediate >> 1) & 3U) {
          case 1:
            if (!translation_supports(ArmFeature::sme))
              return Action::unsupported;
            store_boolean(offsetof(CpuState, pstate_sm));
            return Action::keep_going;
          case 2:
            if (!translation_supports(ArmFeature::sme))
              return Action::unsupported;
            store_boolean(offsetof(CpuState, pstate_za));
            return Action::keep_going;
          case 3:
            if (!translation_supports(ArmFeature::sme))
              return Action::unsupported;
            store_boolean(offsetof(CpuState, pstate_sm));
            store_boolean(offsetof(CpuState, pstate_za));
            return Action::keep_going;
          default:
            return reject();
          }
        case 4:
          if (!translation_supports(ArmFeature::mte))
            return Action::unsupported;
          store_boolean(offsetof(CpuState, pstate_tco));
          return Action::keep_going;
        case 6:
        case 7: {
          const auto denied = emitter_.make_label();
          const auto complete = emitter_.make_label();
          emitter_.load_b(8, 15, offsetof(CpuState, pstate_daif_access));
          emitter_.compare_zero(denied, 8, false, false);
          emitter_.load_b(8, 15, offsetof(CpuState, pstate_daif));
          emitter_.load_constant(9, immediate);
          emitter_.emit((op2 == 6 ? 0x2a00'0000U : 0x0a20'0000U) |
                        (9U << 16) | (8U << 5) | 8U);
          emitter_.store_b(8, 15, offsetof(CpuState, pstate_daif));
          emitter_.branch(complete);
          emitter_.bind(denied);
          store_pc(pc);
          emitter_.return_constant(Event::illegal_instruction,
                                   resulting_count);
          emitter_.bind(complete);
          return Action::keep_going;
        }
        default:
          return reject();
        }
      }
    }
    if ((instruction & 0xffe0'7c10U) == 0xba00'0400U ||
        (instruction & 0xffff'bc1fU) == 0x3a00'080dU) {
      if (emit_flag_manipulation(instruction))
        return Action::keep_going;
      return Action::unsupported;
    }
    if ((instruction & 0xffe0'0400U) == 0xba00'0400U ||
        (instruction & 0xffff'0800U) == 0x3a00'0800U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffff'f0ffU) == 0xd503'309fU ||
        (instruction & 0xffff'f0ffU) == 0xd503'30bfU ||
        (instruction & 0xffff'f0ffU) == 0xd503'30dfU ||
        (instruction & 0xffff'f3ffU) == 0xd503'323fU ||
        instruction == 0xd503'30ffU) {
      const auto lowered = lower_memory_barrier(
          instruction, host_arm_features().has(ArmFeature::sb),
          host_arm_features().has(ArmFeature::xs));
      for (std::size_t index = 0; index < lowered.count; ++index)
        emitter_.emit(lowered.words[index]);
      return Action::keep_going;
    }
    if ((instruction & 0xffff'f0ffU) == 0xd503'30ffU) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffff'f0ffU) == 0xd503'305fU) {
      emitter_.emit(instruction);
      emitter_.load_constant(8, ~std::uint64_t{0});
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_address));
      emitter_.load_constant(8, 0);
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_key));
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_value));
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_value2));
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_version));
      emitter_.store_w(8, 15, offsetof(CpuState, exclusive_size));
      return Action::keep_going;
    }
    if ((instruction & 0xffff'f01fU) == 0xd503'201fU) {
      const unsigned immediate = (instruction >> 5) & 0x7fU;
      const bool guest_key_operation =
          immediate == 7U || immediate == 8U || immediate == 10U ||
          immediate == 12U || immediate == 14U ||
          (immediate >= 24U && immediate <= 31U);
      if (guest_key_operation) {
        if (!enable_pointer_authentication_)
          return Action::keep_going;
        emit_memory_slow(instruction, pc, resulting_count);
        return Action::terminate;
      }
    }
    if (emit_primary_hint(instruction) ||
        (instruction & 0xffd8'f000U) == 0xd500'2000U)
      return Action::keep_going;
    /*
     * The guest profile does not expose the enhanced return-address
     * authentication extension. Resolve its complete fixed, immediate, and
     * register allocations here so valid unavailable forms and reserved
     * neighbors terminate deterministically instead of entering an
     * unsupported native block.
     */
    if ((instruction & 0xffff'c000U) == 0xdac1'8000U ||
        (instruction & 0xffc0'0000U) == 0xf380'0000U ||
        (instruction & 0xffc0'0000U) == 0x5500'0000U ||
        ((instruction & 0xffff'fbe0U) == 0xd65f'0be0U &&
         (instruction & 31U) != 31U)) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xfedf'f800U) == 0xd61f'0800U ||
        (instruction & 0xffff'fbffU) == 0xd65f'0bffU) {
      if (!enable_pointer_authentication_) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      const bool return_form =
          (instruction & 0xffff'fbffU) == 0xd65f'0bffU;
      const bool zero_modifier =
          !return_form && (instruction & 0x0100'0000U) == 0;
      if (zero_modifier && (instruction & 31U) != 31U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      const bool link =
          !return_form && (instruction & 0x0020'0000U) != 0;
      if (enable_guarded_control_flow_ && link)
        mark_memory_write();
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffff'ffe0U) == 0xd503'1000U ||
        (instruction & 0xffff'ffe0U) == 0xd503'1020U) {
      /* A timed wait is a hint and is permitted to complete immediately. */
      return Action::keep_going;
    }
    if ((instruction & 0x7e00'0000U) == 0x7400'0000U) {
      const bool immediate = (instruction & 0x0100'0000U) != 0;
      const unsigned narrow_control = (instruction >> 14) & 3U;
      const unsigned condition = (instruction >> 21) & 7U;
      const bool valid_form =
          immediate ? (instruction & 0x0000'4000U) == 0
                    : (narrow_control == 0U ||
                       ((instruction >> 31) == 0U &&
                        narrow_control >= 2U));
      if (!valid_form || condition == 4U || condition == 5U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      return Action::unsupported;
    }
    if ((instruction & 0x1f00'0000U) == 0x1000'0000U) {
      emit_pc_relative(instruction, pc);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1280'0000U) {
      if (!valid_move_wide(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_move_wide(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x7ff0'0000U) == 0x11c0'0000U) {
      if (!host_arm_features().cssc)
        return Action::unsupported;
      emit_integer_min_max(instruction, true);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1100'0000U) {
      emit_add_sub_immediate(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f20'0000U) == 0x0b00'0000U) {
      if (!valid_shifted_register(instruction, true)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_native_binary(instruction, false, (instruction & 0x2000'0000U) != 0);
      return Action::keep_going;
    }
    if ((instruction & 0x1fe0'0000U) == 0x0b20'0000U) {
      if (!valid_extended_register(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_add_sub_extended(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1fe0'fc00U) == 0x1a00'0000U) {
      emit_add_sub_carry(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1200'0000U) {
      if (!valid_logical_immediate(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_logical_immediate(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1300'0000U) {
      if (!valid_bitfield(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_bitfield(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1380'0000U) {
      const bool wide = (instruction & 0x8000'0000U) != 0;
      if (((instruction >> 29) & 3U) != 0U ||
          (instruction & 0x0020'0000U) != 0U ||
          ((instruction >> 22) & 1U) != static_cast<unsigned>(wide) ||
          (!wide && ((instruction >> 10) & 63U) >= 32U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_native_binary(instruction, false, false);
      return Action::keep_going;
    }
    if ((instruction & 0x1f00'0000U) == 0x0a00'0000U) {
      if (!valid_shifted_register(instruction, false)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_logical_shifted(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x3fe0'0000U) == 0x1a80'0000U) {
      if ((instruction & 0x0000'0800U) != 0) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_conditional_select(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x3fe0'0000U) == 0x3a40'0000U) {
      if ((instruction & 0x0000'0410U) != 0) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_conditional_compare(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f00'0000U) == 0x1b00'0000U) {
      if ((instruction & 0xffe0'8000U) == 0x9b60'0000U ||
          (instruction & 0xffe0'8000U) == 0x9b60'8000U)
        return Action::unsupported;
      if (!valid_multiply_add(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_multiply_add(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x7fe0'e000U) == 0x1ac0'4000U) {
      const unsigned size = (instruction >> 10) & 3U;
      if (((instruction >> 31) != 0) != (size == 3U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (emit_crc32(instruction))
        return Action::keep_going;
      return Action::unsupported;
    }
    if ((instruction & 0x7fe0'fc00U) == 0x1ac0'2000U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'2400U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'2800U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'2c00U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'0800U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'0c00U) {
      emit_native_binary(instruction, false, false);
      return Action::keep_going;
    }
    if ((instruction & 0x7fe0'f000U) == 0x1ac0'6000U) {
      if (!host_arm_features().cssc)
        return Action::unsupported;
      emit_integer_min_max(instruction, false);
      return Action::keep_going;
    }
    if ((instruction & 0x5fe0'f000U) == 0x1ac0'6000U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffff'c000U) == 0xdac1'0000U) {
      if (!enable_pointer_authentication_) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      const unsigned operation = (instruction >> 10) & 15U;
      const unsigned modifier = (instruction >> 5) & 31U;
      if (operation >= 8U && modifier != 31U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if ((instruction & 31U) == 31U)
        return Action::keep_going;
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffff'f800U) == 0xdac1'4000U) {
      if (!enable_pointer_authentication_) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (((instruction >> 5) & 31U) != 31U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if ((instruction & 31U) == 31U)
        return Action::keep_going;
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffe0'fc00U) == 0x9ac0'3000U) {
      if (!enable_pointer_authentication_) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if ((instruction & 31U) == 31U)
        return Action::keep_going;
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0x5fe0'0000U) == 0x1ac0'0000U) {
      const bool wide = (instruction & 0x8000'0000U) != 0;
      const bool s = (instruction & 0x2000'0000U) != 0;
      const unsigned opcode = (instruction >> 10) & 63U;
      bool allocated = false;
      if (opcode < 32U) {
        if (s) {
          allocated = wide && opcode == 0U;
        } else if (!wide) {
          allocated = opcode == 2U || opcode == 3U ||
                      (opcode >= 8U && opcode <= 11U) ||
                      (opcode >= 16U && opcode <= 18U) ||
                      (opcode >= 20U && opcode <= 22U) ||
                      (opcode >= 24U && opcode <= 27U);
        } else {
          allocated = opcode == 0U || opcode == 2U || opcode == 3U ||
                      opcode == 4U || opcode == 5U ||
                      (opcode >= 8U && opcode <= 12U) ||
                      opcode == 19U || opcode == 23U ||
                      (opcode >= 24U && opcode <= 27U);
        }
      }
      if (allocated)
        return Action::unsupported;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0x7fff'8000U) == 0x5ac0'0000U) {
      if (!valid_bit_reverse_count(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_native_unary(instruction);
      return Action::keep_going;
    }
    /*
     * Decode the exact conversion allocations before the broader SIMD/FP
     * classes below. Several conversion opcodes deliberately reuse fields
     * that those legacy recognizers inspect, including the integer-reduction
     * class.
     */
    if ((instruction & 0xffff'fc00U) == 0x1e7e'0000U) {
      if (!emit_floating_javascript_convert(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto floating_to_integer =
            arm64::detail::decode_floating_to_integer(instruction);
        floating_to_integer.recognized) {
      if (!floating_to_integer.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_floating_to_integer(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto fp8 = arm64::detail::decode_fp8(instruction);
        fp8.recognized &&
        (fp8.operation == arm64::detail::Fp8Operation::convert_to_half ||
         fp8.operation == arm64::detail::Fp8Operation::convert_to_bfloat ||
         fp8.operation == arm64::detail::Fp8Operation::convert_from_half ||
         fp8.operation == arm64::detail::Fp8Operation::convert_from_single)) {
      if (!fp8.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      const bool converted_up =
          fp8.operation == arm64::detail::Fp8Operation::convert_to_half ||
          fp8.operation == arm64::detail::Fp8Operation::convert_to_bfloat;
      if (!(converted_up ? emit_fp8_convert_up(instruction)
                         : emit_fp8_convert_down(instruction)))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto fp8 = arm64::detail::decode_fp8(instruction);
        fp8.recognized &&
        (fp8.operation == arm64::detail::Fp8Operation::dot_half ||
         fp8.operation == arm64::detail::Fp8Operation::dot_single)) {
      if (!fp8.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_fp8_dot(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto fp8 = arm64::detail::decode_fp8(instruction);
        fp8.recognized &&
        (fp8.operation == arm64::detail::Fp8Operation::multiply_half ||
         fp8.operation == arm64::detail::Fp8Operation::multiply_single ||
         fp8.operation == arm64::detail::Fp8Operation::matrix_half ||
         fp8.operation == arm64::detail::Fp8Operation::matrix_single)) {
      if (!fp8.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_fp8_multiply_matrix(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto complex =
            arm64::detail::decode_floating_complex(instruction);
        complex.recognized) {
      if (!complex.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_floating_complex(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const unsigned simd_integer_reduce_opcode =
        (instruction >> 12) & 31U;
    const bool simd_integer_reduce =
        (instruction & 0x9f30'0c00U) == 0x0e30'0800U &&
        (simd_integer_reduce_opcode == 3U ||
         simd_integer_reduce_opcode == 10U ||
         simd_integer_reduce_opcode == 26U ||
         simd_integer_reduce_opcode == 27U);
    if (simd_integer_reduce) {
      const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
      const bool q = (instruction & 0x4000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      if (size > 2U || (!q && size == 2U) ||
          (simd_integer_reduce_opcode == 27U && unsigned_group)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_arm_features().advanced_simd ||
          !emit_simd_integer_reduce(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if ((instruction & 0xff20'fc00U) == 0x1e20'2000U) {
      const unsigned type = (instruction >> 22) & 3U;
      const bool compare_zero = (instruction & 8U) != 0U;
      if (type == 2U || (instruction & 7U) != 0U ||
          (compare_zero && ((instruction >> 16) & 31U) != 0U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_compare(instruction, false);
      return Action::keep_going;
    }
    if ((instruction & 0xff20'0c00U) == 0x1e20'0400U) {
      if (((instruction >> 22) & 3U) == 2U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_compare(instruction, true);
      return Action::keep_going;
    }
    if ((instruction & 0xff20'0c00U) == 0x1e20'0c00U) {
      if (((instruction >> 22) & 3U) == 2U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_select(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0xff20'1fe0U) == 0x1e20'1000U) {
      if (((instruction >> 22) & 3U) == 2U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_move(instruction, true);
      return Action::keep_going;
    }
    if ((instruction & 0xff3f'fc00U) == 0x1e20'4000U) {
      if (((instruction >> 22) & 3U) == 2U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_move(instruction, false);
      return Action::keep_going;
    }
    if (const auto floating_compare =
            arm64::detail::decode_simd_floating_compare(instruction);
        floating_compare.recognized) {
      if (!floating_compare.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_floating_compare(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const bool scalar_precision_allocation =
        (instruction & 0xff20'7c00U) == 0x1e20'4000U &&
        ((instruction >> 15) & 0x3fU) >= 4U &&
        ((instruction >> 15) & 0x3fU) <= 7U;
    if (scalar_precision_allocation) {
      const unsigned source_type = (instruction >> 22) & 3U;
      const unsigned destination_type = (instruction >> 15) & 3U;
      /* The source=01/destination=10 cell belongs to the separate
       * reduced-precision conversion allocation. All other invalid type
       * pairs are reserved scalar FCVT encodings and must terminate as an
       * architectural illegal instruction, even though no native operation
       * can be emitted for them. */
      const bool separate_allocation =
          source_type == 1U && destination_type == 2U;
      if (!separate_allocation &&
          (source_type == 2U || destination_type == 2U ||
           source_type == destination_type)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!separate_allocation) {
        if (!emit_floating_precision_convert(instruction))
          return Action::unsupported;
        return Action::keep_going;
      }
    }
    if (emit_floating_precision_convert(instruction))
      return Action::keep_going;
    if (const auto floating_round =
            arm64::detail::decode_floating_round(instruction);
        floating_round.recognized) {
      if (!floating_round.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_floating_round(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const auto scalar_sign_operation = instruction & 0xff3f'fc00U;
    const auto half_sign_operation = instruction & 0xbfff'fc00U;
    const auto vector_sign_operation = instruction & 0xbf3f'fc00U;
    const bool scalar_sign =
        scalar_sign_operation == 0x1e20'c000U ||
        scalar_sign_operation == 0x1e21'4000U;
    const bool half_sign =
        half_sign_operation == 0x0ef8'f800U ||
        half_sign_operation == 0x2ef8'f800U;
    const bool vector_sign =
        vector_sign_operation == 0x0e20'f800U ||
        vector_sign_operation == 0x2e20'f800U;
    if (scalar_sign || half_sign || vector_sign) {
      const unsigned type = (instruction >> 22) & 3U;
      const bool q = (instruction & 0x4000'0000U) != 0;
      const bool valid =
          scalar_sign ? type != 2U
                      : half_sign ||
                            ((instruction & 0x0080'0000U) != 0 &&
                             ((instruction & 0x0040'0000U) == 0 || q));
      if (!valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_floating_unary(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (emit_floating_unary(instruction))
      return Action::keep_going;
    if (const auto floating_horizontal =
            arm64::detail::decode_floating_horizontal(instruction);
        floating_horizontal.recognized) {
      if (!floating_horizontal.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_floating_horizontal(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto floating_reciprocal =
            arm64::detail::decode_floating_reciprocal(instruction);
        floating_reciprocal.recognized) {
      if (!floating_reciprocal.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_floating_reciprocal(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto floating_scale =
            arm64::detail::decode_floating_scale(instruction);
        floating_scale.recognized) {
      if (!floating_scale.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_floating_scale(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto integer_to_floating =
            arm64::detail::decode_integer_to_floating(instruction);
        integer_to_floating.recognized) {
      if (!integer_to_floating.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_integer_to_floating(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (arm64::detail::decode_half_dot(instruction).recognized) {
      if (!emit_half_dot(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const bool half_dot_scalar_hole =
        arm64::detail::reserved_half_dot_scalar_class(instruction);
    if (half_dot_scalar_hole) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (arm64::detail::decode_half_fused_long(instruction).recognized) {
      if (!emit_half_fused_long(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto half_matrix =
            arm64::detail::decode_half_matrix(instruction);
        half_matrix.recognized) {
      if (!half_matrix.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_half_matrix(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (arm64::detail::reserved_half_fused_long(instruction)) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction,
                               resulting_count);
      return Action::terminate;
    }
    if (emit_simd_integer_float_convert(instruction))
      return Action::keep_going;
    if (const auto fused_floating =
            arm64::detail::decode_fused_floating_arithmetic(instruction);
        fused_floating.recognized) {
      if (!fused_floating.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_fused_floating(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto basic_floating =
            arm64::detail::decode_basic_floating_arithmetic(instruction);
        basic_floating.recognized) {
      if (!basic_floating.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!emit_basic_floating(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (arm64::detail::decode_bfloat_dot(instruction).recognized) {
      if (!emit_bfloat_dot(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (arm64::detail::decode_bfloat_matrix_multiply(instruction)) {
      if (!emit_bfloat_matrix(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (arm64::detail::decode_bfloat_multiply_long(instruction)
            .recognized) {
      if (!emit_bfloat_multiply_long(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    /*
     * Resolve the unallocated cells adjacent to the reduced-precision
     * vector operations before the broader SIMD classes can treat them as
     * unsupported. Q, register fields, and the allocated arrangements have
     * already been consumed by the exact decoders above.
     */
    if ((instruction & 0xbf20'fc00U) == 0x2e00'fc00U) {
      const unsigned size = (instruction >> 22) & 3U;
      if (size == 0U || size == 2U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
    }
    if ((instruction & 0xbf20'fc00U) == 0x2e00'ec00U) {
      const bool q = (instruction & 0x4000'0000U) != 0U;
      const unsigned size = (instruction >> 22) & 3U;
      if ((!q && size == 1U) || (q && size == 3U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
    }
    if ((instruction & 0xbfe0'8400U) == 0x2e00'0000U) {
      const bool q = (instruction & 0x4000'0000U) != 0;
      const unsigned offset = (instruction >> 11) & 15U;
      if (!q && offset >= 8U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_arm_features().advanced_simd ||
          !emit_simd_extract(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if ((instruction & 0xff20'8c00U) == 0x4e00'0000U &&
        (instruction & 0x00c0'0000U) != 0U) {
      const unsigned op2 = (instruction >> 22) & 3U;
      const unsigned len = (instruction >> 13) & 3U;
      const unsigned op = (instruction >> 12) & 1U;
      const bool valid =
          op2 == 3U ||
          (op2 == 2U && op != 0U) ||
          (op2 == 1U && (op != 0U || (len & 1U) != 0U));
      if (!valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_lut(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (emit_simd_table_lookup(instruction))
      return Action::keep_going;
    const auto simd_permute_operation = instruction & 0xbf20'fc00U;
    const bool simd_permute = simd_permute_operation == 0x0e00'1800U ||
                              simd_permute_operation == 0x0e00'2800U ||
                              simd_permute_operation == 0x0e00'3800U ||
                              simd_permute_operation == 0x0e00'5800U ||
                              simd_permute_operation == 0x0e00'6800U ||
                              simd_permute_operation == 0x0e00'7800U;
    if (simd_permute && (instruction & 0x4000'0000U) == 0 &&
        ((instruction >> 22) & 3U) == 3U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_simd_permute(instruction))
      return Action::keep_going;
    const bool simd_aes =
        (instruction & 0xffff'8c00U) == 0x4e28'0800U;
    if (simd_aes && ((instruction >> 12) & 7U) < 4U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_simd_aes(instruction))
      return Action::keep_going;
    const bool simd_pmull =
        (instruction & 0xbf20'fc00U) == 0x0e20'e000U;
    const unsigned simd_pmull_size = (instruction >> 22) & 3U;
    if (simd_pmull && simd_pmull_size != 0U && simd_pmull_size != 3U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_simd_pmull(instruction))
      return Action::keep_going;
    const bool simd_sha12_three =
        (instruction & 0xffe0'8c00U) == 0x5e00'0000U;
    const bool simd_sha12_two =
        (instruction & 0xffff'8c00U) == 0x5e28'0800U;
    if ((simd_sha12_three && ((instruction >> 12) & 7U) == 7U) ||
        (simd_sha12_two && ((instruction >> 12) & 3U) == 3U)) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_simd_sha1_sha2(instruction))
      return Action::keep_going;
    if (emit_simd_sha512(instruction))
      return Action::keep_going;
    if (emit_simd_sm(instruction))
      return Action::keep_going;
    const bool reserved_simd_logic =
        (instruction & 0xffe0'fc00U) == 0xce60'cc00U ||
        (instruction & 0xffe0'8000U) == 0xce60'0000U ||
        (instruction & 0xffff'fc00U) == 0xcec0'8800U ||
        (instruction & 0xffff'fc00U) == 0xcec0'8c00U;
    if (reserved_simd_logic) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_simd_sha3(instruction))
      return Action::keep_going;
    const auto simd_high_narrow_class = instruction & 0x9f20'fc00U;
    const bool simd_high_narrow =
        simd_high_narrow_class == 0x0e20'4000U ||
        simd_high_narrow_class == 0x0e20'6000U;
    if (simd_high_narrow && ((instruction >> 22) & 3U) == 3U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_simd_arithmetic_narrow(instruction))
      return Action::keep_going;
    const bool simd_widening_vector =
        (instruction & 0x9f20'0c00U) == 0x0e20'0000U;
    const bool simd_widening_scalar =
        (instruction & 0xdf20'0c00U) == 0x5e20'0000U;
    const unsigned simd_widening_operation = (instruction >> 12) & 15U;
    const bool simd_widening =
        (simd_widening_vector || simd_widening_scalar) &&
        simd_widening_operation != 4U &&
        simd_widening_operation != 6U &&
        simd_widening_operation != 14U;
    if (simd_widening) {
      const bool unsigned_values = (instruction & 0x2000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      const bool saturating = simd_widening_operation == 9U ||
                              simd_widening_operation == 11U ||
                              simd_widening_operation == 13U;
      const bool valid =
          size < 3U &&
          (simd_widening_scalar
               ? saturating && !unsigned_values && size != 0U
               : saturating
                     ? !unsigned_values && size != 0U
                     : simd_widening_operation <= 3U ||
                           simd_widening_operation == 5U ||
                           simd_widening_operation == 7U ||
                           simd_widening_operation == 8U ||
                           simd_widening_operation == 10U ||
                           simd_widening_operation == 12U);
      if (!valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
    }
    if (emit_simd_widening(instruction))
      return Action::keep_going;
    if (emit_simd_narrow_move(instruction))
      return Action::keep_going;
    const bool simd_two_register_vector =
        (instruction & 0x9f20'0c00U) == 0x0e20'0800U;
    const bool simd_two_register_scalar =
        (instruction & 0xdf20'0c00U) == 0x5e20'0800U;
    const unsigned simd_two_register_opcode = (instruction >> 12) & 31U;
    const bool simd_two_register_integer =
        (simd_two_register_vector || simd_two_register_scalar) &&
        (simd_two_register_opcode == 2U ||
         simd_two_register_opcode == 3U ||
         simd_two_register_opcode == 4U ||
         simd_two_register_opcode == 6U ||
         simd_two_register_opcode == 7U ||
         simd_two_register_opcode == 11U ||
         simd_two_register_opcode == 18U ||
         simd_two_register_opcode == 19U ||
         simd_two_register_opcode == 20U);
    if (simd_two_register_integer) {
      const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
      const bool q = (instruction & 0x4000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      bool valid = false;
      switch (simd_two_register_opcode) {
      case 2:
      case 4:
      case 6:
        valid = simd_two_register_vector && size < 3U;
        break;
      case 3:
      case 7:
        valid = simd_two_register_scalar || q || size < 3U;
        break;
      case 11:
        valid =
            simd_two_register_scalar ? size == 3U : q || size < 3U;
        break;
      case 18:
        valid = size < 3U &&
                (unsigned_group || simd_two_register_vector);
        break;
      case 19:
        valid = size < 3U && unsigned_group &&
                simd_two_register_vector;
        break;
      case 20:
        valid = size < 3U;
        break;
      }
      if (!valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_arm_features().advanced_simd ||
          !emit_simd_two_register_integer(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (simd_two_register_scalar &&
        (simd_two_register_opcode == 0U ||
         simd_two_register_opcode == 1U ||
         simd_two_register_opcode == 5U)) {
      /* Reverse and byte-unary operations have no scalar allocation. */
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0x9fbf'fc00U) == 0x0ea1'c800U) {
      if ((instruction & 0x0040'0000U) != 0) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_arm_features().advanced_simd ||
          !emit_simd_unsigned_estimate(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_fmulx(instruction)) {
      if (!valid_simd_fmulx(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_fmulx(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_dot(instruction)) {
      if (!valid_simd_dot(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_dot(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_matrix(instruction)) {
      if (!valid_simd_matrix(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_matrix(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_floating_indexed(instruction)) {
      if (!valid_simd_floating_indexed(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_floating_indexed(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_integer_indexed(instruction)) {
      if (!valid_simd_integer_indexed(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_integer_indexed(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_rdm(instruction)) {
      if (!valid_simd_rdm(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_rdm(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if ((instruction & 0x9f20'fc00U) == 0x0e20'1c00U) {
      if (!emit_simd_bitwise_register(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const bool simd_three_same_vector =
        (instruction & 0x9f20'0400U) == 0x0e20'0400U;
    const bool simd_three_same_scalar =
        (instruction & 0xdf20'0400U) == 0x5e20'0400U;
    const unsigned simd_three_same_opcode = (instruction >> 11) & 31U;
    if ((simd_three_same_vector || simd_three_same_scalar) &&
        simd_three_same_opcode < 24U) {
      if (!valid_simd_three_same_integer(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_arm_features().advanced_simd ||
          !emit_simd_three_same_integer(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if ((instruction & 0x9ff8'0400U) == 0x0f00'0400U) {
      const auto decoded =
          arm64::detail::decode_simd_modified_immediate(instruction);
      if (!decoded) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_arm_features().advanced_simd)
        return Action::unsupported;
      emit_simd_modified_immediate(instruction, decoded->action != 0U);
      return Action::keep_going;
    }
    if ((instruction & 0xbfe0'fc00U) == 0x0e00'0c00U) {
      const auto lane =
          arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U);
      const bool q = (instruction & 0x4000'0000U) != 0;
      if (!lane || (!q && lane->element_shift == 3U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_arm_features().advanced_simd)
        return Action::unsupported;
      emit_simd_duplicate_general(instruction);
      return Action::keep_going;
    }
    const bool simd_insert_general =
        (instruction & 0xffe0'fc00U) == 0x4e00'1c00U;
    const bool simd_insert_element =
        (instruction & 0xffe0'8400U) == 0x6e00'0400U;
    if (simd_insert_general || simd_insert_element) {
      if (!arm64::detail::decode_simd_lane_index(
              (instruction >> 16) & 31U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_arm_features().advanced_simd ||
          !emit_simd_insert(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const bool simd_duplicate_element_vector =
        (instruction & 0xbf20'fc00U) == 0x0e00'0400U;
    const bool simd_duplicate_element_scalar =
        (instruction & 0xff20'fc00U) == 0x5e00'0400U;
    if (simd_duplicate_element_vector || simd_duplicate_element_scalar) {
      const auto lane =
          arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U);
      const bool q = (instruction & 0x4000'0000U) != 0;
      if (!lane ||
          (simd_duplicate_element_vector && !q &&
           lane->element_shift == 3U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_arm_features().advanced_simd ||
          !emit_simd_duplicate_element(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const auto simd_bit_unary_operation = instruction & 0x9f20'fc00U;
    const bool simd_bit_unary =
        simd_bit_unary_operation == 0x0e20'0800U ||
        simd_bit_unary_operation == 0x0e20'1800U ||
        simd_bit_unary_operation == 0x0e20'5800U;
    if (simd_bit_unary) {
      const bool alternate = (instruction & 0x2000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      const bool valid =
          simd_bit_unary_operation == 0x0e20'0800U
              ? (alternate ? size < 2U : size < 3U)
          : simd_bit_unary_operation == 0x0e20'1800U
              ? !alternate && size == 0U
              : alternate ? size <= 1U : size == 0U;
      if (!valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_arm_features().advanced_simd ||
          !emit_simd_bit_unary(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (emit_simd_scalar_pairwise_add(instruction))
      return Action::keep_going;
    if ((instruction & 0x9f80'fc00U) == 0x0f00'a400U) {
      const unsigned encoded = (instruction >> 16) & 0x7fU;
      const unsigned source_bits = std::bit_floor(encoded);
      if (source_bits < 8U || source_bits > 32U ||
          encoded - source_bits >= source_bits) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_arm_features().advanced_simd ||
          !emit_simd_shift_left_long(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (emit_simd_shift_immediate(instruction))
      return Action::keep_going;
    const bool vector_shift_immediate =
        (instruction & 0x9f00'0400U) == 0x0f00'0400U &&
        ((instruction >> 16) & 0x7fU) >= 8U;
    const bool scalar_shift_immediate =
        (instruction & 0xdf00'0400U) == 0x5f00'0400U;
    const unsigned shift_opcode = (instruction >> 11) & 31U;
    const bool reserved_shift_opcode =
        shift_opcode < 20U ||
        (shift_opcode >= 21U && shift_opcode <= 27U) ||
        shift_opcode == 29U || shift_opcode == 30U ||
        (scalar_shift_immediate && shift_opcode == 20U);
    if ((vector_shift_immediate || scalar_shift_immediate) &&
        reserved_shift_opcode) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    const auto simd_compare_zero_class = instruction & 0x9f3f'fc00U;
    if (simd_compare_zero_class == 0x0e20'8800U ||
        simd_compare_zero_class == 0x0e20'9800U ||
        simd_compare_zero_class == 0x0e20'a800U ||
        simd_compare_zero_class == 0x1e20'8800U ||
        simd_compare_zero_class == 0x1e20'9800U ||
        simd_compare_zero_class == 0x1e20'a800U) {
      const unsigned opcode = (instruction >> 12) & 31U;
      const bool alternate = (instruction & 0x2000'0000U) != 0;
      const bool scalar = (instruction & 0x1000'0000U) != 0;
      const bool q = (instruction & 0x4000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      const bool valid_opcode =
          opcode == 8U || opcode == 9U || (opcode == 10U && !alternate);
      const bool valid_shape =
          (!scalar || size == 3U) && (scalar || q || size != 3U);
      if (!valid_opcode || !valid_shape || !host_arm_features().advanced_simd) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_simd_unary_native(instruction);
      return Action::keep_going;
    }
    const auto simd_general_opcode = instruction & 0xffff'fc00U;
    const bool simd_general_class =
        (instruction & 0xbfe0'fc00U) == 0x0e00'3c00U ||
        (instruction & 0xbfe0'fc00U) == 0x0e00'2c00U ||
        simd_general_opcode == 0x1ee6'0000U ||
        simd_general_opcode == 0x9ee6'0000U ||
        simd_general_opcode == 0x1ee7'0000U ||
        simd_general_opcode == 0x9ee7'0000U ||
        simd_general_opcode == 0x9e66'0000U ||
        simd_general_opcode == 0x9e67'0000U ||
        simd_general_opcode == 0x1e26'0000U ||
        simd_general_opcode == 0x1e27'0000U ||
        simd_general_opcode == 0x9eae'0000U ||
        simd_general_opcode == 0x9eaf'0000U;
    if (simd_general_class) {
      if (!host_arm_features().advanced_simd ||
          !emit_simd_general_move(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      return Action::keep_going;
    }
    /* FEAT_MTE STGP occupies the GPR opc=01 store allocation. It is a
     * distinct instruction family and must not be diagnosed as an
     * unallocated ordinary pair.
     */
    if ((instruction & 0xffc0'0000U) == 0x6880'0000U ||
        (instruction & 0xffc0'0000U) == 0x6900'0000U ||
        (instruction & 0xffc0'0000U) == 0x6980'0000U)
      return Action::unsupported;
    if ((instruction & 0xff20'0400U) == 0xf820'0400U) {
      if (!enable_pointer_authentication_) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      const unsigned destination = instruction & 31U;
      const unsigned base = (instruction >> 5) & 31U;
      const bool writeback = (instruction & 0x800U) != 0;
      if (writeback && base == destination && base != 31U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    /*
     * LS64 overlaps the broader atomic-memory class. Resolve its complete
     * allocation and exact native feature at translation time. The semantic
     * cold path handles alignment faults and active allocation-tag checking;
     * valid hot execution uses a translated host address and a native 64-byte
     * transaction without a gadget fallback. Unsupported hosts and reserved
     * Rt partitions terminate as undefined before observing an address.
     */
    const bool ls64_load =
        (instruction & 0xffff'fc00U) == 0xf83f'd000U;
    const bool ls64_store =
        (instruction & 0xffff'fc00U) == 0xf83f'9000U;
    const auto ls64_status_opcode = instruction & 0xffe0'fc00U;
    const bool ls64_status = ls64_status_opcode == 0xf820'b000U;
    const bool ls64_status_accdata =
        ls64_status_opcode == 0xf820'a000U;
    if (ls64_load || ls64_store || ls64_status || ls64_status_accdata) {
      const unsigned first = instruction & 31U;
      const auto required =
          ls64_status_accdata ? ArmFeature::ls64_accdata
          : ls64_status      ? ArmFeature::ls64_v
                             : ArmFeature::ls64;
      if ((first & 1U) != 0U || first >= 24U ||
          !host_arm_features().has(required)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_ls64(instruction, pc, ls64_load, ls64_status,
                ls64_status_accdata);
      return Action::keep_going;
    }
    /*
     * MOPS shares major encodings with atomic memory instructions. Decode its
     * complete allocation first. Translation resolves every legality choice;
     * valid instructions take the checked memory bridge only on hosts that
     * advertise FEAT_MOPS, while every reserved encoding terminates here.
     */
    if ((instruction & 0x3b20'0c00U) == 0x1900'0400U) {
      const unsigned size = instruction >> 30;
      const unsigned op1 = (instruction >> 22) & 3U;
      const unsigned op2 = (instruction >> 12) & 15U;
      const unsigned destination = instruction & 31U;
      const unsigned count = (instruction >> 5) & 31U;
      const unsigned source = (instruction >> 16) & 31U;
      const bool set = op1 == 3U;
      const bool set_allocation_tags =
          set && (instruction & 0x0400'0000U) != 0U;
      const bool allocated = size == 0U && (!set || op2 < 12U);
      const bool valid_registers =
          destination != 31U && count != 31U && destination != count &&
          source != destination && source != count && (set || source != 31U);
      if (!allocated || !valid_registers ||
          !host_arm_features().has(ArmFeature::mops) ||
          (set_allocation_tags &&
           !host_arm_features().has(ArmFeature::mte))) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      const unsigned stage = set ? op2 >> 2 : op1;
      if (stage == 2U)
        mark_memory_write();
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    if (const auto atomic_floating =
            arm64::detail::decode_atomic_floating(instruction);
        atomic_floating.recognized) {
      if (!atomic_floating.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_arm_features().has(ArmFeature::lsfe))
        return Action::unsupported;
      emit_atomic_floating_memory(instruction, pc, atomic_floating);
      return Action::keep_going;
    }
    /*
     * Read-check-write shares the scalar atomic and 128-bit pair major
     * encodings. Resolve the complete allocation before those broader
     * decoders. Valid words use the existing one-instruction semantic bridge,
     * which preserves exact guest-memory fault metadata; unsupported native
     * features and reserved register/operation cells terminate as undefined.
     */
    const bool rcw_scalar_compare =
        (instruction & 0xbf20'fc00U) == 0x1920'0800U;
    const bool rcw_scalar_modify =
        (instruction & 0xbf20'8c00U) == 0x3820'8000U &&
        ((instruction >> 12) & 7U) >= 1U &&
        ((instruction >> 12) & 7U) <= 3U;
    const bool rcw_pair_compare =
        (instruction & 0xbf20'fc00U) == 0x1920'0c00U;
    const bool rcw_pair_allocation =
        (instruction & 0xbf20'8c00U) == 0x1920'8000U &&
        (((instruction >> 12) & 7U) != 0U ||
         (instruction & 0x4000'0000U) != 0U);
    if (rcw_scalar_compare || rcw_scalar_modify || rcw_pair_compare ||
        rcw_pair_allocation) {
      const bool pair = rcw_pair_compare || rcw_pair_allocation;
      const unsigned first = instruction & 31U;
      const unsigned second = (instruction >> 16) & 31U;
      const unsigned operation = (instruction >> 12) & 7U;
      const bool valid =
          pair
              ? (rcw_pair_compare
                     ? (first & 1U) == 0U && (second & 1U) == 0U
                     : operation >= 1U && operation <= 3U && first != 31U &&
                           second != 31U && first != second)
              : true;
      const bool feature =
          pair ? host_arm_features().has(ArmFeature::d128) &&
                     host_arm_features().has(ArmFeature::the)
               : host_arm_features().has(ArmFeature::the);
      if (!valid || !feature) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      mark_memory_write();
      const auto slow = emitter_.make_label();
      cold_.push_back(
          {slow, instruction, pc, count_ + 1,
           ColdPath::Action::exclusive_slow});
      emitter_.branch(slow);
      /*
       * The helper always returns to the dispatcher.  Ending the translated
       * block here also prevents an unreachable later store from publishing
       * this load-reservation block as a store in flight, which can otherwise
       * make concurrent reservation loops wait on one another forever.
       */
      return Action::terminate;
    }
    /*
     * FEAT_LSE128 shares the pair RCW major allocation. RCW cells above have
     * already been consumed, leaving the clear/set/swap cells and their
     * reserved S/opcode neighbours. Resolve feature and register legality
     * here so an invalid word never becomes an unsupported runtime event.
     * The semantic memory bridge preserves one-copy atomicity and exact
     * guest fault metadata while remaining independent of the gadget engine.
     */
    if ((instruction & 0xbf20'0c00U) == 0x1920'0000U) {
      const bool software = (instruction & 0x4000'0000U) != 0U;
      const unsigned first = instruction & 31U;
      const unsigned second = (instruction >> 16) & 31U;
      const unsigned o3 = (instruction >> 15) & 1U;
      const unsigned operation = (instruction >> 12) & 7U;
      const bool valid_operation =
          !software &&
          ((!o3 && (operation == 1U || operation == 3U)) ||
           (o3 && operation == 0U));
      const bool valid_registers =
          first != 31U && second != 31U && first != second;
      if (!valid_operation || !valid_registers ||
          !host_arm_features().has(ArmFeature::lse128)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      mark_memory_write();
      const auto slow = emitter_.make_label();
      cold_.push_back(
          {slow, instruction, pc, count_ + 1,
           ColdPath::Action::exclusive_slow});
      emitter_.branch(slow);
      return Action::terminate;
    }
    if ((instruction & 0x3f20'0c00U) == 0x3820'0000U ||
        (instruction & 0x3f20'0c00U) == 0x1920'0400U) {
      const auto action = emit_atomic_memory(instruction, pc);
      if (action != Action::unsupported)
        return action;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0x3f00'0000U) == 0x0800'0000U ||
        (instruction & 0x3f00'0000U) == 0x0900'0000U) {
      const auto action = emit_exclusive_memory(instruction, pc);
      if (action != Action::unsupported)
        return action;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0x3a00'0000U) == 0x2800'0000U) {
      const bool vector = (instruction & 0x0400'0000U) != 0;
      const bool load = (instruction & 0x0040'0000U) != 0;
      const unsigned operation = instruction >> 30;
      const unsigned mode = (instruction >> 23) & 3U;
      const unsigned first = instruction & 31U;
      const unsigned second = (instruction >> 10) & 31U;
      const bool valid =
          (vector || (operation != 1U || (load && mode != 0U))) &&
          (!load || first != second);
      if (!valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!vector && operation == 3U &&
          !host_arm_features().has(ArmFeature::lsui)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (emit_pair_memory(instruction, pc))
        return Action::keep_going;
      return Action::unsupported;
    }
    if ((instruction & 0xbf20'0000U) == 0x0c00'0000U) {
      if (emit_simd_structure_memory(instruction, pc, false))
        return Action::keep_going;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xbf80'0000U) == 0x0d00'0000U ||
        (instruction & 0xbf80'0000U) == 0x0d80'0000U) {
      if (emit_simd_structure_memory(instruction, pc, true))
        return Action::keep_going;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_literal_memory(instruction, pc))
      return Action::keep_going;
    if ((instruction & 0x3b00'0000U) == 0x1800'0000U) {
      /*
       * The only allocation rejected by emit_literal_memory after the
       * host-feature gate is SIMD&FP opc=11. Resolve that reserved quadrant
       * while translating so it cannot become an unsupported runtime event.
       */
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0x3b00'0000U) == 0x3800'0000U) {
      if (emit_other_memory(instruction, pc))
        return Action::keep_going;
      const bool vector = (instruction & 0x0400'0000U) != 0U;
      const unsigned size_shift = instruction >> 30;
      const unsigned opc = (instruction >> 22) & 3U;
      const bool register_offset = (instruction & 0x0020'0000U) != 0U;
      const unsigned mode = (instruction >> 10) & 3U;
      const unsigned option = (instruction >> 13) & 7U;
      const bool invalid_vector =
          vector &&
          ((opc >= 2U && size_shift != 0U) ||
           (register_offset && (mode != 2U || (option & 2U) == 0U)) ||
           (!register_offset && mode == 2U));
      if (!vector || invalid_vector) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      return Action::unsupported;
    }
    if ((instruction & 0x3b00'0000U) == 0x3900'0000U) {
      if (emit_unsigned_memory(instruction, pc))
        return Action::keep_going;
      const bool vector = (instruction & 0x0400'0000U) != 0U;
      const unsigned size_shift = instruction >> 30;
      const unsigned opc = (instruction >> 22) & 3U;
      if (!vector || (opc >= 2U && size_shift != 0U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      return Action::unsupported;
    }
    return Action::unsupported;
  }

  Arm64Emitter emitter_;
  ColdPaths cold_;
  std::vector<TranslationColdPath> translation_cold_;
  std::size_t count_{};
  std::uint64_t memory_write_mask_{};
  std::uint64_t direct_memory_write_mask_{};
  bool terminated_{};
  bool accesses_fp_state_{};
  bool link_target_safe_{true};
  bool native_fp_{};
  bool native_fpmr_{};
  bool guest_illegal_{};
  void *configured_link_context_{};
  LinkResolver configured_link_resolver_{};
  IndirectLinkResolver configured_indirect_link_resolver_{};
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  void *configured_gpr_link_profile_context_{};
  DirectLinkProfileResolver configured_direct_profile_resolver_{};
  IndirectLinkProfileResolver configured_indirect_profile_resolver_{};
#endif
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  bool configured_profile_gpr_pressure_{true};
#endif
  bool configured_persistent_linked_block_abi_{
      persistent_linked_block_abi_default};
  bool track_direct_stores_{};
  bool enable_guarded_control_flow_{};
  bool enable_pointer_authentication_{true};
  bool gpr_residency_planning_enabled_{true};
  bool table_list_planning_enabled_{true};
  const ArmFeatureSet *translation_features_{};
  std::vector<std::uint32_t> selected_words_;
};

using JitFunction = std::uint64_t (*)(CpuState *, Tlb *, JitChainState *);

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
  operator()(const JitGprProfileEdgeKey &key) const noexcept {
    auto hash = static_cast<std::size_t>(
        key.source_pc ^ (key.source_pc >> 32U));
    const auto mix = [&](std::uint64_t value) {
      hash ^= static_cast<std::size_t>(value ^ (value >> 32U)) +
              0x9e37'79b9U + (hash << 6U) + (hash >> 2U);
    };
    mix(key.target_pc);
    mix((static_cast<std::uint64_t>(key.dirty_live_out) << 32U) |
        key.read_before_write);
    mix(key.indirect);
    return hash;
  }
};

void add_gpr_link_profile_totals(
    JitGprLinkProfileDiagnostics &destination,
    const JitGprLinkProfileDiagnostics &source) noexcept {
  destination.successful_links += source.successful_links;
  destination.unattributed_links += source.unattributed_links;
  destination.outer_invocations += source.outer_invocations;
  for (std::size_t reg = 0; reg < destination.live_in_saves.size(); ++reg) {
    destination.live_in_saves[reg] += source.live_in_saves[reg];
    destination.dirty_out_saves[reg] += source.dirty_out_saves[reg];
    destination.full_handoffs[reg] += source.full_handoffs[reg];
  }
}

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

  [[nodiscard]] static JitGprLinkProfileGlobal &instance() noexcept {
    static auto *aggregate = [] {
      auto *created = new JitGprLinkProfileGlobal;
      if (std::atexit(&JitGprLinkProfileGlobal::report_at_exit) != 0)
        std::fprintf(stderr,
                     "Darwin JIT GPR link profile cannot register report\n");
      return created;
    }();
    return *aggregate;
  }

  void merge(const JitGprLinkProfileDiagnostics &totals,
             const Edges &edges) noexcept {
    std::scoped_lock lock{mutex_};
    add_gpr_link_profile_totals(totals_, totals);
    for (const auto &[edge, count] : edges)
      edges_[edge] += count;
  }

  void register_live(void *context, LiveCollector collect) {
    std::scoped_lock lock{mutex_};
    live_.push_back({context, collect});
  }

  void unregister_live(void *context) noexcept {
    std::scoped_lock lock{mutex_};
    live_.erase(
        std::remove_if(live_.begin(), live_.end(),
                       [&](const auto &profile) {
                         return profile.context == context;
                       }),
        live_.end());
  }

  [[nodiscard]] JitGprLinkProfileDiagnostics diagnostics() noexcept {
    std::scoped_lock lock{mutex_};
    return totals_;
  }

  void report() noexcept {
    std::scoped_lock lock{mutex_};
    if (reported_)
      return;
    reported_ = true;
    /*
     * The macOS frontend reports immediately before its intentional _Exit,
     * so its main thread's TLS executor is still alive. Collect those
     * diagnostic-only counters here instead of scanning every translated
     * edge at each ordinary guest execution boundary.
     */
    for (const auto &profile : live_)
      profile.collect(profile.context, totals_, edges_);
    const auto *path = std::getenv("UML_JIT_GPR_LINK_PROFILE_PATH");
    auto *output =
        path == nullptr || path[0] == '\0' ? stderr : std::fopen(path, "a");
    if (output == nullptr) {
      std::fprintf(stderr,
                   "Darwin JIT GPR link profile cannot open path=%s\n",
                   path);
      output = stderr;
    }
    std::fprintf(
        output,
        "darwin-jit-gpr-link-summary executor=global successful=%llu "
        "unattributed=%llu outer=%llu\n",
        static_cast<unsigned long long>(totals_.successful_links),
        static_cast<unsigned long long>(totals_.unattributed_links),
        static_cast<unsigned long long>(totals_.outer_invocations));
    for (const auto &[edge, count] : edges_) {
      std::fprintf(
          output,
          "darwin-jit-gpr-link-edge executor=global kind=%s source=%#llx "
          "target=%#llx dirty=%#x livein=%#x count=%llu\n",
          edge.indirect ? "pic" : "direct",
          static_cast<unsigned long long>(edge.source_pc),
          static_cast<unsigned long long>(edge.target_pc),
          edge.dirty_live_out, edge.read_before_write,
          static_cast<unsigned long long>(count));
    }
    for (unsigned reg = 0; reg < totals_.live_in_saves.size(); ++reg) {
      const auto saved =
          totals_.live_in_saves[reg] + totals_.dirty_out_saves[reg];
      if (reg == 31U)
        std::fprintf(
            output,
            "darwin-jit-gpr-link-reg executor=global reg=sp livein=%llu "
            "dirty=%llu overlap=%llu saved=%llu\n",
            static_cast<unsigned long long>(totals_.live_in_saves[reg]),
            static_cast<unsigned long long>(totals_.dirty_out_saves[reg]),
            static_cast<unsigned long long>(totals_.full_handoffs[reg]),
            static_cast<unsigned long long>(saved));
      else
        std::fprintf(
            output,
            "darwin-jit-gpr-link-reg executor=global reg=x%u livein=%llu "
            "dirty=%llu overlap=%llu saved=%llu\n",
            reg,
            static_cast<unsigned long long>(totals_.live_in_saves[reg]),
            static_cast<unsigned long long>(totals_.dirty_out_saves[reg]),
            static_cast<unsigned long long>(totals_.full_handoffs[reg]),
            static_cast<unsigned long long>(saved));
    }
    std::fflush(output);
    if (output != stderr)
      std::fclose(output);
  }

private:
  static void report_at_exit() noexcept { instance().report(); }

  std::mutex mutex_;
  Edges edges_;
  std::vector<LiveProfile> live_;
  JitGprLinkProfileDiagnostics totals_{};
  bool reported_{};
};

class JitGprLinkProfiler final {
public:
  JitGprLinkProfiler() noexcept {
    JitGprLinkProfileGlobal::instance().register_live(
        this,
        [](void *context, JitGprLinkProfileDiagnostics &totals,
           JitGprLinkProfileGlobal::Edges &edges) noexcept {
          static_cast<JitGprLinkProfiler *>(context)->collect(totals, edges);
        });
  }

  ~JitGprLinkProfiler() {
    JitGprLinkProfileGlobal::instance().unregister_live(this);
  }

  [[nodiscard]] std::uint64_t *
  add_direct(const JitBlockLink *target, std::uint64_t source_pc,
             std::uint32_t dirty_live_out) {
    auto sample = std::make_unique<JitGprDirectLinkSample>();
    sample->target = target;
    sample->source_pc = source_pc;
    sample->dirty_live_out = dirty_live_out;
    auto *counter = &sample->successes;
    direct_.push_back(std::move(sample));
    return counter;
  }

  [[nodiscard]] jit::JitIndirectLinkProfileCounters
  add_indirect(const JitIndirectLink *site, std::uint64_t source_pc,
               std::uint32_t dirty_live_out) {
    auto sample = std::make_unique<JitGprIndirectLinkSample>();
    sample->site = site;
    sample->source_pc = source_pc;
    sample->dirty_live_out = dirty_live_out;
    auto *first = &sample->successes[0];
    auto *second = &sample->successes[1];
    indirect_by_site_[site].push_back(sample.get());
    indirect_.push_back(std::move(sample));
    return {first, second};
  }

  void record_outer_invocation() noexcept {
    ++lifetime_totals_.outer_invocations;
    ++pending_totals_.outer_invocations;
  }

  void drain_direct() noexcept {
    for (const auto &sample : direct_)
      drain(sample->successes, sample->target, sample->source_pc,
            sample->dirty_live_out, false);
  }

  void drain_indirect_slot(const JitIndirectLink &site,
                           std::size_t slot) noexcept {
    const auto found = indirect_by_site_.find(&site);
    if (found == indirect_by_site_.end())
      return;
    const auto *target = site.profile_targets[slot];
    for (auto *sample : found->second)
      drain(sample->successes[slot], target, sample->source_pc,
            sample->dirty_live_out, true);
  }

  void drain_indirect_site(const JitIndirectLink &site) noexcept {
    drain_indirect_slot(site, 0);
    drain_indirect_slot(site, 1);
  }

  void drain_all() noexcept {
    drain_direct();
    for (const auto &sample : indirect_) {
      for (std::size_t slot = 0; slot < sample->successes.size(); ++slot)
        drain(sample->successes[slot],
              sample->site->profile_targets[slot],
              sample->source_pc, sample->dirty_live_out, true);
    }
  }

  void clear_code() noexcept {
    drain_all();
    direct_.clear();
    indirect_.clear();
    indirect_by_site_.clear();
  }

  [[nodiscard]] JitGprLinkProfileDiagnostics diagnostics() noexcept {
    drain_all();
    return lifetime_totals_;
  }

  void publish() noexcept {
    drain_all();
    if (pending_totals_.successful_links == 0 &&
        pending_totals_.unattributed_links == 0 &&
        pending_totals_.outer_invocations == 0 && pending_edges_.empty())
      return;
    JitGprLinkProfileGlobal::instance().merge(pending_totals_,
                                               pending_edges_);
    pending_totals_ = {};
    pending_edges_.clear();
  }

private:
  void collect(JitGprLinkProfileDiagnostics &totals,
               JitGprLinkProfileGlobal::Edges &edges) noexcept {
    drain_all();
    add_gpr_link_profile_totals(totals, pending_totals_);
    for (const auto &[edge, count] : pending_edges_)
      edges[edge] += count;
    pending_totals_ = {};
    pending_edges_.clear();
  }

  void drain(std::uint64_t &counter, const JitBlockLink *target,
             std::uint64_t source_pc, std::uint32_t dirty_live_out,
             bool indirect) noexcept {
    const auto count = std::exchange(counter, std::uint64_t{});
    if (count == 0)
      return;
    lifetime_totals_.successful_links += count;
    pending_totals_.successful_links += count;
    if (target == nullptr || !target->profile_mask_valid) {
      lifetime_totals_.unattributed_links += count;
      pending_totals_.unattributed_links += count;
      return;
    }
    const auto read_before_write = target->profile_read_before_write;
    const JitGprProfileEdgeKey key{
        .source_pc = source_pc,
        .target_pc = target->guest_pc,
        .dirty_live_out = dirty_live_out,
        .read_before_write = read_before_write,
        .indirect = indirect,
    };
    pending_edges_[key] += count;
    for (unsigned reg = 0; reg < lifetime_totals_.live_in_saves.size();
         ++reg) {
      const auto bit = std::uint32_t{1} << reg;
      if ((read_before_write & bit) != 0) {
        lifetime_totals_.live_in_saves[reg] += count;
        pending_totals_.live_in_saves[reg] += count;
      }
      if ((dirty_live_out & bit) != 0) {
        lifetime_totals_.dirty_out_saves[reg] += count;
        pending_totals_.dirty_out_saves[reg] += count;
      }
      if (((read_before_write & dirty_live_out) & bit) != 0) {
        lifetime_totals_.full_handoffs[reg] += count;
        pending_totals_.full_handoffs[reg] += count;
      }
    }
  }

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

using BlockCache = std::array<CacheSet, cache_sets>;

using SavedGuestWords =
    std::array<std::uint32_t, maximum_block_instructions>;

struct SavedGuestWordSet final {
  std::array<SavedGuestWords, cache_ways> ways;
};

using SavedGuestWordCache = std::array<SavedGuestWordSet, cache_sets>;

struct CacheSlot final {
  CompiledBlock *block{};
  SavedGuestWords *guest_words{};
};

[[nodiscard]] std::size_t cache_index(unsigned long generation,
                                      std::uint64_t pc) noexcept {
  const auto mixed = pc ^ (pc >> 17U) ^
                     (static_cast<std::uint64_t>(generation) * 0x9e37'79b9U);
  return static_cast<std::size_t>(mixed) & (cache_sets - 1);
}

} // namespace

namespace jit::testing {

namespace {

[[nodiscard]] CompiledCode copied_code(const JitCompiler::Result &compiled) {
  CompiledCode result{
      .words = std::vector<std::uint32_t>{compiled.words.begin(),
                                         compiled.words.end()},
      .hot_words = compiled.hot_words,
      .guest_instructions = compiled.guest_instructions,
      .internal_entry_word = compiled.internal_entry_word,
      .memory_write_mask = compiled.memory_write_mask,
      .direct_memory_write_mask = compiled.direct_memory_write_mask,
      .accesses_fp_state = compiled.accesses_fp_state,
      .link_target_safe = compiled.link_target_safe,
      .native_fp = compiled.native_fp,
      .native_fpmr = compiled.native_fpmr,
      .guest_illegal = compiled.guest_illegal,
      .gpr_residency_planned =
          compiled.gpr_residency_plan.count != 0U,
      .gpr_estimated_avoided_accesses =
          compiled.gpr_residency_plan.estimated_avoided_accesses,
  };
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  result.gpr_read_before_write_mask = compiled.profile_read_before_write;
  result.gpr_written_mask = compiled.profile_written;
#endif
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  result.gpr_pressure = compiled.gpr_pressure;
#endif
  return result;
}

} // namespace

#if UML_JIT_PUBLICATION_GATE_TESTING
namespace {

[[nodiscard]] bool jit_exclusive_gate_is_quiescent_for_testing() {
  return jit_exclusive_operation_phase.load(std::memory_order_seq_cst) == 0U &&
         jit_waiting_store_count_value.load(std::memory_order_seq_cst) == 0U &&
         jit_exclusive_operation_count.load(std::memory_order_seq_cst) == 0U &&
         !jit_exclusive_operation_turn.load(std::memory_order_seq_cst);
}

void wait_for_jit_exclusive_turn_waiter() {
  while (jit_exclusive_operation_count.load(
             std::memory_order_seq_cst) < 2U)
    std::this_thread::yield();
}

[[maybe_unused, nodiscard]] bool
waiting_store_precedes_queued_helper_for_testing(bool queue_helper_first) {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  JitStoreParticipant participant;
  CpuState cpu{};
  register_jit_store_participant(participant);

  std::binary_semaphore first_helper_entered{0};
  std::binary_semaphore release_first_helper{0};
  std::binary_semaphore store_started{0};
  std::binary_semaphore store_entered{0};
  std::binary_semaphore release_store{0};
  std::atomic<unsigned> entry_order{};
  std::atomic<unsigned> store_order{};
  std::atomic<unsigned> second_helper_order{};

  std::thread first_helper{[&] {
    JitExclusiveOperationScope scope;
    first_helper_entered.release();
    release_first_helper.acquire();
  }};
  first_helper_entered.acquire();

  std::thread store;
  const auto start_store = [&] {
    store = std::thread{[&] {
      store_started.release();
      JitBlockPublication publication{participant, cpu};
      store_order.store(
          entry_order.fetch_add(1, std::memory_order_relaxed) + 1U,
          std::memory_order_relaxed);
      store_entered.release();
      release_store.acquire();
    }};
    store_started.acquire();
    while (jit_waiting_store_count(jit_exclusive_operation_phase.load(
               std::memory_order_seq_cst)) == 0U ||
           participant.state.load(std::memory_order_seq_cst) !=
               JitStoreState::idle)
      std::this_thread::yield();
  };

  std::thread second_helper;
  const auto start_second_helper = [&] {
    second_helper = std::thread{[&] {
      JitExclusiveOperationScope scope;
      second_helper_order.store(
          entry_order.fetch_add(1, std::memory_order_relaxed) + 1U,
          std::memory_order_relaxed);
    }};
    wait_for_jit_exclusive_turn_waiter();
  };

  if (queue_helper_first) {
    start_second_helper();
    start_store();
  } else {
    start_store();
    start_second_helper();
  }

  const auto phase_before_release =
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
  const bool both_waiting_before_release =
      jit_exclusive_operation_is_active(phase_before_release) &&
      jit_waiting_store_count(phase_before_release) == 1U &&
      store_order.load(std::memory_order_relaxed) == 0U &&
      second_helper_order.load(std::memory_order_relaxed) == 0U &&
      participant.state.load(std::memory_order_seq_cst) ==
          JitStoreState::idle;

  release_first_helper.release();
  first_helper.join();
  store_entered.acquire();
  while (!jit_exclusive_operation_is_active(
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst)))
    std::this_thread::yield();
  const auto phase_while_store_runs =
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
  const bool store_won_phase =
      store_order.load(std::memory_order_relaxed) == 1U &&
      second_helper_order.load(std::memory_order_relaxed) == 0U &&
      jit_exclusive_operation_is_active(phase_while_store_runs) &&
      jit_waiting_store_count(phase_while_store_runs) == 0U &&
      participant.state.load(std::memory_order_seq_cst) ==
          JitStoreState::direct;

  release_store.release();
  store.join();
  second_helper.join();

  const bool completed_in_order =
      second_helper_order.load(std::memory_order_relaxed) == 2U &&
      participant.state.load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      jit_exclusive_gate_is_quiescent_for_testing();
  unregister_jit_store_participant(participant);
  return both_waiting_before_release && store_won_phase &&
         completed_in_order;
}

} // namespace

bool exclusive_operations_are_serialized_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  std::binary_semaphore first_entered{0};
  std::binary_semaphore release_first{0};
  std::binary_semaphore second_entered{0};

  std::thread first{[&] {
    JitExclusiveOperationScope scope;
    first_entered.release();
    release_first.acquire();
  }};
  first_entered.acquire();

  std::thread second{[&] {
    JitExclusiveOperationScope scope;
    second_entered.release();
  }};
  wait_for_jit_exclusive_turn_waiter();

  const auto queued_phase =
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
  const bool follower_parked =
      !second_entered.try_acquire() &&
      jit_exclusive_operation_is_active(queued_phase) &&
      jit_waiting_store_count(queued_phase) == 0U;

  release_first.release();
  first.join();
  if (follower_parked)
    second_entered.acquire();
  second.join();
  return follower_parked && jit_exclusive_gate_is_quiescent_for_testing();
}

bool normal_store_leaves_exclusive_gate_unchanged_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  JitStoreParticipant participant;
  CpuState cpu{};
  register_jit_store_participant(participant);
  bool direct_without_mutation = false;
  {
    JitBlockPublication publication{participant, cpu};
    direct_without_mutation =
        !publication.needs_tracking() &&
        participant.state.load(std::memory_order_seq_cst) ==
            JitStoreState::direct &&
        jit_exclusive_operation_phase.load(std::memory_order_seq_cst) == 0U;
  }
  const bool unchanged_after =
      participant.state.load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst) == 0U;
  unregister_jit_store_participant(participant);
  return direct_without_mutation && unchanged_after;
}

bool store_first_publication_handshake_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  JitStoreParticipant participant;
  CpuState cpu{};
  register_jit_store_participant(participant);

  std::binary_semaphore store_entered{0};
  std::binary_semaphore release_store{0};
  std::binary_semaphore helper_entered{0};
  std::binary_semaphore release_helper{0};
  std::thread store{[&] {
    JitBlockPublication publication{participant, cpu};
    store_entered.release();
    release_store.acquire();
  }};
  store_entered.acquire();

  std::thread helper{[&] {
    JitExclusiveOperationScope scope;
    helper_entered.release();
    release_helper.acquire();
  }};
  while (!jit_exclusive_operation_is_active(
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst)))
    std::this_thread::yield();

  const auto phase_while_helper_waits =
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
  const bool helper_crossed_published_store = helper_entered.try_acquire();
  release_store.release();
  store.join();
  if (!helper_crossed_published_store)
    helper_entered.acquire();
  release_helper.release();
  helper.join();

  const bool idle =
      participant.state.load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      jit_exclusive_gate_is_quiescent_for_testing();
  unregister_jit_store_participant(participant);
  return !helper_crossed_published_store &&
         jit_exclusive_operation_is_active(phase_while_helper_waits) &&
         jit_waiting_store_count(phase_while_helper_waits) == 0U && idle;
}

bool waiting_store_precedes_later_helper_for_testing() {
  return waiting_store_precedes_queued_helper_for_testing(false);
}

bool waiting_store_precedes_already_queued_helper_for_testing() {
  return waiting_store_precedes_queued_helper_for_testing(true);
}

bool all_waiting_stores_precede_queued_helper_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  JitStoreParticipant first_participant;
  JitStoreParticipant second_participant;
  CpuState first_cpu{};
  CpuState second_cpu{};
  register_jit_store_participant(first_participant);
  register_jit_store_participant(second_participant);

  std::binary_semaphore first_helper_entered{0};
  std::binary_semaphore release_first_helper{0};
  std::binary_semaphore first_store_started{0};
  std::binary_semaphore second_store_started{0};
  std::binary_semaphore first_store_entered{0};
  std::binary_semaphore second_store_entered{0};
  std::binary_semaphore release_first_store{0};
  std::binary_semaphore release_second_store{0};
  std::binary_semaphore queued_helper_entered{0};
  std::atomic<unsigned> released_stores{};
  std::atomic<unsigned> helper_observed_releases{};

  std::thread first_helper{[&] {
    JitExclusiveOperationScope scope;
    first_helper_entered.release();
    release_first_helper.acquire();
  }};
  first_helper_entered.acquire();

  std::thread first_store{[&] {
    first_store_started.release();
    JitBlockPublication publication{first_participant, first_cpu};
    first_store_entered.release();
    release_first_store.acquire();
  }};
  std::thread second_store{[&] {
    second_store_started.release();
    JitBlockPublication publication{second_participant, second_cpu};
    second_store_entered.release();
    release_second_store.acquire();
  }};
  first_store_started.acquire();
  second_store_started.acquire();
  while (jit_waiting_store_count(jit_exclusive_operation_phase.load(
             std::memory_order_seq_cst)) != 2U ||
         first_participant.state.load(std::memory_order_seq_cst) !=
             JitStoreState::idle ||
         second_participant.state.load(std::memory_order_seq_cst) !=
             JitStoreState::idle)
    std::this_thread::yield();

  std::thread queued_helper{[&] {
    JitExclusiveOperationScope scope;
    helper_observed_releases.store(
        released_stores.load(std::memory_order_seq_cst),
        std::memory_order_seq_cst);
    queued_helper_entered.release();
  }};
  wait_for_jit_exclusive_turn_waiter();

  const auto phase_before_release =
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
  const auto stores_waiting_before_release =
      jit_waiting_store_count(phase_before_release);
  release_first_helper.release();
  first_helper.join();
  first_store_entered.acquire();
  second_store_entered.acquire();
  while (!jit_exclusive_operation_is_active(
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst)))
    std::this_thread::yield();

  const bool helper_crossed_both_stores =
      queued_helper_entered.try_acquire();
  const bool both_stores_won_phase =
      jit_exclusive_operation_is_active(phase_before_release) &&
      stores_waiting_before_release == 2U &&
      first_participant.state.load(std::memory_order_seq_cst) ==
          JitStoreState::direct &&
      second_participant.state.load(std::memory_order_seq_cst) ==
          JitStoreState::direct &&
      !helper_crossed_both_stores;

  released_stores.store(1U, std::memory_order_seq_cst);
  release_first_store.release();
  first_store.join();
  const bool helper_crossed_second_store =
      queued_helper_entered.try_acquire();
  const bool helper_waited_for_second =
      second_participant.state.load(std::memory_order_seq_cst) ==
          JitStoreState::direct &&
      !helper_crossed_second_store;

  released_stores.store(2U, std::memory_order_seq_cst);
  release_second_store.release();
  second_store.join();
  if (!helper_crossed_both_stores && !helper_crossed_second_store)
    queued_helper_entered.acquire();
  queued_helper.join();

  const bool completed =
      helper_observed_releases.load(std::memory_order_seq_cst) == 2U &&
      first_participant.state.load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      second_participant.state.load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      jit_exclusive_gate_is_quiescent_for_testing();
  unregister_jit_store_participant(second_participant);
  unregister_jit_store_participant(first_participant);
  return both_stores_won_phase && helper_waited_for_second && completed;
}

bool late_store_waits_for_current_helper_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  JitStoreParticipant participant;
  CpuState cpu{};
  register_jit_store_participant(participant);
  std::binary_semaphore helper_entered{0};
  std::binary_semaphore release_helper{0};
  std::binary_semaphore store_entered{0};

  std::thread helper{[&] {
    JitExclusiveOperationScope scope;
    helper_entered.release();
    release_helper.acquire();
  }};
  helper_entered.acquire();
  std::thread store{[&] {
    JitBlockPublication publication{participant, cpu};
    store_entered.release();
  }};
  while (participant.state.load(std::memory_order_seq_cst) !=
         JitStoreState::idle)
    std::this_thread::yield();
  const bool parked = !store_entered.try_acquire();
  release_helper.release();
  helper.join();
  if (parked)
    store_entered.acquire();
  store.join();
  const bool completed =
      parked &&
      participant.state.load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      jit_exclusive_gate_is_quiescent_for_testing();
  unregister_jit_store_participant(participant);
  return completed;
}

bool tracked_store_invalidation_does_not_deadlock_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  struct Shared final {
    JitStoreParticipant participant;
    CpuState reservation_cpu{};
    CpuState store_cpu{};
    alignas(16) std::array<std::byte, 16> backing{};
    std::array<std::uint64_t, 2> ranges{};
    std::binary_semaphore store_published{0};
    std::binary_semaphore begin_invalidation{0};
    std::binary_semaphore invalidation_finished{0};
    std::binary_semaphore helper_entered{0};
    std::atomic<bool> tracked{};
  };
  auto shared = std::make_shared<Shared>();
  shared->ranges = {
      reinterpret_cast<std::uint64_t>(shared->backing.data()),
      shared->backing.size(),
  };
  register_jit_store_participant(shared->participant);
  if (!begin_jit_reservation(shared->reservation_cpu)) {
    unregister_jit_store_participant(shared->participant);
    return false;
  }

  std::thread store{[shared] {
    {
      JitBlockPublication publication{shared->participant, shared->store_cpu};
      shared->tracked.store(publication.needs_tracking(),
                            std::memory_order_seq_cst);
      shared->store_published.release();
      shared->begin_invalidation.acquire();
      uml_arm64_jit_invalidate_store_ranges(shared->ranges.data(), 1);
    }
    shared->invalidation_finished.release();
  }};
  shared->store_published.acquire();

  std::thread helper{[shared] {
    JitExclusiveOperationScope scope;
    shared->helper_entered.release();
  }};
  while (!jit_exclusive_operation_is_active(
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst)))
    std::this_thread::yield();

  const bool helper_crossed_store = shared->helper_entered.try_acquire();
  const bool tracked_before_invalidation =
      shared->tracked.load(std::memory_order_seq_cst) &&
      shared->participant.state.load(std::memory_order_seq_cst) ==
          JitStoreState::tracked &&
      !helper_crossed_store;
  shared->begin_invalidation.release();

  constexpr auto timeout = std::chrono::seconds{5};
  if (!shared->invalidation_finished.try_acquire_for(timeout)) {
    /*
     * A semantic-lock-before-publication-scan regression deadlocks these two
     * threads. Keep their shared state alive until the test process reports
     * failure instead of blocking forever in thread destructors.
     */
    store.detach();
    helper.detach();
    return false;
  }
  store.join();
  const bool helper_completed =
      helper_crossed_store ||
      shared->helper_entered.try_acquire_for(timeout);
  if (!helper_completed) {
    helper.detach();
    return false;
  }
  helper.join();

  const bool completed =
      tracked_before_invalidation && !helper_crossed_store &&
      shared->participant.state.load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      jit_exclusive_gate_is_quiescent_for_testing();
  end_jit_reservation(shared->reservation_cpu);
  unregister_jit_store_participant(shared->participant);
  return completed;
}

bool publication_gate_randomized_stress_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  constexpr std::size_t helper_threads = 3;
  constexpr std::size_t store_threads = 4;
  constexpr unsigned helper_iterations = 400;
  constexpr unsigned store_iterations = 600;
  std::array<JitStoreParticipant, store_threads> participants;
  std::array<CpuState, store_threads> cpus{};
  for (auto &participant : participants)
    register_jit_store_participant(participant);

  std::atomic<bool> start{};
  std::atomic<bool> failed{};
  std::atomic<unsigned> active_helpers{};
  std::atomic<unsigned> active_stores{};
  const auto jitter = [](std::uint64_t &state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    if ((state & 7U) == 0U)
      std::this_thread::yield();
  };

  std::array<std::thread, helper_threads> helpers;
  for (std::size_t index = 0; index < helpers.size(); ++index) {
    helpers[index] = std::thread{[&, index] {
      std::uint64_t random = 0x9e37'79b9'7f4a'7c15ULL ^
                             (index * 0x100'0000'01b3ULL);
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (unsigned iteration = 0; iteration < helper_iterations;
           ++iteration) {
        jitter(random);
        JitExclusiveOperationScope scope;
        if (active_helpers.fetch_add(1, std::memory_order_seq_cst) != 0U ||
            active_stores.load(std::memory_order_seq_cst) != 0U)
          failed.store(true, std::memory_order_seq_cst);
        jitter(random);
        if (active_stores.load(std::memory_order_seq_cst) != 0U)
          failed.store(true, std::memory_order_seq_cst);
        active_helpers.fetch_sub(1, std::memory_order_seq_cst);
      }
    }};
  }

  std::array<std::thread, store_threads> stores;
  for (std::size_t index = 0; index < stores.size(); ++index) {
    stores[index] = std::thread{[&, index] {
      std::uint64_t random = 0xd1b5'4a32'd192'ed03ULL ^
                             (index * 0x9e37'79b9ULL);
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (unsigned iteration = 0; iteration < store_iterations;
           ++iteration) {
        jitter(random);
        JitBlockPublication publication{participants[index], cpus[index]};
        active_stores.fetch_add(1, std::memory_order_seq_cst);
        if (active_helpers.load(std::memory_order_seq_cst) != 0U)
          failed.store(true, std::memory_order_seq_cst);
        jitter(random);
        if (active_helpers.load(std::memory_order_seq_cst) != 0U)
          failed.store(true, std::memory_order_seq_cst);
        active_stores.fetch_sub(1, std::memory_order_seq_cst);
      }
    }};
  }

  start.store(true, std::memory_order_release);
  for (auto &helper : helpers)
    helper.join();
  for (auto &store : stores)
    store.join();

  const bool all_idle = std::ranges::all_of(
      participants, [](const auto &participant) {
        return participant.state.load(std::memory_order_seq_cst) ==
               JitStoreState::idle;
      });
  const bool completed =
      !failed.load(std::memory_order_seq_cst) &&
      active_helpers.load(std::memory_order_seq_cst) == 0U &&
      active_stores.load(std::memory_order_seq_cst) == 0U && all_idle &&
      jit_exclusive_gate_is_quiescent_for_testing();
  for (auto &participant : participants)
    unregister_jit_store_participant(participant);
  return completed;
}
#endif

CompiledCode compile_block(
    std::uint64_t pc,
    std::span<const std::uint32_t> instructions) {
  JitCompiler compiler{/*enable_guarded_control_flow=*/false};
  return copied_code(compiler.compile(pc, instructions));
}

CompiledCode compile_block_without_table_list_planning(
    std::uint64_t pc,
    std::span<const std::uint32_t> instructions) {
  JitCompiler compiler{/*enable_guarded_control_flow=*/false};
  compiler.set_table_list_planning_enabled(false);
  return copied_code(compiler.compile(pc, instructions));
}

CompiledCode compile_block_without_gpr_residency_planning(
    std::uint64_t pc,
    std::span<const std::uint32_t> instructions) {
  JitCompiler compiler{/*enable_guarded_control_flow=*/false};
  compiler.set_gpr_residency_planning_enabled(false);
  return copied_code(compiler.compile(pc, instructions));
}

#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
CompiledCode compile_gpr_pressure_profile_block(
    std::uint64_t pc, std::span<const std::uint32_t> instructions,
    bool instrument) {
  JitCompiler compiler{/*enable_guarded_control_flow=*/false};
  compiler.set_gpr_pressure_profile_enabled(instrument);
  compiler.set_gpr_residency_planning_enabled(false);
  return copied_code(compiler.compile(pc, instructions));
}
#endif

CompiledCode compile_block_with_feature_override(
    std::uint64_t pc, std::span<const std::uint32_t> instructions,
    ArmFeature feature, bool available) {
  auto features = host_arm_features().supported;
  features.set(static_cast<std::size_t>(feature), available);
  JitCompiler compiler{/*enable_guarded_control_flow=*/false, features};
  return copied_code(compiler.compile(pc, instructions));
}

std::vector<std::uint32_t>
lower_memory_barrier_for_testing(std::uint32_t instruction, bool native_sb,
                                 bool native_xs) {
  const auto lowered =
      lower_memory_barrier(instruction, native_sb, native_xs);
  return {lowered.words.begin(), lowered.words.begin() + lowered.count};
}

bool verification_replay_safe_for_testing(
    std::uint32_t instruction) noexcept {
  return verification_replay_safe(instruction);
}

CompiledCode compile_tracked_block(
    std::uint64_t pc,
    std::span<const std::uint32_t> instructions) {
  JitCompiler compiler{/*enable_guarded_control_flow=*/false,
                       /*link_context=*/nullptr,
                       /*link_resolver=*/nullptr,
                       /*track_direct_stores=*/true};
  return copied_code(compiler.compile(pc, instructions));
}

CompiledCode compile_linked_block(
    std::uint64_t pc,
    std::span<const std::uint32_t> instructions) {
  JitBlockLink link{};
  const auto resolver = +[](void *context, std::uint64_t target) -> const void * {
    auto &resolved = *static_cast<JitBlockLink *>(context);
    resolved.guest_pc = target;
    return &resolved;
  };
  JitCompiler compiler{/*enable_guarded_control_flow=*/false, &link, resolver};
  return copied_code(compiler.compile(pc, instructions));
}

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
CompiledCode compile_gpr_profile_linked_block(
    std::uint64_t pc, std::span<const std::uint32_t> instructions,
    bool instrument) {
  struct Context final {
    JitBlockLink link{};
    std::uint64_t counter{};
    std::uint32_t dirty_live_out{};
  } context;
  const auto resolver = +[](void *opaque,
                            std::uint64_t target) -> const void * {
    auto &resolved = *static_cast<Context *>(opaque);
    resolved.link.guest_pc = target;
    return &resolved.link;
  };
  const auto profile_resolver =
      +[](void *opaque, const void *, std::uint64_t,
          std::uint32_t dirty_live_out) -> std::uint64_t * {
    auto &resolved = *static_cast<Context *>(opaque);
    resolved.dirty_live_out = dirty_live_out;
    return &resolved.counter;
  };
  JitCompiler compiler{
      /*enable_guarded_control_flow=*/false,
      &context,
      resolver,
      /*track_direct_stores=*/false,
      /*indirect_link_resolver=*/nullptr,
      /*persistent_linked_block_abi=*/persistent_linked_block_abi_default,
      instrument ? &context : nullptr,
      instrument ? profile_resolver : nullptr,
      /*indirect_profile_resolver=*/nullptr};
  auto compiled = copied_code(compiler.compile(pc, instructions));
  compiled.gpr_link_dirty_live_out_mask = context.dirty_live_out;
  return compiled;
}

bool gpr_profile_disabled_resolver_preserves_linked_code(
    std::uint64_t pc, std::span<const std::uint32_t> instructions) {
  JitBlockLink link{};
  const auto resolver = +[](void *opaque,
                            std::uint64_t target) -> const void * {
    auto &resolved = *static_cast<JitBlockLink *>(opaque);
    resolved.guest_pc = target;
    return &resolved;
  };
  JitCompiler baseline{/*enable_guarded_control_flow=*/false, &link, resolver};
  const auto expected = copied_code(baseline.compile(pc, instructions));
  JitCompiler diagnostic_disabled{
      /*enable_guarded_control_flow=*/false,
      &link,
      resolver,
      /*track_direct_stores=*/false,
      /*indirect_link_resolver=*/nullptr,
      /*persistent_linked_block_abi=*/persistent_linked_block_abi_default,
      &link,
      /*direct_profile_resolver=*/nullptr,
      /*indirect_profile_resolver=*/nullptr};
  const auto actual =
      copied_code(diagnostic_disabled.compile(pc, instructions));
  return actual.words == expected.words &&
         actual.hot_words == expected.hot_words &&
         actual.guest_instructions == expected.guest_instructions &&
         actual.internal_entry_word == expected.internal_entry_word &&
         actual.memory_write_mask == expected.memory_write_mask &&
         actual.direct_memory_write_mask == expected.direct_memory_write_mask &&
         actual.accesses_fp_state == expected.accesses_fp_state &&
         actual.link_target_safe == expected.link_target_safe &&
         actual.native_fp == expected.native_fp &&
         actual.native_fpmr == expected.native_fpmr;
}

bool gpr_profile_disabled_resolver_preserves_indirect_code(
    std::uint64_t pc, std::span<const std::uint32_t> instructions) {
  struct Context final {
    JitBlockLink empty{.guest_pc = 1};
    JitIndirectLink site{};
  } context;
  context.site.source_pc = pc;
  for (auto &entry : context.site.entries) {
    entry.guest_pc = 1;
    entry.link_entries = context.empty.entries.data();
  }
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  context.site.profile_targets.fill(&context.empty);
#endif
  const auto resolver =
      +[](void *opaque, std::uint64_t) -> const void * {
    return &static_cast<Context *>(opaque)->site;
  };
  JitCompiler baseline{/*enable_guarded_control_flow=*/false,
                       &context,
                       /*link_resolver=*/nullptr,
                       /*track_direct_stores=*/false,
                       resolver};
  const auto expected = copied_code(baseline.compile(pc, instructions));
  JitCompiler diagnostic_disabled{
      /*enable_guarded_control_flow=*/false,
      &context,
      /*link_resolver=*/nullptr,
      /*track_direct_stores=*/false,
      resolver,
      /*persistent_linked_block_abi=*/persistent_linked_block_abi_default,
      &context,
      /*direct_profile_resolver=*/nullptr,
      /*indirect_profile_resolver=*/nullptr};
  const auto actual =
      copied_code(diagnostic_disabled.compile(pc, instructions));
  return actual.words == expected.words &&
         actual.hot_words == expected.hot_words &&
         actual.guest_instructions == expected.guest_instructions &&
         actual.internal_entry_word == expected.internal_entry_word &&
         actual.memory_write_mask == expected.memory_write_mask &&
         actual.direct_memory_write_mask == expected.direct_memory_write_mask &&
         actual.accesses_fp_state == expected.accesses_fp_state &&
         actual.link_target_safe == expected.link_target_safe &&
         actual.native_fp == expected.native_fp &&
         actual.native_fpmr == expected.native_fpmr;
}

void publish_gpr_link_profile_delta_for_testing(
    std::uint64_t source_pc, std::uint64_t target_pc,
    std::uint32_t dirty_live_out, std::uint32_t read_before_write,
    std::uint64_t count, bool indirect) {
  JitGprLinkProfileDiagnostics totals{
      .successful_links = count,
      .outer_invocations = count,
  };
  for (unsigned reg = 0; reg < totals.live_in_saves.size(); ++reg) {
    const auto bit = std::uint32_t{1} << reg;
    totals.live_in_saves[reg] =
        (read_before_write & bit) != 0 ? count : 0;
    totals.dirty_out_saves[reg] =
        (dirty_live_out & bit) != 0 ? count : 0;
    totals.full_handoffs[reg] =
        ((dirty_live_out & read_before_write) & bit) != 0 ? count : 0;
  }
  const JitGprProfileEdgeKey key{
      .source_pc = source_pc,
      .target_pc = target_pc,
      .dirty_live_out = dirty_live_out,
      .read_before_write = read_before_write,
      .indirect = indirect,
  };
  JitGprLinkProfileGlobal::Edges edges{{key, count}};
  JitGprLinkProfileGlobal::instance().merge(totals, edges);
}
#endif

CompiledCode compile_linked_block_memory_abi(
    std::uint64_t pc,
    std::span<const std::uint32_t> instructions) {
  JitBlockLink link{};
  const auto resolver = +[](void *context, std::uint64_t target) -> const void * {
    auto &resolved = *static_cast<JitBlockLink *>(context);
    resolved.guest_pc = target;
    return &resolved;
  };
  JitCompiler compiler{/*enable_guarded_control_flow=*/false, &link, resolver,
                       /*track_direct_stores=*/false,
                       /*indirect_link_resolver=*/nullptr,
                       /*persistent_linked_block_abi=*/false};
  return copied_code(compiler.compile(pc, instructions));
}

CompiledCode compile_indirect_linked_block(
    std::uint64_t pc,
    std::span<const std::uint32_t> instructions) {
  struct Context final {
    JitBlockLink empty{.guest_pc = 1};
    JitIndirectLink site{};
  } context;
  context.site.source_pc = pc;
  for (auto &entry : context.site.entries) {
    entry.guest_pc = 1;
    entry.link_entries = context.empty.entries.data();
  }
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  context.site.profile_targets.fill(&context.empty);
#endif
  const auto resolver =
      +[](void *opaque, std::uint64_t) -> const void * {
    return &static_cast<Context *>(opaque)->site;
  };
  JitCompiler compiler{/*enable_guarded_control_flow=*/false, &context,
                       nullptr, false, resolver};
  return copied_code(compiler.compile(pc, instructions));
}

CompiledCode compile_indirect_linked_block_memory_abi(
    std::uint64_t pc,
    std::span<const std::uint32_t> instructions) {
  struct Context final {
    JitBlockLink empty{.guest_pc = 1};
    JitIndirectLink site{};
  } context;
  context.site.source_pc = pc;
  for (auto &entry : context.site.entries) {
    entry.guest_pc = 1;
    entry.link_entries = context.empty.entries.data();
  }
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  context.site.profile_targets.fill(&context.empty);
#endif
  const auto resolver =
      +[](void *opaque, std::uint64_t) -> const void * {
    return &static_cast<Context *>(opaque)->site;
  };
  JitCompiler compiler{/*enable_guarded_control_flow=*/false, &context,
                       /*link_resolver=*/nullptr,
                       /*track_direct_stores=*/false, resolver,
                       /*persistent_linked_block_abi=*/false};
  return copied_code(compiler.compile(pc, instructions));
}

std::vector<std::uint32_t>
compile_stable_pointer_load_for_testing(std::uintptr_t value,
                                        std::size_t literal_range_words) {
  Arm64Emitter emitter{persistent_linked_block_abi_default,
                       literal_range_words};
  emitter.load_stable_pointer(12, value);
  emitter.emit(0xd65f'03c0U); // ret
  const auto words = emitter.finish();
  return {words.begin(), words.end()};
}

std::vector<std::uint32_t> compile_two_stable_pointer_loads_for_testing(
    std::uintptr_t first, std::uintptr_t second,
    std::size_t literal_range_words) {
  Arm64Emitter emitter{persistent_linked_block_abi_default,
                       literal_range_words};
  emitter.load_stable_pointer(12, first);
  emitter.load_stable_pointer(13, second);
  emitter.emit(0xd65f'03c0U); // ret
  const auto words = emitter.finish();
  return {words.begin(), words.end()};
}

std::vector<std::uint32_t>
materialize_constant_for_testing(unsigned destination, std::uint64_t value) {
  Arm64Emitter emitter;
  emitter.load_constant(destination, value);
  const auto words = emitter.finish();
  return {words.begin(), words.end()};
}

bool persistent_linked_block_abi_enabled() noexcept {
  return persistent_linked_block_abi_default;
}

std::size_t persistent_link_successful_path_word_savings() noexcept {
  /*
   * Each successful edge removes the chain-pointer, selector-byte, and
   * remaining-count loads plus the remaining-count store. The persistent
   * subtract itself is shared with the memory-backed sequence.
   */
  return 4;
}

std::size_t successful_link_host_word_savings() noexcept {
  /*
   * The common basic-frame transition would reload x0/x1/x2 and unwind
   * before its indirect branch (five words including BR), then execute the
   * successor's external prologue. The persistent internal ABI needs only the
   * existing one-word BR.
   */
  constexpr std::size_t legacy_tail_words = 5;
  constexpr std::size_t external_prologue_words =
      persistent_linked_block_abi_default ? 10 : 6;
  constexpr std::size_t internal_tail_words = 1;
  return legacy_tail_words + external_prologue_words - internal_tail_words;
}

std::size_t link_chain_instruction_limit() noexcept {
  return maximum_chain_instructions;
}

std::size_t link_capability_selector_count() noexcept {
  return jit_link_selector_count;
}

std::size_t block_link_record_size() noexcept {
  return sizeof(JitBlockLink);
}

std::size_t block_link_record_alignment() noexcept {
  return alignof(JitBlockLink);
}

std::size_t block_link_entries_offset() noexcept {
  return offsetof(JitBlockLink, entries);
}

std::size_t indirect_link_entry_size() noexcept {
  return sizeof(JitIndirectLinkEntry);
}

std::uint8_t
link_selector_for_capability_mask(std::uint8_t capabilities) noexcept {
  return link_selector_for_capabilities(capabilities);
}

std::uint8_t
link_capability_mask_for_selector(std::size_t selector) noexcept {
  if (selector >= jit_link_selector_capabilities.size())
    return invalid_link_selector;
  return jit_link_selector_capabilities[selector];
}

bool block_link_table_is_bounded(std::size_t capacity) {
  JitBlockLinkTable table{capacity};
  if (capacity == 0)
    return table.find_or_create(1) == nullptr && table.size() == 0;
  auto *first = table.find_or_create(0x1000);
  if (first == nullptr)
    return false;
  first->entries[link_capability(JitLinkCapability::store_publication)] =
      0x1234;
  for (std::size_t index = 1; index < capacity; ++index)
    if (table.find_or_create(0x1000 + index * 4U) == nullptr)
      return false;
  return table.size() == capacity &&
         table.find_or_create(0x1000) == first &&
         table.find_or_create(0x1000)
                 ->entries[link_capability(
                     JitLinkCapability::store_publication)] == 0x1234 &&
         table.find_or_create(0x8000'0000) == nullptr &&
         table.size() == capacity;
}

bool indirect_link_table_is_bounded(std::size_t capacity) {
  JitBlockLink empty{.guest_pc = 1};
  JitIndirectLinkTable table{capacity, &empty};
  if (capacity == 0)
    return table.find_or_create(0x1000) == nullptr && table.size() == 0;
  auto *first = table.find_or_create(0x1000);
  if (first == nullptr || first->source_pc != 0x1000)
    return false;
  first->entries[0].guest_pc = 0x2000;
  first->entries[0].link_entries = empty.entries.data();
  for (std::size_t index = 1; index < capacity; ++index)
    if (table.find_or_create(0x1000 + index * 4U) == nullptr)
      return false;
  return table.size() == capacity &&
         table.find_or_create(0x1000) == first &&
         first->entries[0].guest_pc == 0x2000 &&
         first->entries[0].link_entries == empty.entries.data() &&
         table.find_or_create(0x8000'0000) == nullptr &&
         table.size() == capacity;
}

} // namespace jit::testing

class JitExecutor::Implementation final {
public:
  explicit Implementation(JitExecutorOptions options)
      : empty_link_{.guest_pc = 1},
        links_{maximum_block_links},
        indirect_links_{maximum_indirect_links, &empty_link_},
        arena_{options.code_arena_size == 0
                   ? arena_size
                   : options.code_arena_size},
        enable_guarded_control_flow_{options.enable_guarded_control_flow},
        enable_pointer_authentication_{
            options.enable_pointer_authentication},
        reuse_validated_stale_blocks_{
            options.reuse_validated_stale_blocks} {
    verification_active_ = verification_configured();
    active_links_.reserve(cache_sets * cache_ways);
    active_indirect_links_.reserve(cache_sets * cache_ways);
    register_jit_store_participant(store_participant_);
  }

  ~Implementation() {
    if (current_cpu_ != nullptr)
      arm64::gadgets::restore_native_fp_environment(*current_cpu_);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    gpr_link_profile_.publish();
#endif
    release_current_reservation();
    unregister_jit_store_participant(store_participant_);
    darwin_uml_static_vm_execution_context_destroy(execution_context_);
  }

  [[nodiscard]] bool begin_execution(unsigned long generation,
                                     std::uint64_t guest_pc) {
    auto *context = execution_context(generation);
    return context != nullptr &&
           darwin_uml_static_vm_execution_context_begin(context, guest_pc) == 0;
  }

  void end_execution() noexcept {
    pending_indirect_ = {};
    if (current_cpu_ != nullptr)
      arm64::gadgets::restore_native_fp_environment(*current_cpu_);
    release_current_reservation();
    current_cpu_ = nullptr;
    if (execution_context_ != nullptr)
      (void)darwin_uml_static_vm_execution_context_end(execution_context_);
  }

  [[nodiscard]] Event run_block(CpuState &cpu, StaticGuestMemory &memory,
                                unsigned long generation,
                                std::size_t &executed) {
    executed = 0;
    floating_point_syndrome_ = 0;
    select_cpu(cpu);
    const auto sequences =
        darwin_uml_static_vm_execution_context_sequences(execution_context_);
    preempted_ = sequences.execution_interrupt != 0;
    if (preempted_)
      return Event::interrupt;
    if (mapping_generation_ != generation ||
        mapping_changes_ != sequences.mapping_changes) {
      memory.prepare(generation, sequences.mapping_changes);
      tlb_.prepare(generation, sequences.mapping_changes);
      mapping_generation_ = generation;
      mapping_changes_ = sequences.mapping_changes;
    }
    prepare_links(generation, sequences.code_changes);
    const auto block_pc = cpu.pc;
    const auto pending_source = consume_pending_indirect(
        cpu, generation, sequences.code_changes, block_pc);

    /*
     * Enabled guest exception traps are rare.  Keep them completely outside
     * the cached multi-instruction hot path so no translated block can run
     * past a potentially trapping instruction.  This cold path still lowers
     * and executes the exact native JIT form; it merely limits the boundary
     * to one guest instruction so a register-only FP/SIMD operation can be
     * committed transactionally.
     */
    const auto guest_fpcr = cpu.fp_loaded ? cpu.fpcr : cpu.deferred_fpcr;
    if (arm64::detail::enabled_exception_flags(guest_fpcr) != 0U) {
      indirect_link_diagnostics_.unsafe_target_refusals +=
          pending_source.has_value();
      return run_floating_exception_mode(cpu, memory, executed);
    }

    auto *block =
        find(generation, sequences.code_changes, cpu.pc, memory);
    if (block == nullptr) {
      std::array<std::uint32_t, maximum_block_instructions> instructions{};
      std::size_t count = 0;
      while (count < instructions.size()) {
        std::array<std::byte, sizeof(std::uint32_t)> encoded{};
        if (!memory.fetch(cpu.pc + count * 4, encoded))
          break;
        std::memcpy(&instructions[count], encoded.data(), encoded.size());
        const auto terminates =
            obvious_translated_block_terminator(instructions[count]);
        ++count;
        if (terminates)
          break;
      }
      if (count == 0) {
        const auto fault = memory.last_fault();
        tlb_.fault_address = fault.address;
        tlb_.fault_access = fault.access == arm64::MemoryAccess::write
                                ? Access::write
                                : Access::read;
        return Event::memory_fault;
      }
      const bool enable_links = block_links_enabled();
      JitCompiler compiler{
          enable_guarded_control_flow_, enable_links ? this : nullptr,
          enable_links ? &Implementation::resolve_block_link : nullptr,
          false,
          enable_links ? &Implementation::resolve_indirect_link : nullptr
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
          ,
          /*persistent_linked_block_abi=*/
          persistent_linked_block_abi_default,
          enable_links ? this : nullptr,
          enable_links ? &Implementation::resolve_direct_link_profile
                       : nullptr,
          enable_links ? &Implementation::resolve_indirect_link_profile
                       : nullptr
#endif
      };
      compiler.set_pointer_authentication_enabled(
          enable_pointer_authentication_);
      const auto compiled =
          compiler.compile(cpu.pc, std::span{instructions}.first(count));
      {
        const auto valid_compilation = [&](const auto &candidate) {
          return !candidate.words.empty() && candidate.guest_instructions != 0 &&
                 candidate.guest_instructions <= instructions.size() &&
                 valid_link_state_metadata(candidate.accesses_fp_state,
                                           candidate.native_fp,
                                           candidate.native_fpmr);
        };
        const auto install_compilation = [&](const auto &candidate,
                                             void *code) {
          const auto internal_entry =
              reinterpret_cast<std::uintptr_t>(code) +
              candidate.internal_entry_word * sizeof(std::uint32_t);
          return &store_native(
              generation, sequences.code_changes, cpu.pc,
              sign_jit_function<JitFunction>(code), internal_entry,
              std::span{instructions}.first(candidate.guest_instructions),
              candidate.memory_write_mask, candidate.direct_memory_write_mask,
              candidate.accesses_fp_state, candidate.link_target_safe,
              candidate.native_fp, candidate.native_fpmr,
              candidate.guest_illegal
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
              ,
              candidate.profile_read_before_write
#endif
          );
        };
        if (!valid_compilation(compiled))
          return Event::fatal;
        if (auto *code = arena_.store(compiled.words); code != nullptr) {
          block = install_compilation(compiled, code);
        } else {
          profile_jit_arena_reset();
          ++cache_validation_diagnostics_.arena_resets;
          clear_cache();
          arena_.reset();

          /*
           * Link and PIC operands are absolute pointers into the descriptor
           * tables. clear_cache() invalidates those tables, so the retry must
           * be freshly lowered against their new storage rather than copying
           * the now-stale first compilation into the reset arena.
           */
          JitCompiler retry_compiler{
              enable_guarded_control_flow_, enable_links ? this : nullptr,
              enable_links ? &Implementation::resolve_block_link : nullptr,
              false,
              enable_links ? &Implementation::resolve_indirect_link : nullptr
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
              ,
              /*persistent_linked_block_abi=*/
              persistent_linked_block_abi_default,
              enable_links ? this : nullptr,
              enable_links ? &Implementation::resolve_direct_link_profile
                           : nullptr,
              enable_links ? &Implementation::resolve_indirect_link_profile
                           : nullptr
#endif
          };
          retry_compiler.set_pointer_authentication_enabled(
              enable_pointer_authentication_);
          const auto retry_compiled = retry_compiler.compile(
              cpu.pc, std::span{instructions}.first(count));
          if (!valid_compilation(retry_compiled))
            return Event::fatal;
          code = arena_.store(retry_compiled.words);
          if (code == nullptr)
            return Event::fatal;
          block = install_compilation(retry_compiled, code);
        }
      }
    }
    if (pending_source)
      train_indirect(*pending_source, *block);
    bool deferred_semantic_instruction = false;
    const auto execute_native = [&](JitFunction function,
                                    bool tracked_stores) -> Event {
      std::unique_ptr<CpuState> expected;
      std::unique_ptr<OverlayMemory> overlay;
      arm64::RunResult expected_result{};
      bool verify =
          verification_active_ && verification_address_enabled(cpu.pc);
      if (verify && block->guest_illegal)
        verify = false;
      for (std::size_t index = 0; verify && index < block->instruction_count;
           ++index) {
        std::uint32_t instruction{};
        if (!memory.fetch(cpu.pc + index * sizeof(instruction),
                          std::as_writable_bytes(std::span{&instruction, 1}))) {
          verify = false;
          break;
        }
        if (!verification_replay_safe(instruction))
          verify = false;
      }
      if (verify) {
        /*
         * Verification is an architectural observer and its reference pass
         * executes under the host environment. Commit any state retained by
         * an earlier translated block before copying CpuState.
         */
        arm64::gadgets::restore_native_fp_environment(cpu);
        expected = std::make_unique<CpuState>(cpu);
        overlay = std::make_unique<OverlayMemory>(memory);
        expected_result = arm64::Dispatcher{}.run(*expected, *overlay,
                                                  block->instruction_count);
      }
      const auto block_pc = cpu.pc;
      if (tracked_stores)
        tracked_store_ranges_.fill({});
      std::uint8_t link_capabilities =
          block->direct_memory_write_mask != 0
              ? link_capability(JitLinkCapability::store_publication)
              : std::uint8_t{0};
      JitChainState chain{
          .stores = tracked_stores ? tracked_store_ranges_.data() : nullptr,
      };
      if (block->accesses_fp_state) {
        cpu.access_fp_state();
        link_capabilities |=
            link_capability(JitLinkCapability::fp_state_materialized);
        if (block->native_fp) {
          activate_native_fp_environment(cpu);
          link_capabilities |=
              link_capability(JitLinkCapability::native_fp_environment);
        }
      }
      if (block->native_fpmr) {
        activate_native_fpmr_environment(cpu);
        link_capabilities |=
            link_capability(JitLinkCapability::native_fpmr_environment);
      }
      chain.link_selector = checked_link_selector(link_capabilities);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
      if (!tracked_stores)
        gpr_link_profile_.record_outer_invocation();
#endif
      const auto result = function(&cpu, &tlb_, &chain);
      if (block->native_fpmr)
        arm64::gadgets::restore_native_fpmr_environment(cpu);
      if (tracked_stores)
        uml_arm64_jit_invalidate_store_ranges(
            reinterpret_cast<const std::uint64_t *>(
                tracked_store_ranges_.data()),
            block->instruction_count);
      const auto current_executed =
          static_cast<std::size_t>(result >> 32);
      const auto event =
          static_cast<Event>(static_cast<std::int32_t>(result & 0xffff'ffffU));
      deferred_semantic_instruction =
          chain.deferred_semantic_instruction != 0U;
      executed = maximum_chain_instructions -
                     static_cast<std::size_t>(chain.remaining_instructions) +
                 current_executed;
      if (event == Event::memory_fault)
        tlb_.discard(tlb_.fault_address);
      /* A cross-page cold path deliberately ends the generated block after
       * its one interpreted instruction. The optimistic reference run may
       * already have consumed later hot-path instructions; compare only
       * when both paths reached the same architectural boundary.
       */
      if (expected)
        arm64::gadgets::restore_native_fp_environment(cpu);
      if (expected && executed == block->instruction_count &&
          expected_result.instructions == executed) {
        /*
         * The reference dispatcher imports deferred FP/SIMD state at its
         * execution boundary, while a native block that does not access that
         * state deliberately leaves the live CpuState lazy. Compare
         * materialized copies so verification observes architectural state
         * without defeating deferred loading on the production path.
         */
        auto compared_expected = *expected;
        auto compared_actual = cpu;
        compared_expected.access_fp_state();
        compared_actual.access_fp_state();
        if (reference_event(expected_result) != event ||
            !same_cpu(compared_expected, compared_actual, block_pc) ||
            !overlay->matches_guest()) {
          std::fprintf(stderr,
                       "Darwin JIT verify failed pc=%#llx count=%zu event=%d/%d "
                       "executed=%zu/%zu instruction=%#x\n",
                       static_cast<unsigned long long>(block_pc),
                       block->instruction_count, static_cast<int>(event),
                       static_cast<int>(reference_event(expected_result)),
                       executed, expected_result.instructions,
                       expected_result.instruction);
          for (std::size_t index = 0; index < block->instruction_count;
               ++index) {
            std::uint32_t instruction{};
            if (memory.fetch(block_pc + index * sizeof(instruction),
                             std::as_writable_bytes(std::span{&instruction, 1})))
              std::fprintf(stderr,
                           "Darwin JIT verify instruction pc=%#llx value=%#x\n",
                           static_cast<unsigned long long>(block_pc + index * 4),
                           instruction);
          }
          std::abort();
        }
      }
      if (event == Event::continued && chain.indirect_miss != nullptr)
        record_pending_indirect(chain.indirect_miss,
                                chain.indirect_source_pc, cpu, generation,
                                sequences.code_changes);
      if (!deferred_semantic_instruction)
        reconcile_reservation(cpu);
      return event;
    };

    if (block->direct_memory_write_mask == 0)
      return execute_native(block->function, false);
    Event event{};
    bool compile_fallback = false;
    {
      JitBlockPublication publication{store_participant_, cpu};
      if (!publication.needs_tracking()) {
        event = execute_native(block->function, false);
      } else {
        const auto tracked = tracked_store_function(*block, memory);
        if (tracked == nullptr)
          compile_fallback = true;
        else
          event = execute_native(tracked, true);
      }
    }
    if (deferred_semantic_instruction || compile_fallback) {
      const auto retired_prefix = executed;
      std::size_t semantic_executed = 0;
      if (deferred_semantic_instruction)
        event = run_deferred_exclusive_instruction(
            cpu, memory, semantic_executed);
      else
        event = run_semantic_instruction(cpu, memory, semantic_executed);
      executed = retired_prefix + semantic_executed;
    }
    reconcile_reservation(cpu);
    return event;
  }

  [[nodiscard]] bool preempted() const noexcept { return preempted_; }
  [[nodiscard]] std::uint64_t fault_address() const noexcept {
    return tlb_.fault_address;
  }
  [[nodiscard]] Access fault_access() const noexcept {
    return tlb_.fault_access;
  }
  [[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept {
    return floating_point_syndrome_;
  }
  [[nodiscard]] JitCacheValidationDiagnostics
  cache_validation_diagnostics() const noexcept {
    return cache_validation_diagnostics_;
  }
  [[nodiscard]] JitIndirectLinkDiagnostics
  indirect_link_diagnostics() const noexcept {
    return indirect_link_diagnostics_;
  }
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  [[nodiscard]] JitGprLinkProfileDiagnostics
  gpr_link_profile_diagnostics() noexcept {
    return gpr_link_profile_.diagnostics();
  }
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
  tracked_store_function(CompiledBlock &block, StaticGuestMemory &memory) {
    if (block.tracked_function != nullptr)
      return block.tracked_function;

    std::array<std::uint32_t, maximum_block_instructions> instructions{};
    for (std::size_t index = 0; index < block.instruction_count; ++index) {
      if (!memory.fetch(
              block.pc + index * sizeof(std::uint32_t),
              std::as_writable_bytes(std::span{&instructions[index], 1})))
        return nullptr;
    }
    JitCompiler compiler{enable_guarded_control_flow_, nullptr, nullptr, true};
    compiler.set_pointer_authentication_enabled(
        enable_pointer_authentication_);
    const auto compiled = compiler.compile(
        block.pc, std::span{instructions}.first(block.instruction_count));
    if (compiled.words.empty() ||
        compiled.guest_instructions != block.instruction_count ||
        !valid_link_state_metadata(compiled.accesses_fp_state,
                                   compiled.native_fp,
                                   compiled.native_fpmr) ||
        compiled.direct_memory_write_mask != block.direct_memory_write_mask)
      return nullptr;

    auto *code = tracked_store_arena_.store(compiled.words);
    if (code == nullptr) {
      clear_tracked_store_cache();
      code = tracked_store_arena_.store(compiled.words);
      if (code == nullptr)
        return nullptr;
    }
    block.tracked_function = sign_jit_function<JitFunction>(code);
    return block.tracked_function;
  }

  void clear_tracked_store_cache() noexcept {
    for (auto &set : *block_cache_)
      for (auto &block : set.ways)
        block.tracked_function = nullptr;
    tracked_store_arena_.reset();
  }

  [[nodiscard]] Event run_floating_exception_mode(
      CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed) {
    std::uint32_t instruction{};
    if (!memory.fetch(
            cpu.pc,
            std::as_writable_bytes(std::span{&instruction, std::size_t{1}}))) {
      const auto fault = memory.last_fault();
      tlb_.fault_address = fault.address;
      tlb_.fault_access = fault.access == arm64::MemoryAccess::write
                              ? Access::write
                              : Access::read;
      return Event::memory_fault;
    }

    const auto guest_fpcr = cpu.fp_loaded ? cpu.fpcr : cpu.deferred_fpcr;
    const bool transactional =
        arm64::detail::needs_floating_exception_transaction(guest_fpcr,
                                                            instruction);
    if (transactional) {
      /*
       * A previous native block may have retained the guest environment in
       * host registers.  Materialize it before taking the isolated snapshot.
       */
      arm64::gadgets::restore_native_fp_environment(cpu);
      cpu.access_fp_state();
    }

    JitCompiler compiler{enable_guarded_control_flow_};
    compiler.set_pointer_authentication_enabled(
        enable_pointer_authentication_);
    const auto compiled =
        compiler.compile(cpu.pc, std::span{&instruction, std::size_t{1}});
    if (compiled.words.empty() || compiled.guest_instructions != 1 ||
        !valid_link_state_metadata(compiled.accesses_fp_state,
                                   compiled.native_fp,
                                   compiled.native_fpmr))
      return Event::fatal;
    floating_transaction_arena_.reset();
    auto *code = floating_transaction_arena_.store(compiled.words);
    if (code == nullptr)
      return Event::fatal;
    const auto function = sign_jit_function<JitFunction>(code);

    const auto invoke = [&](CpuState &target,
                            bool restore_environment) -> Event {
      if (compiled.accesses_fp_state) {
        target.access_fp_state();
        if (compiled.native_fp)
          activate_native_fp_environment(target);
      }
      if (compiled.native_fpmr) {
        activate_native_fpmr_environment(target);
      }
      std::uint8_t link_capabilities =
          compiled.direct_memory_write_mask != 0
              ? link_capability(JitLinkCapability::store_publication)
              : std::uint8_t{0};
      JitChainState chain{};
      if (compiled.accesses_fp_state)
        link_capabilities |=
            link_capability(JitLinkCapability::fp_state_materialized);
      if (compiled.native_fp)
        link_capabilities |=
            link_capability(JitLinkCapability::native_fp_environment);
      if (compiled.native_fpmr)
        link_capabilities |=
            link_capability(JitLinkCapability::native_fpmr_environment);
      chain.link_selector = checked_link_selector(link_capabilities);
      const auto result = function(&target, &tlb_, &chain);
      if (compiled.native_fpmr)
        arm64::gadgets::restore_native_fpmr_environment(target);
      if (restore_environment)
        arm64::gadgets::restore_native_fp_environment(target);
      executed = static_cast<std::size_t>(result >> 32);
      const auto event =
          static_cast<Event>(static_cast<std::int32_t>(result & 0xffff'ffffU));
      if (event == Event::memory_fault)
        tlb_.discard(tlb_.fault_address);
      return event;
    };

    if (transactional) {
      if (compiled.memory_write_mask != 0)
        return Event::fatal;
      const auto original_fpsr = cpu.fpsr;
      auto isolated = cpu;
      isolated.fpsr &=
          ~arm64::detail::fpsr_exception_cumulative_mask;
      isolated.gadget_host_fpcr = 0;
      isolated.gadget_host_fpsr = 0;
      isolated.gadget_fp_active = false;
      isolated.gadget_indirect_miss = nullptr;
      isolated.gadget_host_fpmr = 0;
      isolated.gadget_fpmr_active = false;
      const auto event = invoke(isolated, true);
      if (event != Event::continued)
        return event;
      const auto decision = arm64::detail::decide_floating_exception(
          cpu.fpcr, original_fpsr, isolated.fpsr);
      if (decision.traps()) {
        cpu.fpsr = decision.committed_fpsr;
        floating_point_syndrome_ = decision.syndrome;
        executed = 0;
        return Event::floating_point_trap;
      }
      isolated.fpsr =
          (isolated.fpsr &
           ~arm64::detail::fpsr_exception_cumulative_mask) |
          (decision.committed_fpsr &
           arm64::detail::fpsr_exception_cumulative_mask);
      cpu = isolated;
      reconcile_reservation(cpu);
      return event;
    }

    const auto execute_native = [&] { return invoke(cpu, false); };
    Event event{};
    if (compiled.direct_memory_write_mask == 0) {
      event = execute_native();
    } else {
      JitBlockPublication publication{store_participant_, cpu};
      if (publication.needs_tracking())
        event = run_semantic_instruction(cpu, memory, executed);
      else
        event = execute_native();
    }
    reconcile_reservation(cpu);
    return event;
  }

  void select_cpu(CpuState &cpu) noexcept {
    if (current_cpu_ == &cpu)
      return;
    pending_indirect_ = {};
    if (current_cpu_ != nullptr)
      arm64::gadgets::restore_native_fp_environment(*current_cpu_);
    release_current_reservation();
    current_cpu_ = &cpu;
  }

  void release_current_reservation() noexcept {
    if (current_cpu_ == nullptr ||
        store_participant_.reservation_cpu.load(
            std::memory_order_seq_cst) != current_cpu_)
      return;
    store_participant_.reservation_cpu.store(nullptr,
                                             std::memory_order_seq_cst);
    end_jit_reservation(*current_cpu_);
  }

  void reconcile_reservation(CpuState &cpu) noexcept {
    if (cpu.exclusive_size != 0) {
      store_participant_.reservation_cpu.store(
          &cpu, std::memory_order_seq_cst);
      return;
    }
    if (store_participant_.reservation_cpu.load(
            std::memory_order_seq_cst) != &cpu)
      return;
    store_participant_.reservation_cpu.store(nullptr,
                                             std::memory_order_seq_cst);
    end_jit_reservation(cpu);
  }

  [[nodiscard]] Event run_semantic_instruction(
      CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed) {
    materialize_semantic_cpu_state(cpu);
    std::uint32_t instruction{};
    if (!memory.fetch(
            cpu.pc,
            std::as_writable_bytes(std::span{&instruction, std::size_t{1}}))) {
      const auto fault = memory.last_fault();
      tlb_.fault_address = fault.address;
      tlb_.fault_access = fault.access == arm64::MemoryAccess::write
                              ? Access::write
                              : Access::read;
      return Event::memory_fault;
    }
    JitSemanticMemory semantic{tlb_};
    const auto result = arm64::Dispatcher{}.execute(cpu, semantic, instruction);
    executed = result.instructions;
    if (result.reason == arm64::StopReason::memory_fault) {
      record_semantic_fault(tlb_, semantic, result);
      tlb_.discard(tlb_.fault_address);
    }
    return reference_event(result);
  }

  [[nodiscard]] Event run_deferred_exclusive_instruction(
      CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed) {
    std::uint32_t instruction{};
    if (!memory.fetch(
            cpu.pc,
            std::as_writable_bytes(std::span{&instruction, std::size_t{1}}))) {
      const auto fault = memory.last_fault();
      tlb_.fault_address = fault.address;
      tlb_.fault_access = fault.access == arm64::MemoryAccess::write
                              ? Access::write
                              : Access::read;
      executed = 0;
      return Event::memory_fault;
    }
    const auto result =
        uml_arm64_jit_execute_exclusive(&cpu, &tlb_, instruction);
    executed = static_cast<std::size_t>(result >> 32U);
    const auto event =
        static_cast<Event>(static_cast<std::int32_t>(result & 0xffff'ffffU));
    if (event == Event::memory_fault)
      tlb_.discard(tlb_.fault_address);
    return event;
  }

  [[nodiscard]] void *execution_context(unsigned long generation) {
    if (execution_context_ != nullptr && execution_generation_ == generation)
      return execution_context_;
    darwin_uml_static_vm_execution_context_destroy(execution_context_);
    execution_context_ =
        darwin_uml_static_vm_execution_context_create(generation);
    execution_generation_ = execution_context_ != nullptr ? generation : 0;
    return execution_context_;
  }

  [[nodiscard]] static const void *
  resolve_block_link(void *context, std::uint64_t pc) {
    return static_cast<Implementation *>(context)->link_for(pc);
  }

  [[nodiscard]] static const void *
  resolve_indirect_link(void *context, std::uint64_t source_pc) {
    return static_cast<Implementation *>(context)->indirect_link_for(source_pc);
  }

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  [[nodiscard]] static std::uint64_t *
  resolve_direct_link_profile(void *context, const void *target,
                              std::uint64_t source_pc,
                              std::uint32_t dirty_live_out) {
    return static_cast<Implementation *>(context)
        ->gpr_link_profile_.add_direct(
            static_cast<const JitBlockLink *>(target), source_pc,
            dirty_live_out);
  }

  [[nodiscard]] static jit::JitIndirectLinkProfileCounters
  resolve_indirect_link_profile(void *context, const void *site,
                                std::uint64_t source_pc,
                                std::uint32_t dirty_live_out) {
    return static_cast<Implementation *>(context)
        ->gpr_link_profile_.add_indirect(
            static_cast<const JitIndirectLink *>(site), source_pc,
            dirty_live_out);
  }
#endif

  [[nodiscard]] JitBlockLink *link_for(std::uint64_t pc) {
    return links_.find_or_create(pc);
  }

  [[nodiscard]] JitIndirectLink *
  indirect_link_for(std::uint64_t source_pc) {
    auto *site = indirect_links_.find_or_create(source_pc);
    indirect_link_diagnostics_.capacity_refusals += site == nullptr;
    return site;
  }

  void record_pending_indirect(void *pointer, std::uint64_t source_pc,
                               CpuState &cpu,
                               unsigned long generation,
                               std::uint64_t changes) noexcept {
    /*
     * Resolve the translation-owned source address before touching the
     * generated-code pointer. A miss can therefore never turn a corrupted or
     * stale pointer into a host dereference.
     */
    auto *site = indirect_links_.find(source_pc);
    if (site == nullptr || static_cast<void *>(site) != pointer) {
      pending_indirect_ = {};
      return;
    }
    pending_indirect_ = {
        .source_pc = source_pc,
        .target_pc = cpu.pc,
        .cpu = &cpu,
        .generation = generation,
        .code_changes = changes,
        .link_epoch = link_epoch_,
        .valid = true,
    };
    ++indirect_link_diagnostics_.misses;
  }

  [[nodiscard]] std::optional<std::uint64_t>
  consume_pending_indirect(CpuState &cpu, unsigned long generation,
                           std::uint64_t changes,
                           std::uint64_t target_pc) noexcept {
    const auto pending = pending_indirect_;
    pending_indirect_ = {};
    if (!pending.valid || pending.cpu != &cpu ||
        pending.generation != generation ||
        pending.code_changes != changes ||
        pending.link_epoch != link_epoch_ ||
        pending.target_pc != target_pc) {
      indirect_link_diagnostics_.scope_rejections += pending.valid;
      return std::nullopt;
    }
    return pending.source_pc;
  }

  void train_indirect(std::uint64_t source_pc,
                      const CompiledBlock &target) {
    auto *site = indirect_links_.find(source_pc);
    if (site == nullptr || target.link == nullptr ||
        target.link_epoch != link_epoch_ || !target.link_target_safe) {
      ++indirect_link_diagnostics_.unsafe_target_refusals;
      return;
    }
    if (site->activation_epoch != link_epoch_) {
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
      gpr_link_profile_.drain_indirect_site(*site);
#endif
      indirect_links_.reset(*site);
      site->activation_epoch = link_epoch_;
      active_indirect_links_.push_back(site);
    }
    for (std::size_t index = 0; index < site->entries.size(); ++index) {
      auto &entry = site->entries[index];
      if (entry.guest_pc != target.pc)
        continue;
      entry.link_entries = target.link->entries.data();
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
      site->profile_targets[index] = target.link;
#endif
      ++indirect_link_diagnostics_.trains;
      return;
    }
    const auto replacement = site->next_replacement & 1U;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    gpr_link_profile_.drain_indirect_slot(*site, replacement);
#endif
    auto &entry = site->entries[replacement];
    /*
     * Publish the host-owned entry table before the comparison key. Execution
     * is single-threaded per executor, but this order also prevents a matching
     * key from ever exposing an uninitialized pointer during diagnostics.
     */
    entry.link_entries = target.link->entries.data();
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    site->profile_targets[replacement] = target.link;
#endif
    entry.guest_pc = target.pc;
    site->next_replacement ^= 1U;
    ++indirect_link_diagnostics_.trains;
  }

  void deactivate_links() noexcept {
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    gpr_link_profile_.drain_direct();
#endif
    for (auto *link : active_links_) {
      link->entries.fill(0);
      link->instruction_count = 0;
      link->activation_epoch = 0;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
      link->profile_mask_valid = false;
#endif
    }
    active_links_.clear();
  }

  void deactivate_indirect_links() noexcept {
    indirect_link_diagnostics_.deactivated_sites +=
        active_indirect_links_.size();
    for (auto *site : active_indirect_links_) {
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
      gpr_link_profile_.drain_indirect_site(*site);
#endif
      indirect_links_.reset(*site);
    }
    active_indirect_links_.clear();
  }

  void prepare_links(unsigned long generation, std::uint64_t changes) {
    if (link_generation_ != generation || link_code_changes_ != changes) {
      if (link_sequence_initialized_)
        profile_jit_sequence_transition(link_generation_ != generation,
                                        compiled_in_link_sequence_);
      else
        link_sequence_initialized_ = true;
      compiled_in_link_sequence_ = 0;
      pending_indirect_ = {};
      deactivate_indirect_links();
      deactivate_links();
      ++link_epoch_;
      link_generation_ = generation;
      link_code_changes_ = changes;
    }
  }

  [[gnu::noinline]] void refresh_link(CompiledBlock &block) {
    if (block.link == nullptr) {
      block.link_epoch = link_epoch_;
      return;
    }
    auto &link = *block.link;
    link.instruction_count = block.instruction_count;
    link.entries.fill(0);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    link.profile_mask_valid = false;
#endif
    /*
     * Verification ranges are translation diagnostics. Keep links for code
     * outside the selected range (notably kernel boot), but force every
     * selected target through run_block so its native result is checked.
     * With the default all-address range this retains the original
     * link-disabled verifier behavior.
     */
    if (block.link_target_safe &&
        !(verification_active_ &&
          verification_address_enabled(block.pc))) {
      const auto requirements =
          link_state_requirements(block.accesses_fp_state, block.native_fp,
                                  block.native_fpmr);
      const auto publication =
          link_capability(JitLinkCapability::store_publication);
      for (std::size_t index = 0; index < link.entries.size(); ++index) {
        const auto capabilities = jit_link_selector_capabilities[index];
        if ((capabilities & requirements) != requirements ||
            (block.direct_memory_write_mask != 0 &&
             (capabilities & publication) == 0U))
          continue;
        link.entries[index] = block.internal_entry;
      }
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
      link.profile_read_before_write = block.profile_read_before_write;
      link.profile_mask_valid = true;
#endif
      if (link.activation_epoch != link_epoch_) {
        link.activation_epoch = link_epoch_;
        active_links_.push_back(&link);
      }
    }
    block.link_epoch = link_epoch_;
  }

  [[gnu::always_inline]] void activate_link(CompiledBlock &block) {
    if (block.link_epoch != link_epoch_)
      refresh_link(block);
  }

  [[nodiscard]] JitCacheValidationResult validate_stale_block(
      const CompiledBlock &block, const SavedGuestWords &guest_words,
      std::uint64_t changes,
      StaticGuestMemory &memory) {
    if (block.instruction_count == 0 ||
        block.instruction_count > guest_words.size())
      return JitCacheValidationResult::mismatch;
    for (std::size_t index = 0; index < block.instruction_count; ++index) {
      std::array<std::byte, sizeof(std::uint32_t)> current{};
      if (!memory.fetch(block.pc + index * sizeof(std::uint32_t), current))
        return JitCacheValidationResult::fault;
      if (std::memcmp(current.data(), &guest_words[index],
                      current.size()) != 0)
        return JitCacheValidationResult::mismatch;
    }

    /*
     * The exact-byte proof is useful only for the sequence whose transition
     * caused this lookup. A concurrent code publication after the first
     * sequence snapshot must leave the old link inactive and retry through
     * the normal miss path.
     */
    const auto current =
        darwin_uml_static_vm_execution_context_sequences(execution_context_);
    if (current.execution_interrupt != 0 ||
        current.mapping_changes != mapping_changes_ ||
        current.code_changes != changes)
      return JitCacheValidationResult::sequence_race;
    return JitCacheValidationResult::hit;
  }

  [[nodiscard]] CompiledBlock *find(unsigned long generation,
                                    std::uint64_t changes, std::uint64_t pc,
                                    StaticGuestMemory &memory) {
    const auto set_index = cache_index(generation, pc);
    auto &set = (*block_cache_)[set_index];
    auto &saved = (*saved_guest_words_)[set_index];
    CompiledBlock *stale{};
    std::size_t stale_way{};
    for (std::size_t way = 0; way < set.ways.size(); ++way) {
      auto &block = set.ways[way];
      if (block.function != nullptr && block.generation == generation &&
          block.code_changes == changes && block.pc == pc) {
        block.stamp = ++stamp_;
        activate_link(block);
        return &block;
      }
      if (block.function != nullptr && block.generation == generation &&
          block.pc == pc &&
          (stale == nullptr || block.stamp > stale->stamp)) {
        stale = &block;
        stale_way = way;
      }
    }
    if (!reuse_validated_stale_blocks_ || stale == nullptr)
      return nullptr;

    ++cache_validation_diagnostics_.validation_attempts;
    const auto validation =
        validate_stale_block(*stale, saved.ways[stale_way], changes, memory);
    profile_jit_cache_validation(validation);
    switch (validation) {
    case JitCacheValidationResult::hit:
      ++cache_validation_diagnostics_.validation_hits;
      stale->code_changes = changes;
      stale->stamp = ++stamp_;
      ++compiled_in_link_sequence_;
      activate_link(*stale);
      return stale;
    case JitCacheValidationResult::mismatch:
      ++cache_validation_diagnostics_.validation_mismatches;
      *stale = {};
      break;
    case JitCacheValidationResult::fault:
      ++cache_validation_diagnostics_.validation_faults;
      *stale = {};
      break;
    case JitCacheValidationResult::sequence_race:
      ++cache_validation_diagnostics_.validation_sequence_races;
      break;
    }
    return nullptr;
  }

  [[nodiscard]] CacheSlot replacement(unsigned long generation,
                                      std::uint64_t changes,
                                      std::uint64_t pc) {
    const auto set_index = cache_index(generation, pc);
    auto &set = (*block_cache_)[set_index];
    std::size_t selected_way = 0;
    for (std::size_t way = 0; way < set.ways.size(); ++way)
      if (set.ways[way].function == nullptr ||
          set.ways[way].stamp < set.ways[selected_way].stamp)
        selected_way = way;
    auto *selected = &set.ways[selected_way];
    const bool eviction = selected->function != nullptr;
    profile_jit_cache_replacement(eviction);
    cache_validation_diagnostics_.cache_replacements += eviction;
    *selected = {};
    selected->generation = generation;
    selected->code_changes = changes;
    selected->pc = pc;
    selected->stamp = ++stamp_;
    return {
        .block = selected,
        .guest_words =
            &(*saved_guest_words_)[set_index].ways[selected_way],
    };
  }

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
  ) {
    if (!valid_link_state_metadata(accesses_fp_state, native_fp,
                                   native_fpmr))
      std::abort();
    const auto selected = replacement(generation, changes, pc);
    auto &block = *selected.block;
    block.function = function;
    block.internal_entry = internal_entry;
    block.link = link_for(pc);
    block.instruction_count = guest_words.size();
    std::copy(guest_words.begin(), guest_words.end(),
              selected.guest_words->begin());
    block.memory_write_mask = memory_write_mask;
    block.direct_memory_write_mask = direct_memory_write_mask;
    block.accesses_fp_state = accesses_fp_state;
    block.link_target_safe = link_target_safe;
    block.native_fp = native_fp;
    block.native_fpmr = native_fpmr;
    block.guest_illegal = guest_illegal;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    block.profile_read_before_write = profile_read_before_write;
#endif
    ++cache_validation_diagnostics_.translations;
    ++compiled_in_link_sequence_;
    activate_link(block);
    return block;
  }

  void clear_cache() noexcept {
    for (auto &set : *block_cache_)
      set = {};
    deactivate_indirect_links();
    deactivate_links();
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    gpr_link_profile_.clear_code();
#endif
    links_.clear();
    indirect_links_.clear();
    pending_indirect_ = {};
    tracked_store_arena_.reset();
    link_generation_ = 0;
    link_code_changes_ = 0;
    compiled_in_link_sequence_ = 0;
    link_sequence_initialized_ = false;
  }

  std::unique_ptr<BlockCache> block_cache_{std::make_unique<BlockCache>()};
  std::unique_ptr<SavedGuestWordCache> saved_guest_words_{
      std::make_unique_for_overwrite<SavedGuestWordCache>()};
  JitBlockLink empty_link_;
  JitBlockLinkTable links_;
  JitIndirectLinkTable indirect_links_;
  std::vector<JitBlockLink *> active_links_;
  std::vector<JitIndirectLink *> active_indirect_links_;
  JitArena arena_;
  JitArena tracked_store_arena_{tracked_store_arena_size};
  JitArena floating_transaction_arena_{floating_transaction_arena_size};
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
  JitCacheValidationDiagnostics cache_validation_diagnostics_{};
  JitIndirectLinkDiagnostics indirect_link_diagnostics_{};
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  JitGprLinkProfiler gpr_link_profile_{};
#endif
  PendingIndirect pending_indirect_{};
  bool preempted_{};
  bool enable_guarded_control_flow_{};
  bool enable_pointer_authentication_{true};
  bool reuse_validated_stale_blocks_{};
  bool link_sequence_initialized_{};
  bool verification_active_{};
};

JitExecutor::JitExecutor(bool enable_guarded_control_flow)
    : JitExecutor{JitExecutorOptions{
          .enable_guarded_control_flow = enable_guarded_control_flow}} {}

JitExecutor::JitExecutor(JitExecutorOptions options)
    : implementation_{std::make_unique<Implementation>(options)} {}

JitExecutor::~JitExecutor() = default;

bool JitExecutor::begin_execution(unsigned long generation,
                                  std::uint64_t guest_pc) {
  return implementation_->begin_execution(generation, guest_pc);
}

void JitExecutor::end_execution() noexcept { implementation_->end_execution(); }

Event JitExecutor::run_block(CpuState &cpu, StaticGuestMemory &memory,
                             unsigned long generation, std::size_t &executed) {
  return implementation_->run_block(cpu, memory, generation, executed);
}

bool JitExecutor::preempted() const noexcept {
  return implementation_->preempted();
}

std::uint64_t JitExecutor::fault_address() const noexcept {
  return implementation_->fault_address();
}

Access JitExecutor::fault_access() const noexcept {
  return implementation_->fault_access();
}

std::uint32_t JitExecutor::floating_point_syndrome() const noexcept {
  return implementation_->floating_point_syndrome();
}

JitCacheValidationDiagnostics
JitExecutor::cache_validation_diagnostics() const noexcept {
  return implementation_->cache_validation_diagnostics();
}

JitIndirectLinkDiagnostics
JitExecutor::indirect_link_diagnostics() const noexcept {
  return implementation_->indirect_link_diagnostics();
}

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
JitGprLinkProfileDiagnostics
JitExecutor::gpr_link_profile_diagnostics() noexcept {
  return implementation_->gpr_link_profile_diagnostics();
}

JitGprLinkProfileDiagnostics
global_gpr_link_profile_diagnostics() noexcept {
  return JitGprLinkProfileGlobal::instance().diagnostics();
}

void report_global_gpr_link_profile() noexcept {
  JitGprLinkProfileGlobal::instance().report();
}
#endif

bool native_jit_available() noexcept {
  static const bool available = [] {
#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT)
    void *mapping =
        mmap(nullptr, 16U * 1024U, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (mapping == MAP_FAILED)
      return false;
    (void)munmap(mapping, 16U * 1024U);
    return true;
#else
    return false;
#endif
  }();
  return available;
}

} // namespace uml::backend

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
extern "C" void uml_backend_report_jit_gpr_link_profile(void) {
  uml::backend::report_global_gpr_link_profile();
}
#endif
