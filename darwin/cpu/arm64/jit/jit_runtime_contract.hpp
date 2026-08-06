// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/engine.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64 {

using Event = ::uml::cpu::EngineEvent;

inline constexpr std::size_t maximum_chain_instructions = 4096;
/* Translation-time only: generated blocks select one ABI shape. */
inline constexpr bool persistent_linked_block_abi_default = true;

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
