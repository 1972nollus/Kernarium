// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/engine.hpp"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64 {

using Event = ::uml::cpu::EngineEvent;

inline constexpr std::size_t maximum_block_instructions = 64;
/* The block cache, link tables, and arena are one coupled resource sized from
 * the process budget. Arena exhaustion clears the block cache. See
 * jit_cache_geometry.hpp.
 */
inline constexpr std::size_t cache_ways = 4;
inline constexpr std::size_t single_instruction_arena_size = 16U * 1024U;
inline constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr unsigned page_shift = DARWIN_UML_GUEST_PAGE_SHIFT;
inline constexpr std::uint32_t native_fpsr_mutable_mask = 0x0800'009fU;
inline constexpr std::size_t maximum_chain_instructions = 4096;
/* Translation-time only: generated blocks select one ABI shape. */
inline constexpr bool persistent_linked_block_abi_default = true;

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

[[nodiscard]] constexpr std::int64_t
sign_extend(std::uint64_t value, unsigned bits) noexcept {
  const auto shift = 64U - bits;
  return static_cast<std::int64_t>(value << shift) >> shift;
}

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

/*
 * A translation has two executor-owned link operands: a direct successor key
 * whose mutable descriptor is regenerated beside the installed code, and an
 * indirect-link site. Canonical shared words zero both pointer cells and retain
 * only their neutral guest keys. Recording the cells lets every executor
 * recreate its private link state while reusing decode and emission work.
 */
/* The guest-GPR link profiler also embeds executor-owned counter pointers.
 * Those diagnostic pointers are intentionally not part of the production
 * relocation ABI, so a profiling build must keep translations private to the
 * executor that owns the counters.
 */
#ifndef UML_JIT_SHARE_TRANSLATIONS_BETWEEN_EXECUTORS
#define UML_JIT_SHARE_TRANSLATIONS_BETWEEN_EXECUTORS 1
#endif

inline constexpr bool share_translations_between_executors =
    UML_JIT_SHARE_TRANSLATIONS_BETWEEN_EXECUTORS != 0;

enum class JitRelocationKind : std::uint8_t {
  direct_link,
  indirect_link,
};

struct JitRelocation final {
  /* Index of the low half of the 64-bit literal pool slot. */
  std::size_t word{};
  /* Guest PC the pointer is resolved from: the branch target for a direct
   * link, the branch source for an indirect one.
   */
  std::uint64_t key{};
  JitRelocationKind kind{};
};

struct JitStoreRange final {
  /* The tracked-only program records the translated backing address so
   * distinct guest virtual aliases share one reservation identity.
   */
  std::uint64_t address;
  std::uint64_t size;
};

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
   * publication is alive. Generated code records the translated exclusive
   * instruction here and returns without retiring it; run_block executes that
   * exact word after the publication scope has ended. Zero (UDF #0) is never
   * deferred and means that no instruction is pending.
   */
  std::uint32_t deferred_exclusive_instruction{};
  std::uint64_t indirect_source_pc{};
};

static_assert(offsetof(JitChainState, remaining_instructions) == 0);
static_assert(offsetof(JitChainState, stores) == 8);
static_assert(offsetof(JitChainState, indirect_miss) == 16);
static_assert(offsetof(JitChainState, link_selector) == 24);
static_assert(offsetof(JitChainState, deferred_exclusive_instruction) == 28);
static_assert(offsetof(JitChainState, indirect_source_pc) == 32);
static_assert(sizeof(JitChainState) == 40);
static_assert(sizeof(JitStoreRange) == 16);
static_assert(sizeof(JitChainState::link_selector) == 1);

} // namespace uml::cpu::arm64
