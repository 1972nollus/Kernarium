// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/a32_profile.hpp"

namespace uml::cpu::arm64::gadgets {

/* How much of the AArch32 EL0 profile the gadget engine implements: the
 * integer core (A32 and T32 integer, IT, branches and interworking, memory,
 * exclusives and the system instructions), VFPv4-D32 and Advanced SIMD, and
 * the Armv8 additions and optional extensions (the complete level). An
 * AArch32 entry (CpuEngine::run_aarch32) runs it through
 * run_translated_aarch32_blocks (engine_execution.hpp). */
inline constexpr A32Support a32_support = A32Support::complete;

} // namespace uml::cpu::arm64::gadgets
