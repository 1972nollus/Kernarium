// SPDX-License-Identifier: GPL-2.0-or-later
#include "program.hpp"
#include "executor.hpp"
#include "native_builder.hpp"
#include "native_entry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>

namespace uml::cpu::x86_64::gadgets {
Program::Program(std::uint64_t address, std::size_t byte_count,
	std::vector<Gadget> gadgets)
	: address_{address}, byte_count_{byte_count}, gadgets_{std::move(gadgets)},
	  native_stream_{build_native_stream(
		  gadgets_, address_ + byte_count_)}
{}

BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, std::size_t maximum_instructions) noexcept
{
	try {
		std::vector<Gadget> stream;
		stream.reserve(maximum_instructions);
		auto current = address;
		std::size_t bytes{};
		for (std::size_t index = 0; index < maximum_instructions; ++index) {
			const auto decoded = decode(memory, current);
			if (decoded.status != DecodeStatus::decoded) {
				/* Preserve the side effects and retirement order of the valid
				 * prefix. Dispatch will decode the faulting instruction again
				 * after that prefix has executed.
				 */
				if (!stream.empty())
					break;
				if (decoded.status == DecodeStatus::fetch_fault)
					return {BuildStatus::fetch_fault, {},
						decoded.fault_address};
				return {BuildStatus::illegal_instruction, {}, current};
			}
			stream.push_back({current, decoded.instruction});
			current += decoded.instruction.length;
			bytes += decoded.instruction.length;
			if (decoded.instruction.terminal())
				break;
		}
		if (stream.empty())
			return {BuildStatus::illegal_instruction, {}, address};
		return {BuildStatus::built,
			Program{address, bytes, std::move(stream)}, 0};
	} catch (const std::logic_error &) {
		return {BuildStatus::illegal_instruction, {}, address};
	} catch (...) {
		return {BuildStatus::allocation_failure, {}, 0};
	}
}

ExecuteResult execute(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt) noexcept

{
	try {
		Executor executor;
		executor.prepare(memory);
		return execute(program, cpu, memory, executor,
			instruction_budget, interrupt);
	} catch (...) {
		return {::uml::cpu::EngineEvent::fatal, 0, {}, false};
	}
}

ExecuteResult execute(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt) noexcept
{
	if (program.native_stream().empty() || cpu.rip != program.address())
		return {::uml::cpu::EngineEvent::fatal, 0, {}, false};
	if (interrupt.pending())
		return {::uml::cpu::EngineEvent::interrupt, 0, {}, false};
	if (instruction_budget == 0)
		return {::uml::cpu::EngineEvent::continued, 0, {}, true};

	std::vector<std::uintptr_t> limited_stream;
	std::span<const std::uintptr_t> native_stream = program.native_stream();
	const bool limited = instruction_budget < program.instruction_count();
	if (limited) {
		try {
			const auto gadgets = program.stream().first(instruction_budget);
			const auto final_rip = program.stream()[instruction_budget].address;
			limited_stream = build_native_stream(gadgets, final_rip);
			native_stream = limited_stream;
		} catch (...) {
			return {::uml::cpu::EngineEvent::fatal, 0, {}, false};
		}
	}

	NativeExecutionContext context{
		.memory = &memory,
		.tlb_entries = executor.tlb_entries(),
		.program_begin = program.address(),
		.program_end = program.address() + program.byte_count(),
	};
	const auto result = uml_x86_64_gadget_enter(
		native_stream.data(), &cpu, &context);
	const auto event = static_cast<::uml::cpu::EngineEvent>(result.event);
	return {
		event,
		result.executed,
		context.fault,
		limited && event == ::uml::cpu::EngineEvent::continued &&
			result.executed == instruction_budget,
	};
}

} // namespace uml::cpu::x86_64::gadgets
