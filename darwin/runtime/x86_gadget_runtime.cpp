// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/x86_gadget_runtime.hpp"

#if DARWIN_UML_X86_64_GUEST

#include "cpu/x86_64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace uml::runtime {
namespace {

namespace x86_64 = ::uml::cpu::x86_64;
using ::uml::memory::StaticGuestMemory;

class X86StaticExecutionScope final {
public:
	X86StaticExecutionScope(unsigned long generation, std::uint64_t guest_pc)
		: context_{darwin_uml_static_vm_execution_context_create(generation)}
	{
		entered_ = context_ != nullptr &&
			darwin_uml_static_vm_execution_context_begin(
				context_, guest_pc) == 0;
	}

	~X86StaticExecutionScope()
	{
		if (entered_)
			(void)darwin_uml_static_vm_execution_context_end(context_);
		darwin_uml_static_vm_execution_context_destroy(context_);
	}

	X86StaticExecutionScope(const X86StaticExecutionScope &) = delete;
	X86StaticExecutionScope &operator=(const X86StaticExecutionScope &) = delete;

	[[nodiscard]] bool entered() const noexcept { return entered_; }
	[[nodiscard]] void *context() const noexcept { return context_; }
	[[nodiscard]] darwin_uml_static_sequences sequences() const noexcept
	{
		return darwin_uml_static_vm_execution_context_sequences(context_);
	}

private:
	void *context_{};
	bool entered_{};
};

bool execution_interrupt_boundary(void *context) noexcept
{
	const auto *execution =
		static_cast<const X86StaticExecutionScope *>(context);
	return execution != nullptr &&
		execution->sequences().execution_interrupt != 0;
}

[[nodiscard]] x86_64::gadgets::ProgramCacheSequences cache_sequences(
	void *context) noexcept
{
	const auto sequences =
		darwin_uml_static_vm_execution_context_sequences(context);
	return {
		.mapping_changes = sequences.mapping_changes,
		.code_changes = sequences.code_changes,
		.interrupted = sequences.execution_interrupt != 0,
	};
}

/* The address space keeps a short history of the ranges its code changes
 * touched, which is what lets the cache retire the translations covering them
 * instead of every link it holds. */
[[nodiscard]] int cache_code_changes(void *context, std::uint64_t sequence,
	x86_64::gadgets::ProgramCodeChange *changes, std::size_t capacity,
	std::uint64_t *current_sequence) noexcept
{
	static_assert(sizeof(x86_64::gadgets::ProgramCodeChange) ==
		sizeof(darwin_uml_static_code_change));
	return darwin_uml_static_vm_execution_context_code_change_ranges(
		context, sequence,
		reinterpret_cast<darwin_uml_static_code_change *>(changes),
		capacity, current_sequence);
}

/* The same history for mappings: a reader that is only a few changes behind
 * retires the software-TLB entries those ranges cover instead of every entry
 * it holds. */
[[nodiscard]] int cache_mapping_changes(void *, std::uint64_t generation,
	std::uint64_t sequence, x86_64::gadgets::ProgramMappingChange *changes,
	std::size_t capacity, std::uint64_t *current_sequence) noexcept
{
	static_assert(sizeof(x86_64::gadgets::ProgramMappingChange) ==
		sizeof(darwin_uml_static_mapping_change));
	return darwin_uml_static_vm_mapping_changes(generation, sequence,
		reinterpret_cast<darwin_uml_static_mapping_change *>(changes),
		capacity, current_sequence);
}

[[nodiscard]] bool fatal_trace_enabled() noexcept
{
	const auto *value = std::getenv("UML_X86_FATAL_TRACE");
	return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void trace_fatal(const char *stage, const x86_64::CpuState &cpu,
	unsigned long generation, int error = 0, std::size_t retired = 0,
	StaticGuestMemory *memory = nullptr) noexcept
{
	if (!fatal_trace_enabled())
		return;
	std::fprintf(stderr,
		"Darwin x86 runtime fatal stage=%s rip=%#llx mode=%u "
		"generation=%lu errno=%d retired=%zu",
		stage, static_cast<unsigned long long>(cpu.rip),
		static_cast<unsigned>(cpu.execution_mode), generation, error,
		retired);
	if (memory != nullptr) {
		std::array<std::byte, 15> bytes{};
		if (memory->fetch(cpu.rip, bytes)) {
			std::fputs(" bytes=", stderr);
			for (const auto byte : bytes)
				std::fprintf(stderr, "%02x",
					std::to_integer<unsigned>(byte));
		} else {
			std::fputs(" bytes=<unreadable>", stderr);
		}
	}
	std::fputc('\n', stderr);
	std::fflush(stderr);
}

} // namespace

X86GadgetRunResult run_x86_gadgets(x86_64::CpuState &cpu,
	StaticGuestMemory &memory, x86_64::gadgets::Executor &executor,
	unsigned long generation, std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt) noexcept
{
	X86GadgetRunResult run{};
	auto &result = run.execution;
	result.reason = ::uml::cpu::ExitReason::fatal;
	X86StaticExecutionScope execution{generation, cpu.rip};
	if (!execution.entered()) {
		trace_fatal("execution-scope", cpu, generation, errno);
		return run;
	}
	/* A reset executor starts without the permission, so every run restates
	 * what the host reported. */
	executor.permit_unaligned_exclusives(
		::uml::platform::apple::probed_host_capabilities().has(
			::uml::platform::ArmFeature::lse2));
	auto guarded_interrupt = interrupt;
	guarded_interrupt.poll = execution_interrupt_boundary;
	guarded_interrupt.poll_context = &execution;
	bool executor_prepared = false;
	std::uint64_t prepared_mapping_changes{};
	std::uint64_t prepared_code_changes{};
	while (result.retired_instructions < instruction_budget) {
		const auto sequences = execution.sequences();
		if (sequences.execution_interrupt != 0) {
			result.reason = ::uml::cpu::ExitReason::interrupt;
			return run;
		}
		memory.prepare(generation, sequences.mapping_changes);
		if (!executor_prepared ||
			prepared_mapping_changes != sequences.mapping_changes ||
			prepared_code_changes != sequences.code_changes) {
			executor.prepare(memory, generation, sequences.mapping_changes,
				sequences.code_changes, execution.context(), cache_sequences,
				cache_code_changes, cache_mapping_changes);
			prepared_mapping_changes = sequences.mapping_changes;
			prepared_code_changes = sequences.code_changes;
			executor_prepared = true;
		}
		if (cpu.rip == DARWIN_UML_SIGRETURN_TRAMPOLINE) {
			cpu.gpr(x86_64::Register::rax) = 15;
			cpu.system_call_32bit = false;
			result.reason = ::uml::cpu::ExitReason::system_call;
			return run;
		}
		const auto available = instruction_budget - result.retired_instructions;
		const auto selected = executor.select_program(
			memory, cpu.rip, cpu.execution_mode,
			std::min<std::size_t>(64, available));
		switch (selected.status) {
		case x86_64::gadgets::BuildStatus::fetch_fault:
			result.reason = ::uml::cpu::ExitReason::memory_fault;
			result.fault = {
				selected.fault_address,
				::uml::memory::MemoryAccess::execute};
			return run;
		case x86_64::gadgets::BuildStatus::illegal_instruction:
			result.reason = ::uml::cpu::ExitReason::illegal_instruction;
			trace_fatal("build-illegal", cpu, generation, 0,
				result.retired_instructions, &memory);
			return run;
		case x86_64::gadgets::BuildStatus::allocation_failure:
			trace_fatal("build-allocation", cpu, generation, errno,
				result.retired_instructions);
			return run;
		case x86_64::gadgets::BuildStatus::built:
			break;
		}
		if (selected.program == nullptr) {
			trace_fatal("select-program", cpu, generation, errno,
				result.retired_instructions);
			return run;
		}
		const auto executed = x86_64::gadgets::execute(
			*selected.program, cpu, memory, executor, available,
			guarded_interrupt);
		result.retired_instructions += executed.retired_instructions;
		prepared_code_changes = executor.prepared_code_changes();
		result.fault = executed.fault;
		if (executed.instruction_budget_exhausted) {
			result.reason = ::uml::cpu::ExitReason::instruction_budget;
			return run;
		}
		switch (executed.event) {
		case ::uml::cpu::EngineEvent::continued:
			continue;
		case ::uml::cpu::EngineEvent::system_call:
			result.reason = ::uml::cpu::ExitReason::system_call;
			return run;
		case ::uml::cpu::EngineEvent::memory_fault:
			result.reason = ::uml::cpu::ExitReason::memory_fault;
			return run;
		case ::uml::cpu::EngineEvent::interrupt:
			result.reason = ::uml::cpu::ExitReason::interrupt;
			return run;
		case ::uml::cpu::EngineEvent::illegal_instruction:
			result.reason = ::uml::cpu::ExitReason::illegal_instruction;
			trace_fatal("execute-illegal", cpu, generation, 0,
				result.retired_instructions, &memory);
			return run;
		case ::uml::cpu::EngineEvent::breakpoint:
			result.reason = ::uml::cpu::ExitReason::breakpoint;
			result.syndrome = 3;
			return run;
		case ::uml::cpu::EngineEvent::overflow:
			result.reason = ::uml::cpu::ExitReason::breakpoint;
			result.syndrome = 4;
			return run;
		case ::uml::cpu::EngineEvent::floating_point_trap:
			result.reason = ::uml::cpu::ExitReason::floating_point_trap;
			return run;
		case ::uml::cpu::EngineEvent::gcs_fault:
		case ::uml::cpu::EngineEvent::pc_alignment:
		case ::uml::cpu::EngineEvent::fatal:
			trace_fatal("execute", cpu, generation, 0,
				result.retired_instructions);
			return run;
		}
	}
	result.reason = ::uml::cpu::ExitReason::instruction_budget;
	return run;
}

} // namespace uml::runtime

#endif
