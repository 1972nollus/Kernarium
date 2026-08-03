/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/arm64/fp_environment.hpp"
#include "cpu/engine.hpp"
#include "memory/static_guest_memory.hpp"
#include "memory/tlb.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::detail {

/* The translated engines intentionally share only this boundary adapter.  It
 * owns no translation state and does not select an engine, so each engine can
 * retain its own cache, compiler, and execution implementation. */
template<bool PollCallback, typename Executor>
[[nodiscard]] ExecutionResult run_translated_loop(
	Executor &executor, ExecutionRequest &request,
	::uml::memory::StaticGuestMemory &memory)
{
	if (!executor.begin_execution(request.generation, request.cpu.pc))
		return {.reason = ExitReason::fatal};
	struct ExecutionScope final {
		Executor &executor;

		~ExecutionScope() noexcept
		{
			executor.end_execution();
		}
	};
	[[maybe_unused]] const ExecutionScope execution_scope{executor};
	const auto interrupt_pending = [&]() noexcept {
		if (request.interrupt.state_pending())
			return true;
		if constexpr (PollCallback)
			return request.interrupt.callback_pending();
		return false;
	};

	const auto finish = [&](ExitReason reason, std::size_t retired,
					std::uint64_t address = 0,
					::uml::memory::MemoryAccess access =
						::uml::memory::MemoryAccess::read,
					std::uint32_t syndrome = 0) {
		/* Return a materialized architectural FP state. The application
		 * boundary also normalizes it before exporting a task frame, but the
		 * common engine contract must be stable for host callers too. */
		arm64::restore_native_fp_environment(request.cpu);
		return ExecutionResult{
			.reason = reason,
			.retired_instructions = retired,
			.fault = {.address = address, .access = access},
			.syndrome = syndrome,
		};
	};

	std::size_t retired = 0;
	for (;;) {
		if (interrupt_pending())
			return finish(ExitReason::interrupt, retired);
		std::size_t block_instructions = 0;
		const auto event = executor.run_block(
			request.cpu, memory, request.generation, block_instructions);
		retired += block_instructions;
		if (executor.preempted())
			return finish(ExitReason::interrupt, retired);
		if (request.boundary != nullptr &&
			request.boundary(request.cpu, request.boundary_context))
			return finish(ExitReason::system_call, retired);
		if (event == EngineEvent::continued) {
			if (retired < request.instruction_budget)
				continue;
			return finish(ExitReason::instruction_budget, retired);
		}

		switch (event) {
		case EngineEvent::system_call:
			return finish(ExitReason::system_call, retired);
		case EngineEvent::memory_fault:
			return finish(ExitReason::memory_fault, retired,
				executor.fault_address(),
				executor.fault_access() == ::uml::memory::Access::write ?
					::uml::memory::MemoryAccess::write :
					::uml::memory::MemoryAccess::read);
		case EngineEvent::interrupt:
			/* Translated blocks use the interrupt event as their ordinary
			 * cold-boundary/fairness exit. It becomes guest-visible only when
			 * an actual interrupt token or engine preemption is pending.
			 */
			if (interrupt_pending())
				return finish(ExitReason::interrupt, retired);
			if (retired < request.instruction_budget)
				continue;
			return finish(ExitReason::instruction_budget, retired);
		case EngineEvent::illegal_instruction:
			return finish(ExitReason::illegal_instruction, retired);
		case EngineEvent::breakpoint:
			return finish(ExitReason::breakpoint, retired);
		case EngineEvent::gcs_fault:
			return finish(ExitReason::gcs_fault, retired);
		case EngineEvent::floating_point_trap:
			return finish(ExitReason::floating_point_trap, retired, 0,
				::uml::memory::MemoryAccess::read,
				executor.floating_point_syndrome());
		case EngineEvent::fatal:
			return finish(ExitReason::fatal, retired);
		case EngineEvent::continued:
			break;
		}
	}
}

template<typename Executor>
[[nodiscard]] ExecutionResult run_translated(
	Executor &executor, ExecutionRequest &request)
{
	if (request.translated_memory == nullptr ||
		request.instruction_budget == 0)
		return {.reason = request.translated_memory == nullptr ?
			ExitReason::fatal : ExitReason::instruction_budget};
	/* Callback presence is stable for the whole guest entry. Production
	 * gadget/JIT requests use only sequence/atomic state, so their block loop
	 * contains no callback branch or indirect call. */
	if (request.interrupt.poll != nullptr)
		return run_translated_loop<true>(
			executor, request, *request.translated_memory);
	return run_translated_loop<false>(
		executor, request, *request.translated_memory);
}

} // namespace uml::cpu::arm64::detail
