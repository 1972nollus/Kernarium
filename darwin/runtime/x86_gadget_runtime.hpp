// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if DARWIN_UML_X86_64_GUEST

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "memory/static_guest_memory.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::runtime {

struct X86GadgetRunResult final {
	::uml::cpu::ExecutionResult execution;
};

[[nodiscard]] X86GadgetRunResult run_x86_gadgets(
	::uml::cpu::x86_64::CpuState &cpu,
	::uml::memory::StaticGuestMemory &memory,
	::uml::cpu::x86_64::gadgets::Executor &executor,
	unsigned long generation, std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt) noexcept;

} // namespace uml::runtime

#endif
