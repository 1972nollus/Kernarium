// SPDX-License-Identifier: GPL-2.0-or-later
#include "program.hpp"
#include "executor.hpp"
#include "native_builder.hpp"
#include "native_entry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

namespace uml::cpu::x86_64::gadgets {
namespace {

[[nodiscard]] bool fatal_trace_enabled() noexcept
{
	const auto *value = std::getenv("UML_X86_FATAL_TRACE");
	return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void trace_fatal(const char *stage, const Program &program,
	const CpuState &cpu, std::size_t instruction_budget,
	std::size_t executed = 0) noexcept
{
	if (!fatal_trace_enabled())
		return;
	std::fprintf(stderr,
		"Darwin x86 gadget fatal stage=%s cpu-rip=%#llx program-rip=%#llx "
		"cpu-mode=%u program-mode=%u native-words=%zu instructions=%zu "
		"budget=%zu executed=%zu\n",
		stage,
		static_cast<unsigned long long>(cpu.rip),
		static_cast<unsigned long long>(program.address()),
		static_cast<unsigned>(cpu.execution_mode),
		static_cast<unsigned>(program.execution_mode()),
		program.native_stream().size(), program.instruction_count(),
		instruction_budget, executed);
	std::fflush(stderr);
}

} // namespace

Program::Program(ExecutionMode mode, std::uint64_t address,
	std::size_t byte_count,
	std::vector<Gadget> gadgets, ProgramLinkResolver links)
	: mode_{mode}, address_{normalize_instruction_pointer(mode, address)},
	  byte_count_{byte_count}, gadgets_{std::move(gadgets)},
	  native_stream_{build_native_stream(
		  gadgets_, advance_instruction_pointer(mode_, address_, byte_count_),
		  mode_, links)}
{}

BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, std::size_t maximum_instructions) noexcept

{
	return build(memory, address, ExecutionMode::long64,
		maximum_instructions);
}

BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions) noexcept
{
	return build(memory, address, mode, maximum_instructions, {});
}

BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions, ProgramLinkResolver links) noexcept
{
	try {
		std::vector<Gadget> stream;
		stream.reserve(maximum_instructions);
		auto current = normalize_instruction_pointer(mode, address);
		std::size_t bytes{};
		for (std::size_t index = 0; index < maximum_instructions; ++index) {
			const auto decoded = decode(memory, current, mode);
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
			current = advance_instruction_pointer(
				mode, current, decoded.instruction.length);
			bytes += decoded.instruction.length;
			if (decoded.instruction.terminal())
				break;
		}
		if (stream.empty())
			return {BuildStatus::illegal_instruction, {}, address};
		return {BuildStatus::built,
			Program{mode, address, bytes, std::move(stream), links}, 0};
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
	if (program.native_stream().empty() ||
		cpu.execution_mode != program.execution_mode() ||
		cpu.rip != program.address()) {
		trace_fatal("precondition", program, cpu, instruction_budget);
		return {::uml::cpu::EngineEvent::fatal, 0, {}, false};
	}
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
			limited_stream = build_native_stream(
				gadgets, final_rip, program.execution_mode());
			native_stream = limited_stream;
		} catch (...) {
			trace_fatal("limited-stream", program, cpu,
				instruction_budget);
			return {::uml::cpu::EngineEvent::fatal, 0, {}, false};
		}
	}

	NativeExecutionContext context{
		.memory = &memory,
		.tlb_entries = executor.tlb_entries(),
		.program_begin = program.address(),
		.program_end = program.address() + program.byte_count(),
		.instruction_limit =
			std::min<std::size_t>(instruction_budget, 4096),
		.target_links = executor.target_links(program.execution_mode()),
		.execution_context = executor.execution_context(),
		.sequence_context = &executor,
		.code_change_reader = [](void *opaque) noexcept {
			return static_cast<Executor *>(opaque)->current_code_changes();
		},
		.code_changes = executor.prepared_code_changes(),
		.code_publications = 0,
	};
	const auto result = uml_x86_64_gadget_enter(
		native_stream.data(), &cpu, &context);
	/* A block that published a code write and then left the stream never
	 * reached the link-following test that settles its own publication, so
	 * the sequence it leaves behind reads as somebody else's change and cools
	 * every link in the address space. Settle it here on the same terms: the
	 * global sequence advanced by exactly this run's own publications, each of
	 * which already cooled the blocks it actually overlapped.
	 */
	if (context.code_publications != 0 &&
		context.code_publications <=
			std::numeric_limits<std::uint64_t>::max() - context.code_changes) {
		const auto settled = context.code_changes + context.code_publications;
		if (executor.current_code_changes() == settled)
			(void)executor.accept_code_changes(settled);
	}
	const auto event = static_cast<::uml::cpu::EngineEvent>(result.event);
	if (event == ::uml::cpu::EngineEvent::continued &&
		context.indirect_miss != nullptr)
		executor.train_indirect(context.indirect_miss, cpu.rip, program.execution_mode());
	if (event == ::uml::cpu::EngineEvent::fatal)
		trace_fatal("native-entry", program, cpu, instruction_budget,
			result.executed);
	return {
		event,
		result.executed,
		context.fault,
		event == ::uml::cpu::EngineEvent::continued &&
			result.executed == instruction_budget,
	};
}

} // namespace uml::cpu::x86_64::gadgets
