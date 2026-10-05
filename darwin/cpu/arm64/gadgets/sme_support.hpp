// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/sme_profile.hpp"

namespace uml::cpu::arm64::gadgets {

/* How much of the SME profile the gadget engine implements: the control
 * core. SMSTART, SMSTOP and the SVCR and TPIDR2_EL0 accesses, RDSVL, ADDSVL
 * and ADDSPL, and the ZA and ZT0 loads, stores and zeroings run in every mode
 * the profile allows; in streaming mode so do the base integer, branch and
 * system instructions whose gadgets are safe there. Every other SVE and SME
 * word, and every instruction without a streaming form in streaming mode,
 * is the illegal-instruction exit. */
inline constexpr SmeSupport sme_support = SmeSupport::control;

} // namespace uml::cpu::arm64::gadgets
