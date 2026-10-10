// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/a32_profile.hpp"

namespace uml::cpu::arm64::jit {

/* How much of the AArch32 EL0 profile the JIT implements: none. An AArch32
 * entry (CpuEngine::run_aarch32) is the illegal-instruction exit, so the
 * engine presents no AArch32 to its guest whatever the development policy
 * says (a32_presented_support). Raising the level is the work of the JIT
 * AArch32 milestones, which replace the entry (engine_execution.hpp,
 * run_translated_aarch32) in the same change. */
inline constexpr A32Support a32_support = A32Support::none;

} // namespace uml::cpu::arm64::jit
