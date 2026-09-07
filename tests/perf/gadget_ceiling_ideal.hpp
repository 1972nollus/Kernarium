// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gadget_ceiling_workload.hpp"

#include <cstdint>
#include <span>

/* Translate the body once with the production Builder and map the guest
 * buffer through the static MMU. Runs outside every timed region. */
void uml_gadget_ceiling_prepare_ideal(std::span<const std::uint32_t> body);
/* Zero the guest buffer so the next run starts from the native control's
 * initial state. */
void uml_gadget_ceiling_ideal_reset();
[[nodiscard]] UmlGadgetCeilingResult uml_gadget_ceiling_ideal_workload(
	std::uint64_t iterations);
