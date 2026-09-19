// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../dispatcher.hpp"
#include "instruction_primitives.hpp"

#include <cstdint>

/* Interpreter result helpers layered on the engine-neutral primitives. */

namespace uml::cpu::arm64::detail {

constexpr RunResult continue_after(std::uint64_t address,
				   std::uint32_t instruction)
{
	return {StopReason::continued, address, instruction, 1, false};
}

} // namespace uml::cpu::arm64::detail
