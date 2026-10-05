// SPDX-License-Identifier: GPL-2.0-or-later
#include "gadget_entry.hpp"
#include "program.hpp"
#include "sme_layout.h"

#include <algorithm>
#include <utility>

namespace uml::cpu::arm64::gadgets {
namespace {

/* A fault in the entered block itself resolves here. One in a linked
 * successor left from a stream this program does not hold, and the executor,
 * which can find that successor, resolves it. */
void resolve_entered_fault(const Program &program, CpuState &cpu,
	::uml::cpu::EngineEvent event) noexcept
{
	if (event == ::uml::cpu::EngineEvent::memory_fault &&
	    program_contains_stream(program, cpu.gadget_indirect_miss))
		resolve_fault_position(program, cpu.pc);
}

} // namespace

Program::Program(std::uint64_t guest_address, std::size_t instruction_count,
		 ProgramWords words,
		 bool accesses_fp_state) noexcept
	: guest_address_{guest_address}, instruction_count_{instruction_count},
	  words_{std::move(words)}, accesses_fp_state_{accesses_fp_state}
{}
::uml::cpu::EngineEvent execute(const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb) noexcept
{
	std::size_t executed = 0;
	return execute(program, cpu, tlb, executed);
}

/* The per-block form below with the host environment handed back; fault
 * resolution touches only the PC and the exit scratch, so doing it first does
 * not matter. */
::uml::cpu::EngineEvent execute(const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
	      std::size_t &executed) noexcept
{
	const auto event = execute_preserving_native_fp_environment(
		program, cpu, tlb, executed);
	restore_native_fp_environment(cpu);
	return event;
}

::uml::cpu::EngineEvent execute_preserving_native_fp_environment(
	const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
	std::size_t &executed) noexcept
{
	executed = 0;
	if (program.words().empty())
		return ::uml::cpu::EngineEvent::fatal;
	if (program.accesses_fp_state())
		cpu.access_fp_state();
	const auto result =
		uml_arm64_gadget_enter(program.words().data(), &cpu, &tlb,
			program.instruction_count()
#if UML_GADGET_EXECUTION_HISTOGRAM
			, diagnostics::execution_counters()
#endif
			);
	executed = result.executed;
	const auto event = static_cast<::uml::cpu::EngineEvent>(result.event);
	resolve_entered_fault(program, cpu, event);
	return event;
}

::uml::cpu::EngineEvent execute_streaming_preserving_native_fp_environment(
	const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
	std::size_t &executed, bool zt0) noexcept
{
	executed = 0;
	if (program.words().empty())
		return ::uml::cpu::EngineEvent::fatal;
	cpu.sme_host_mode = zt0 ? UML_GADGET_SME_HOST_ZT0 : 0U;
	const auto result =
		uml_arm64_gadget_enter_streaming(program.words().data(), &cpu,
			&tlb, program.instruction_count()
#if UML_GADGET_EXECUTION_HISTOGRAM
			, diagnostics::execution_counters()
#endif
			);
	cpu.sme_host_mode = 0;
	executed = result.executed;
	const auto event = static_cast<::uml::cpu::EngineEvent>(result.event);
	resolve_entered_fault(program, cpu, event);
	return event;
}

} // namespace uml::cpu::arm64::gadgets
