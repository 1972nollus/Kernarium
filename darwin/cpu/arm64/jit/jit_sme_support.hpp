// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/sme_profile.hpp"

namespace uml::cpu::arm64::jit {

/* How much of the SME profile the JIT implements. None yet: every SVE and
 * SME word is the illegal-instruction exit, whatever the host. */
inline constexpr SmeSupport sme_support = SmeSupport::none;

} // namespace uml::cpu::arm64::jit
