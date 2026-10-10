// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/a32_profile.hpp"

namespace uml::cpu::arm64::gadgets {

/* How much of the AArch32 EL0 profile the gadget engine implements: the
 * integer core (A32 and T32 integer, IT, branches and interworking, memory,
 * exclusives and the system instructions). An AArch32 entry
 * (CpuEngine::run_aarch32) runs it through run_translated_aarch32_blocks
 * (engine_execution.hpp); with the development policy off the engine still
 * presents no AArch32 to its guest (a32_presented_support). The SIMD and
 * complete levels are the later gadget AArch32 milestones. */
inline constexpr A32Support a32_support = A32Support::base;

} // namespace uml::cpu::arm64::gadgets
