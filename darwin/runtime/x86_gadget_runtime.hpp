// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if DARWIN_UML_X86_64_GUEST

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "memory/static_guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace uml::runtime {

struct X86GadgetRunResult final {
	::uml::cpu::ExecutionResult execution;
	/* The error code of a #GP or #SS the run ended in: for INT n, the
	 * vector's IDT entry. For #MF, the x87 status word with the control
	 * word above it, and for #XM, MXCSR: what Linux derives the signal's
	 * code from. */
	std::int32_t error_code{};
};

/* `instruction_budget` bounds how long one dispatch runs, and the block that
 * crosses it runs to its end, as a block of the arm64 engines does. With
 * `exact_budget` the run instead stops at exactly that many instructions,
 * which is what a single step needs.
 *
 * While the guest enables #MF or #XM, the run uses `checked_executor`,
 * made on first use and made again when the guest enables more, whose blocks
 * stop before every instruction that could raise one; it runs those singly,
 * checked, and the rest linked, as `executor` runs everything otherwise. */
[[nodiscard]] X86GadgetRunResult run_x86_gadgets(
	::uml::cpu::x86_64::CpuState &cpu,
	::uml::memory::StaticGuestMemory &memory,
	::uml::cpu::x86_64::gadgets::Executor &executor,
	std::unique_ptr<::uml::cpu::x86_64::gadgets::Executor> &checked_executor,
	unsigned long generation, std::size_t instruction_budget,
	bool exact_budget, const ::uml::cpu::InterruptToken &interrupt) noexcept;

} // namespace uml::runtime

#endif
