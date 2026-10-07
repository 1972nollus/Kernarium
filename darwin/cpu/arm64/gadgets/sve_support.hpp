// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/sve_profile.hpp"

namespace uml::cpu::arm64::gadgets {

/* How much of the non-streaming SVE profile the gadget engine implements:
 * none of it yet. Every SVE word outside streaming mode is the
 * illegal-instruction exit, and the engine presents no SVE to its guest
 * (sve_host_for_support). */
inline constexpr SveSupport sve_support = SveSupport::none;

} // namespace uml::cpu::arm64::gadgets
