// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"

#include <cstdint>

namespace uml::runtime {

/* What an AArch64 and an AArch32 entry both start from: no exclusive
 * monitor. An exception return may clear the local monitor, and the bridge
 * has no reservation token in uml_pt_regs, so every field is cleared, with
 * the gadget scratch, and reusing the large CpuState cannot make a later
 * STXR succeed spuriously.
 *
 * The stores are spelled out, in place: through
 * CpuState::clear_exclusive_monitor() the optimizer schedules them
 * differently in the entry. */
[[gnu::always_inline]] inline void clear_entry_monitor(
	::uml::cpu::arm64::CpuState &cpu) noexcept
{
	cpu.exclusive_address = ~std::uint64_t{0};
	cpu.exclusive_key = 0;
	cpu.exclusive_value = 0;
	cpu.exclusive_value2 = 0;
	cpu.exclusive_version = 0;
	cpu.exclusive_size = 0;
	cpu.gadget_zero = 0;
	cpu.gadget_indirect_miss = nullptr;
}

} // namespace uml::runtime
