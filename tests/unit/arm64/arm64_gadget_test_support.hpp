// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* What the gadget-structure tests compare a built program's words against:
 * a gadget's entry address and a vector register's CpuState offset. */

#include "cpu/arm64/cpu_state.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::emulator::tests {

/* An extern "C" gadget entry, as the tests declare them. */
using Gadget = void (*)();

[[nodiscard]] inline std::uintptr_t address(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

/* Computed here rather than with the builder's helper, so a packed operand
 * is checked against an independent value. */
[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned reg) noexcept
{
	return offsetof(::uml::cpu::arm64::CpuState, vectors) +
		reg * sizeof(::uml::cpu::arm64::VectorRegister);
}

} // namespace uml::emulator::tests
