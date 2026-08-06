// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_executor.hpp"
#include "jit_arena.hpp"
#include "jit_compiler_families.hpp"
#include "jit_runtime_contract.hpp"
#ifndef UML_JIT_TEST_SUPPORT
#define UML_JIT_TEST_SUPPORT 0
#endif
#if UML_JIT_TEST_SUPPORT
#include "jit_test_api.hpp"
#endif

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
#include "cpu/arm64/fp_environment.hpp"
#include "memory/tlb.hpp"
#include "cpu/host_capabilities.hpp"
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
#if UML_JIT_PUBLICATION_GATE_TESTING
#include <thread>
#endif
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif

extern "C" std::uint32_t uml_arm64_simd_structure_memory_transfer(
    uml::cpu::arm64::CpuState *cpu,
    uml::memory::Tlb *tlb, std::uint64_t tagged_address,
    void *data, std::size_t size, std::uint32_t access, std::uint32_t base_sp,
    std::uint32_t options) noexcept;

extern "C" void uml_arm64_jit_invalidate_store_ranges(
    const std::uint64_t *ranges, std::size_t count) noexcept;

extern "C" void
uml_arm64_guest_code_synchronize(std::uint64_t address) noexcept;

namespace uml::cpu::arm64 {
using ::uml::memory::StaticGuestMemory;
using ::uml::cpu::ArmFeature;
using ::uml::cpu::ArmFeatureSet;
using ::uml::cpu::host_capabilities;
using arm64::CpuState;
using ::uml::memory::Access;
using ::uml::cpu::EngineEvent;
using Event = EngineEvent;
using ::uml::memory::Tlb;

constexpr std::size_t maximum_block_instructions = 64;
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
checked_link_selector(std::uint8_t capabilities) noexcept;

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
  explicit JitBlockLinkTable(std::size_t capacity);
  [[nodiscard]] JitBlockLink *find_or_create(std::uint64_t pc);
  void clear() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;

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
  JitIndirectLinkTable(std::size_t capacity,
                       const JitBlockLink *empty_link);
  [[nodiscard]] JitIndirectLink *find_or_create(std::uint64_t source_pc);
  [[nodiscard]] JitIndirectLink *find(
      std::uint64_t source_pc) const noexcept;
  void reset(JitIndirectLink &site) const noexcept;
  void clear() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;

private:
  std::size_t capacity_{};
  const JitBlockLink *empty_link_{};
  std::unordered_map<std::uint64_t, std::unique_ptr<JitIndirectLink>> entries_;
};

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

} // namespace uml::cpu::arm64
