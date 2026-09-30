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

/* The most instructions the builder puts in one block. */
constexpr std::size_t block_instructions = 64;

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
	return darwin_uml_static_vm_execution_context_code_change_ranges(
		context, sequence, changes, capacity, current_sequence);
}

/* The same history for mappings: a reader that is only a few changes behind
 * retires the software-TLB entries those ranges cover instead of every entry
 * it holds. */
[[nodiscard]] int cache_mapping_changes(void *, std::uint64_t generation,
	std::uint64_t sequence, x86_64::gadgets::ProgramMappingChange *changes,
	std::size_t capacity, std::uint64_t *current_sequence) noexcept
{
	return darwin_uml_static_vm_mapping_changes(generation, sequence,
		changes, capacity, current_sequence);
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

/* x86 exception vectors the runtime reports as a breakpoint's syndrome. */
constexpr std::uint32_t stack_segment_fault = 12;
constexpr std::uint32_t general_protection = 13;

/* Bits 63 to 47 all equal: an address of the 4-level paging x86-64 Linux
 * runs userspace under. */
[[nodiscard]] constexpr bool canonical(std::uint64_t address) noexcept
{
	const auto high = static_cast<std::int64_t>(address) >> 47;
	return high == 0 || high == -1;
}

/* The #GP error code of the instruction at RIP. INT n that user mode may
 * not use names its IDT entry, (n << 3) | 2, as the hardware pushes when
 * the gate refuses it; every other #GP the engine raises has none. */
[[nodiscard]] std::int32_t general_protection_error(
	StaticGuestMemory &memory, const x86_64::CpuState &cpu) noexcept
{
	const auto decoded = x86_64::decode(memory, cpu.rip, cpu.execution_mode);
	if (decoded.status != x86_64::DecodeStatus::decoded)
		return 0;
	const auto &instruction = decoded.instruction;
	if (instruction.operation != x86_64::Operation::privileged ||
		instruction.vector_opcode != 0xcd)
		return 0;
	return static_cast<std::int32_t>((instruction.source.immediate << 3U) | 2U);
}

/* A non-canonical address raises #SS when the stack segment forms it --
 * an implicit stack access, or an operand based on RSP or RBP with no
 * segment override -- and #GP otherwise, neither with an error code. */
[[nodiscard]] std::uint32_t noncanonical_vector(
	StaticGuestMemory &memory, const x86_64::CpuState &cpu) noexcept
{
	const auto decoded = x86_64::decode(memory, cpu.rip, cpu.execution_mode);
	if (decoded.status != x86_64::DecodeStatus::decoded)
		return general_protection;
	const auto &instruction = decoded.instruction;
	using x86_64::Operation;
	switch (instruction.operation) {
	case Operation::push:
	case Operation::pop:
	case Operation::push_all:
	case Operation::pop_all:
	case Operation::push_flags:
	case Operation::pop_flags:
	case Operation::enter:
	case Operation::call:
	case Operation::call_indirect:
	case Operation::far_call:
	case Operation::ret:
	case Operation::far_ret:
	case Operation::interrupt_ret:
	case Operation::segment_stack:
		if (!canonical(cpu.gpr(x86_64::Register::rsp)))
			return stack_segment_fault;
		break;
	case Operation::leave:
		if (!canonical(cpu.gpr(x86_64::Register::rbp)))
			return stack_segment_fault;
		break;
	default:
		break;
	}
	for (const auto *operand : {&instruction.destination,
		&instruction.source, &instruction.auxiliary}) {
		const auto &memory_operand = operand->memory;
		if (operand->kind == x86_64::OperandKind::memory &&
			!memory_operand.rip_relative &&
			memory_operand.segment == x86_64::Segment::none &&
			(memory_operand.base == 4 || memory_operand.base == 5))
			return stack_segment_fault;
	}
	return general_protection;
}

} // namespace

X86GadgetRunResult run_x86_gadgets(x86_64::CpuState &cpu,
	StaticGuestMemory &memory, x86_64::gadgets::Executor &executor,
	unsigned long generation, std::size_t instruction_budget,
	bool exact_budget, const ::uml::cpu::InterruptToken &interrupt) noexcept
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
	/* Set once POPF or IRET arms RFLAGS.TF: the run ends after one more
	 * instruction and reports the #DB that instruction raises. */
	bool trap_armed = false;
	const auto budget_reason = [&]() noexcept {
		return trap_armed ? ::uml::cpu::ExitReason::single_step :
			::uml::cpu::ExitReason::instruction_budget;
	};
	/* A long-mode access to a non-canonical address never reaches the page
	 * tables: x86 raises #GP or #SS for it, not #PF. A branch to one faults
	 * at its target here rather than at the branch. */
	const auto memory_fault = [&]() noexcept {
		if (cpu.execution_mode == x86_64::ExecutionMode::long64 &&
			!canonical(result.fault.address)) {
			result.reason = ::uml::cpu::ExitReason::breakpoint;
			result.syndrome = result.fault.access ==
				::uml::memory::MemoryAccess::execute ?
				general_protection : noncanonical_vector(memory, cpu);
			result.fault = {};
			return;
		}
		result.reason = ::uml::cpu::ExitReason::memory_fault;
	};
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
		/* Only a step has to stop inside a block. Cutting the block that
		 * crosses an ordinary budget would decode it again for a truncated
		 * copy, which the program keeps -- a different block every
		 * dispatch, and on a busy guest more bytes than the streams -- and
		 * would build short blocks that a later full budget throws away. */
		const std::size_t limit = exact_budget || trap_armed ? available :
			std::max<std::size_t>(available, block_instructions);
		const auto selected = executor.select_program(
			memory, cpu.rip, cpu.execution_mode,
			std::min(block_instructions, limit));
		switch (selected.status) {
		case x86_64::gadgets::BuildStatus::fetch_fault:
			result.fault = {
				selected.fault_address,
				::uml::memory::MemoryAccess::execute};
			memory_fault();
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
		/* A step of a repeated string instruction is one iteration. */
		const bool stepping = (exact_budget || trap_armed) && limit == 1;
		const auto executed = x86_64::gadgets::execute(
			*selected.program, cpu, memory, executor, limit,
			guarded_interrupt, stepping);
		result.retired_instructions += executed.retired_instructions;
		prepared_code_changes = executor.prepared_code_changes();
		result.fault = executed.fault;
		if (executed.instruction_budget_exhausted) {
			result.reason = budget_reason();
			return run;
		}
		if (stepping && executed.repeat_yielded) {
			/* An iteration that did not end its instruction is a whole
			 * step: #DB follows it with RIP naming the instruction and,
			 * as the gadget leaves it, RF set, as it follows each
			 * iteration on x86. */
			result.reason = budget_reason();
			return run;
		}
		switch (executed.event) {
		case ::uml::cpu::EngineEvent::continued:
			/* A repeated string instruction retires once however many
			 * elements it moves, and one its repeat budget stopped has
			 * retired nothing yet, so a loop of long ones would never spend
			 * this budget. The kernel services its timer only when a
			 * dispatch returns, so charge the work the repeat budget covered
			 * instead. A step keeps its exact budget. */
			if (executed.repeat_yielded && !exact_budget && !trap_armed)
				instruction_budget -= std::min(
					instruction_budget - result.retired_instructions,
					x86_64::gadgets::string_repeat_budget);
			continue;
		case ::uml::cpu::EngineEvent::trap_flag:
			/* With no budget left for the next instruction, the next entry
			 * sees TF and steps it instead. */
			if (result.retired_instructions < instruction_budget) {
				instruction_budget = result.retired_instructions + 1;
				trap_armed = true;
			}
			continue;
		case ::uml::cpu::EngineEvent::system_call:
			result.reason = ::uml::cpu::ExitReason::system_call;
			return run;
		case ::uml::cpu::EngineEvent::memory_fault:
			memory_fault();
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
		case ::uml::cpu::EngineEvent::general_protection:
			result.reason = ::uml::cpu::ExitReason::breakpoint;
			result.syndrome = general_protection;
			run.error_code = general_protection_error(memory, cpu);
			return run;
		case ::uml::cpu::EngineEvent::debug_trap:
			result.reason = ::uml::cpu::ExitReason::breakpoint;
			result.syndrome = 1;
			return run;
		case ::uml::cpu::EngineEvent::bound_range:
			result.reason = ::uml::cpu::ExitReason::breakpoint;
			result.syndrome = 5;
			return run;
		case ::uml::cpu::EngineEvent::floating_point_trap:
			result.reason = ::uml::cpu::ExitReason::floating_point_trap;
			return run;
		case ::uml::cpu::EngineEvent::pc_alignment:
		case ::uml::cpu::EngineEvent::fatal:
			trace_fatal("execute", cpu, generation, 0,
				result.retired_instructions);
			return run;
		}
	}
	result.reason = budget_reason();
	return run;
}

} // namespace uml::runtime

#endif
