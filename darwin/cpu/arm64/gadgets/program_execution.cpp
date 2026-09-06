// SPDX-License-Identifier: GPL-2.0-or-later
#include "gadget_entry.hpp"
#include "program.hpp"

#if UML_GADGET_DEPENDENCY_COVERAGE && UML_GADGET_EXECUTION_HISTOGRAM
#error "select one gadget counting diagnostic at a time"
#endif

#include <algorithm>
#include <utility>

namespace uml::cpu::arm64::gadgets {

Program::Program(std::uint64_t guest_address, std::size_t instruction_count,
		 ProgramWords words,
		 bool accesses_fp_state) noexcept
	: instruction_count_{instruction_count},
	  words_{std::move(words)}, accesses_fp_state_{accesses_fp_state}
{
	runs_[0] = {guest_address,
		static_cast<std::uint32_t>(instruction_count)};
}

Program::Program(std::span<const GuestRun> runs, std::size_t instruction_count,
		 ProgramWords words,
		 bool accesses_fp_state) noexcept
	: instruction_count_{instruction_count},
	  words_{std::move(words)},
	  run_count_{static_cast<std::uint8_t>(
		  std::min(runs.size(), maximum_runs))},
	  accesses_fp_state_{accesses_fp_state}
{
	std::copy_n(runs.begin(), run_count_, runs_.begin());
}

Program::Program(std::uint64_t guest_address, std::size_t instruction_count,
		 std::pmr::vector<std::uintptr_t> words,
		 bool accesses_fp_state)
	: Program{guest_address, instruction_count, ProgramWords{std::move(words)},
		  accesses_fp_state}
{}

Program::Program(std::span<const GuestRun> runs, std::size_t instruction_count,
		 std::pmr::vector<std::uintptr_t> words,
		 bool accesses_fp_state)
	: Program{runs, instruction_count, ProgramWords{std::move(words)},
		  accesses_fp_state}
{}
::uml::cpu::EngineEvent execute(const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb) noexcept
{
	std::size_t executed = 0;
	return execute(program, cpu, tlb, executed);
}

::uml::cpu::EngineEvent execute(const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
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
#if UML_GADGET_DEPENDENCY_COVERAGE
			, diagnostics::dependency_counters()
#elif UML_GADGET_EXECUTION_HISTOGRAM
			, diagnostics::execution_counters()
#endif
			);
	restore_native_fp_environment(cpu);
	executed = result.executed;
	return static_cast<::uml::cpu::EngineEvent>(result.event);
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
#if UML_GADGET_DEPENDENCY_COVERAGE
			, diagnostics::dependency_counters()
#elif UML_GADGET_EXECUTION_HISTOGRAM
			, diagnostics::execution_counters()
#endif
			);
	executed = result.executed;
	return static_cast<::uml::cpu::EngineEvent>(result.event);
}

} // namespace uml::cpu::arm64::gadgets
