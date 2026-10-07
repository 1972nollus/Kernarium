// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/sve_profile.hpp"

namespace uml::cpu::arm64::gadgets {

/* How much of the non-streaming SVE profile the gadget engine implements:
 * the control core (program_builder_sve.cpp), with every other SVE word
 * outside streaming mode the illegal-instruction exit. Below complete the
 * engine presents no SVE to its guest (sve_host_for_support). */
inline constexpr SveSupport sve_support = SveSupport::control;

} // namespace uml::cpu::arm64::gadgets
