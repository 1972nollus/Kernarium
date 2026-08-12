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
/* The block cache, its link tables and the arena are one coupled resource,
 * and their sizes are now derived per executor from a whole-process budget
 * rather than fixed here. See jit_cache_geometry.hpp.
 *
 * A multi-core Clang plus Object Detection campaign translates 283,353
 * distinct blocks, and a cache holding a fraction of that collapses into
 * retranslation rather than degrading gently: at the former 8192 sets it held
 * 32,768 entries and thrashed into roughly 385 translations per distinct
 * block. Growing either the cache or the arena alone is not enough, and the
 * arena alone is actively harmful, because arena exhaustion calls
 * clear_cache(). See G133 and G135 in docs/HOT_PATH_EXPERIMENTS.md.
 */
inline constexpr std::size_t cache_ways = 4;
inline constexpr std::size_t floating_transaction_arena_size = 16U * 1024U;
inline constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr unsigned page_shift = DARWIN_UML_GUEST_PAGE_SHIFT;
inline constexpr std::uint32_t native_fpsr_mutable_mask = 0x0800'009fU;
inline constexpr std::size_t maximum_chain_instructions = 4096;
/* Translation-time only: generated blocks select one ABI shape. */
inline constexpr bool persistent_linked_block_abi_default = true;

/*
 * Experimental exact A/B arm for the fixed cross-block guest-GPR bank.
 * Non-CMake consumers retain the control shape unless they opt in explicitly.
 */
#ifndef UML_JIT_CARRY_X8_X9_ACROSS_LINKS
#define UML_JIT_CARRY_X8_X9_ACROSS_LINKS 0
#endif
#if UML_JIT_CARRY_X8_X9_ACROSS_LINKS && \
    defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
#error "The fixed linked GPR bank is incompatible with GPR link profiling"
#endif
inline constexpr bool carry_x8_x9_across_links =
    UML_JIT_CARRY_X8_X9_ACROSS_LINKS != 0;

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
 * A translation embeds two kinds of pointer that belong to the executor that
 * compiled it: the successor's JitBlockLink entry array and the indirect link
 * site. Everything else it embeds is either translation-owned or a static host
 * function address, which are identical in every executor. Recording where
 * those two land lets one executor reuse another's translation by rewriting
 * the pointers for its own link tables, so the expensive decode and emit
 * happens once per distinct block instead of once per executor.
 */
/* The guest-GPR link profiler also embeds executor-owned counter pointers.
 * Those diagnostic pointers are intentionally not part of the production
 * relocation ABI, so a profiling build must keep translations private to the
 * executor that owns the counters.
 */
#ifndef UML_JIT_SHARE_TRANSLATIONS_BETWEEN_EXECUTORS
#define UML_JIT_SHARE_TRANSLATIONS_BETWEEN_EXECUTORS 1
#endif

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
inline constexpr bool share_translations_between_executors = false;
#else
inline constexpr bool share_translations_between_executors =
    UML_JIT_SHARE_TRANSLATIONS_BETWEEN_EXECUTORS != 0;
#endif

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
   * publication is alive. Generated code sets this byte and returns without
   * retiring the instruction; run_block performs the semantic instruction
   * after the publication scope has ended.
   */
  std::uint8_t deferred_semantic_instruction{};
  std::uint64_t indirect_source_pc{};
};

static_assert(offsetof(JitChainState, remaining_instructions) == 0);
static_assert(offsetof(JitChainState, stores) == 8);
static_assert(offsetof(JitChainState, indirect_miss) == 16);
static_assert(offsetof(JitChainState, link_selector) == 24);
static_assert(offsetof(JitChainState, indirect_source_pc) == 32);
static_assert(sizeof(JitChainState) == 40);
static_assert(sizeof(JitStoreRange) == 16);
static_assert(sizeof(JitChainState::link_selector) == 1);

} // namespace uml::cpu::arm64
