// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../cpu_state.hpp"
#include "tlb.hpp"
#include "execution_histogram.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::gadgets {

struct EngineResult final {
	std::int32_t event;
	std::uint32_t reserved;
	std::size_t executed;
};

extern "C" EngineResult uml_arm64_gadget_enter(
	const std::uintptr_t *program, CpuState *cpu, ::uml::memory::Tlb *tlb,
	std::size_t initial_instructions
#if UML_GADGET_EXECUTION_HISTOGRAM
	, void *dependency_counters
#endif
	);

} // namespace uml::cpu::arm64::gadgets
