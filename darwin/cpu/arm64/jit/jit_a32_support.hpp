// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/a32_profile.hpp"

namespace uml::cpu::arm64::jit {

/* How much of the AArch32 EL0 profile the JIT implements: the integer core
 * (A32 and T32 data processing, memory, exclusives, branches and
 * interworking, IT, the system forms) and VFPv4-D32 with Advanced SIMD (the
 * simd level), through the entry that a32/a32_loop.hpp provides. The engine
 * presents exactly this to its guest when the development policy is on
 * (a32_presented_support). */
inline constexpr A32Support a32_support = A32Support::simd;

} // namespace uml::cpu::arm64::jit
