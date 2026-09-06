// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "scalar_forwarding.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#ifndef UML_GADGET_DEPENDENCY_COVERAGE
#define UML_GADGET_DEPENDENCY_COVERAGE 0
#endif

namespace uml::cpu::arm64::gadgets::diagnostics {

/* Slot zero counts guest words in entered decode groups (not retired words:
 * a fault may stop such a group). Slot one counts unsupported groups. Other
 * slots count eligible groups by scalar/SIMD shape and adjacent-source mask.
 * Each entered group occupies exactly one bucket; the two catalogs are
 * disjoint, so scalar and vector totals add without double-counting.
 * Counting happens in the threaded stream, including linked entries and only
 * the executed prefix of inlined branches. Ordinary builds have no counters.
 */
inline constexpr unsigned vector_coverage_family = scalar_family_names.size();
inline constexpr std::size_t dependency_counter_count = 2 + (scalar_family_names.size() + 1) * 32;
using DependencyCounts = std::array<std::uint64_t, dependency_counter_count>;

#if UML_GADGET_DEPENDENCY_COVERAGE
[[nodiscard]] void *dependency_counters();
[[nodiscard]] DependencyCounts dependency_snapshot();
extern "C" void uml_arm64_gadget_dependency_coverage();
#endif

} // namespace uml::cpu::arm64::gadgets::diagnostics
