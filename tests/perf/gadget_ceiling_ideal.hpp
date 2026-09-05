// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

[[nodiscard]] std::uint64_t uml_gadget_ceiling_ideal_workload(
	std::uint64_t iterations);
void uml_gadget_ceiling_prepare_ideal();
