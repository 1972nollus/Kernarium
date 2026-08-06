// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_runtime_contract.hpp"
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
#include "jit_gpr_pressure_profile.hpp"
#endif

#include <cstdint>
#include <span>

namespace uml::cpu::arm64 {

enum class JitCacheValidationResult {
  hit,
  mismatch,
  fault,
  sequence_race,
};

#if UML_ENABLE_JIT_PROFILE
void profile_jit_compilation(
    std::uint64_t pc, std::span<const std::uint32_t> guest_words,
    std::span<const std::uint32_t> host_words, std::size_t hot_words,
    bool tracked
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
    ,
    const jit::JitGprPressureProfile &gpr_pressure
#endif
);
void profile_jit_cache_replacement(bool eviction) noexcept;
void profile_jit_arena_reset() noexcept;
void profile_jit_sequence_transition(bool generation_changed,
                                     std::uint64_t discarded_blocks) noexcept;
void profile_jit_cache_validation(JitCacheValidationResult result) noexcept;
#else
inline constexpr void profile_jit_compilation(
    std::uint64_t, std::span<const std::uint32_t>,
    std::span<const std::uint32_t>, std::size_t, bool
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
    ,
    const jit::JitGprPressureProfile &
#endif
) noexcept {}
inline constexpr void profile_jit_cache_replacement(bool) noexcept {}
inline constexpr void profile_jit_arena_reset() noexcept {}
inline constexpr void profile_jit_sequence_transition(
    bool, std::uint64_t) noexcept {}
inline constexpr void profile_jit_cache_validation(
    JitCacheValidationResult) noexcept {}
#endif

} // namespace uml::cpu::arm64
