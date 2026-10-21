/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/arm64/aarch32/guest_key.hpp"
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
	executor.set_single_step(request.single_step);
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
					std::uint32_t syndrome = 0,
					bool alignment = false) {
		/* Return a materialized architectural FP state. The application
		 * boundary also normalizes it before exporting a task frame, but the
		 * common engine contract must be stable for host callers too. */
		arm64::restore_native_fp_environment(request.cpu);
		return ExecutionResult{
			.reason = reason,
			.retired_instructions = retired,
			.fault = {.address = address, .access = access,
				.alignment = alignment},
			.syndrome = syndrome,
		};
	};

	std::size_t retired = 0;
	try {
		for (;;) {
			if (interrupt_pending())
				return finish(ExitReason::interrupt, retired);
			/* AArch64 checks PC alignment at the next instruction boundary.
			 * Keep this outside generated blocks: valid programs pay one cold
			 * block-boundary branch, while a misaligned indirect target retains
			 * the already-retired branch prefix and never reaches host code.
			 */
			if ((request.cpu.pc & (alignof(std::uint32_t) - 1U)) != 0U)
				return finish(ExitReason::pc_alignment, retired,
					request.cpu.pc,
					::uml::memory::MemoryAccess::execute);
			std::size_t block_instructions = 0;
			EngineEvent event;
			try {
				event = executor.run_block(
					request.cpu, memory, request.generation,
					block_instructions);
			} catch (...) {
				/* Executors publish only the committed prefix through the
				 * out-parameter. A cold cache/link postamble can fail after
				 * native work has advanced CpuState, so include that prefix
				 * before materializing the abnormal boundary. */
				retired += block_instructions;
				return finish(ExitReason::fatal, retired);
			}
			retired += block_instructions;
			if (executor.preempted())
				return finish(ExitReason::interrupt, retired);
			if (request.boundary != nullptr &&
				request.boundary(
					request.cpu, request.boundary_context))
				return finish(ExitReason::system_call, retired);
			/* A stepping executor runs one instruction per block. Its
			 * cold-mode boundary is not a guest-visible interrupt. */
			if (request.single_step && block_instructions != 0 &&
				(event == EngineEvent::continued ||
				 event == EngineEvent::interrupt))
				return finish(ExitReason::single_step, retired);
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
					::uml::memory::guest_access(
						executor.fault_access()), 0,
					executor.fault_alignment());
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
			case EngineEvent::floating_point_trap:
				return finish(ExitReason::floating_point_trap, retired, 0,
					::uml::memory::MemoryAccess::read,
					executor.floating_point_syndrome());
			case EngineEvent::pc_alignment:
				return finish(ExitReason::pc_alignment, retired,
					request.cpu.pc,
					::uml::memory::MemoryAccess::execute);
			case EngineEvent::sme_access:
				return finish(ExitReason::sme_access, retired);
			case EngineEvent::overflow:
			case EngineEvent::general_protection:
			case EngineEvent::trap_flag:
			case EngineEvent::debug_trap:
			case EngineEvent::bound_range:
				/* x86 INTO, #GP, TF, ICEBP and BOUND are not arm64 engine
				 * events. */
				return finish(ExitReason::fatal, retired);
			case EngineEvent::fatal:
				return finish(ExitReason::fatal, retired);
			case EngineEvent::continued:
				break;
			}
		}
	} catch (...) {
		/* A later block may fail after earlier translated work left native
		 * architectural state live. Convert the exception here, while both the
		 * exact retired prefix and the request CpuState are still available.
		 * ExecutionScope subsequently balances the MMU execution lifetime. */
		return finish(ExitReason::fatal, retired);
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

/* The entry loop of an AArch32 task for an engine whose executor runs blocks
 * of A32 and T32 code (the gadget engine's run_a32_block): the A64 loop above
 * with the AArch32 differences, kept apart so that loop stays as it is.
 *
 * - cpu.pc is a key (aarch32/guest_key.hpp) for the whole entry; the only
 *   alignment fault left is an A32 target with bit 1 set, whose address is
 *   the PC the key names;
 * - a fault address is the architectural 32-bit address the executor
 *   reports, never a key;
 * - a breakpoint exit carries BKPT's immediate in ExecutionResult::syndrome;
 * - no floating-point trap exit exists: the trap enables are RAZ/WI. */
template<bool PollCallback, typename Executor>
[[nodiscard]] ExecutionResult run_translated_aarch32_loop(
	Executor &executor, ExecutionRequest &request,
	::uml::memory::StaticGuestMemory &memory)
{
	if (!executor.begin_execution(request.generation,
				      aarch32::key_pc(request.cpu.pc)))
		return {.reason = ExitReason::fatal};
	struct ExecutionScope final {
		Executor &executor;

		~ExecutionScope() noexcept
		{
			executor.end_execution();
		}
	};
	[[maybe_unused]] const ExecutionScope execution_scope{executor};
	executor.set_single_step(request.single_step);
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
					std::uint32_t syndrome = 0,
					bool alignment = false) {
		arm64::restore_native_fp_environment(request.cpu);
		return ExecutionResult{
			.reason = reason,
			.retired_instructions = retired,
			.fault = {.address = address, .access = access,
				.alignment = alignment},
			.syndrome = syndrome,
		};
	};

	std::size_t retired = 0;
	try {
		for (;;) {
			if (interrupt_pending())
				return finish(ExitReason::interrupt, retired);
			if ((request.cpu.pc & 3U) != 0U)
				return finish(ExitReason::pc_alignment, retired,
					aarch32::key_pc(request.cpu.pc),
					::uml::memory::MemoryAccess::execute);
			std::size_t block_instructions = 0;
			EngineEvent event;
			try {
				event = executor.run_a32_block(
					request.cpu, memory, request.generation,
					block_instructions);
			} catch (...) {
				retired += block_instructions;
				return finish(ExitReason::fatal, retired);
			}
			retired += block_instructions;
			if (executor.preempted())
				return finish(ExitReason::interrupt, retired);
			if (request.boundary != nullptr &&
				request.boundary(
					request.cpu, request.boundary_context))
				return finish(ExitReason::system_call, retired);
			if (request.single_step && block_instructions != 0 &&
				(event == EngineEvent::continued ||
				 event == EngineEvent::interrupt))
				return finish(ExitReason::single_step, retired);
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
					::uml::memory::guest_access(
						executor.fault_access()), 0,
					executor.fault_alignment());
			case EngineEvent::interrupt:
				if (interrupt_pending())
					return finish(ExitReason::interrupt, retired);
				if (retired < request.instruction_budget)
					continue;
				return finish(ExitReason::instruction_budget, retired);
			case EngineEvent::illegal_instruction:
				return finish(ExitReason::illegal_instruction, retired);
			case EngineEvent::breakpoint:
				return finish(ExitReason::breakpoint, retired, 0,
					::uml::memory::MemoryAccess::read,
					executor.breakpoint_immediate());
			case EngineEvent::pc_alignment:
				return finish(ExitReason::pc_alignment, retired,
					aarch32::key_pc(request.cpu.pc),
					::uml::memory::MemoryAccess::execute);
			case EngineEvent::floating_point_trap:
			case EngineEvent::sme_access:
			case EngineEvent::overflow:
			case EngineEvent::general_protection:
			case EngineEvent::trap_flag:
			case EngineEvent::debug_trap:
			case EngineEvent::bound_range:
			case EngineEvent::fatal:
				return finish(ExitReason::fatal, retired);
			case EngineEvent::continued:
				break;
			}
		}
	} catch (...) {
		return finish(ExitReason::fatal, retired);
	}
}

template<typename Executor>
[[nodiscard]] ExecutionResult run_translated_aarch32_blocks(
	Executor &executor, ExecutionRequest &request)
{
	if (request.translated_memory == nullptr ||
		request.instruction_budget == 0)
		return {.reason = request.translated_memory == nullptr ?
			ExitReason::fatal : ExitReason::instruction_budget};
	if (request.interrupt.poll != nullptr)
		return run_translated_aarch32_loop<true>(
			executor, request, *request.translated_memory);
	return run_translated_aarch32_loop<false>(
		executor, request, *request.translated_memory);
}

} // namespace uml::cpu::arm64::detail
