// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)

#include <array>
#include <cstddef>

namespace uml::cpu::arm64::jit {

inline constexpr std::array<std::size_t, 3>
    jit_gpr_pressure_candidate_capacities{10U, 12U, 15U};

/*
 * Translation-only diagnostics for one compiled block.  The avoided-access
 * estimates model the current first-free allocator with the named capacity:
 * the first required import and the final dirty publication still count,
 * while repeated fallback loads and intermediate stores do not.
 */
struct JitGprPressureProfile final {
  std::size_t slot_high_water{};
  std::size_t allocation_refusals{};
  std::size_t fallback_read_loads{};
  std::size_t fallback_write_stores{};
  std::size_t repeated_overflow_accesses{};
  std::array<std::size_t, jit_gpr_pressure_candidate_capacities.size()>
      estimated_avoided_accesses{};

  friend bool operator==(const JitGprPressureProfile &,
                         const JitGprPressureProfile &) = default;
};

} // namespace uml::cpu::arm64::jit

#endif
