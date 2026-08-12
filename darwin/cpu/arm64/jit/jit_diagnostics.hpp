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
    bool tracked, std::size_t direct_link_refusals,
    std::size_t indirect_link_refusals
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
    ,
    const jit::JitGprPressureProfile &gpr_pressure
#endif
);
void profile_jit_cache_replacement(bool eviction) noexcept;
void profile_jit_arena_reset() noexcept;
void profile_jit_link_table_state(
    std::size_t direct_size, std::size_t direct_capacity,
    std::size_t indirect_size, std::size_t indirect_capacity) noexcept;
void profile_jit_target_link_refusal(bool refused) noexcept;
void profile_jit_sequence_transition(bool generation_changed,
                                     std::uint64_t discarded_blocks) noexcept;
void profile_jit_cache_validation(JitCacheValidationResult result) noexcept;
[[nodiscard]] std::uint64_t jit_profile_timestamp_ns() noexcept;
void profile_jit_local_compile_latency(std::uint64_t nanoseconds) noexcept;
void profile_jit_shared_materialization_latency(
    std::uint64_t nanoseconds) noexcept;
/* Called only after translation is quiescent and task carriers have retired.
 * It drains live thread-local recorders and emits each enabled cumulative
 * report whose state changed since the preceding call.
 */
void report_final_jit_diagnostics() noexcept;
#else
inline constexpr void profile_jit_compilation(
    std::uint64_t, std::span<const std::uint32_t>,
    std::span<const std::uint32_t>, std::size_t, bool, std::size_t,
    std::size_t
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
    ,
    const jit::JitGprPressureProfile &
#endif
) noexcept {}
inline constexpr void profile_jit_cache_replacement(bool) noexcept {}
inline constexpr void profile_jit_arena_reset() noexcept {}
inline constexpr void profile_jit_link_table_state(
    std::size_t, std::size_t, std::size_t, std::size_t) noexcept {}
inline constexpr void profile_jit_target_link_refusal(bool) noexcept {}
inline constexpr void profile_jit_sequence_transition(
    bool, std::uint64_t) noexcept {}
inline constexpr void profile_jit_cache_validation(
    JitCacheValidationResult) noexcept {}
[[nodiscard]] inline constexpr std::uint64_t
jit_profile_timestamp_ns() noexcept { return 0; }
inline constexpr void profile_jit_local_compile_latency(
    std::uint64_t) noexcept {}
inline constexpr void profile_jit_shared_materialization_latency(
    std::uint64_t) noexcept {}
#endif

} // namespace uml::cpu::arm64
