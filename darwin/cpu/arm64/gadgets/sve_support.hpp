// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/sve_profile.hpp"

namespace uml::cpu::arm64::gadgets {

/* How much of the non-streaming SVE profile the gadget engine implements:
 * all of it, the generated forms as gadgets (program_builder_sve.cpp) and
 * every other SVE word outside streaming mode through the shared semantic
 * library (sve_helpers.cpp). Below complete the engine would present no SVE
 * to its guest (sve_host_for_support). */
inline constexpr SveSupport sve_support = SveSupport::complete;

} // namespace uml::cpu::arm64::gadgets
