/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/arm64/engine_adapters.hpp"

#include "cpu/arm64/dispatcher.hpp"
#include <cstddef>
#include <memory>

namespace uml::cpu::arm64::detail {
namespace {

[[nodiscard]] ExecutionResult interpreter_result(
	const RunResult &result, std::size_t retired,
	const GuestAddressSpace &memory) noexcept
{
	ExecutionResult output{};
	output.retired_instructions = retired;
	output.fault = {
		.address = result.address,
		.access = result.write ? ::uml::memory::MemoryAccess::write :
			::uml::memory::MemoryAccess::read,
	};
	output.syndrome = result.syndrome;
	if (result.reason == StopReason::memory_fault) {
		if (memory.has_fault())
			output.fault.address = memory.last_fault().address;
		/* The static MMU reports the failed page, while Dispatcher::run
		 * retains the architectural direction for compound operations such as
		 * atomic RMW. Preserve that direction in the common result. */
		output.fault.access = result.write ?
			::uml::memory::MemoryAccess::write :
			::uml::memory::MemoryAccess::read;
	}
	switch (result.reason) {
	case StopReason::continued:
		output.reason = ExitReason::instruction_budget;
		break;
	case StopReason::system_call:
		output.reason = ExitReason::system_call;
		break;
	case StopReason::memory_fault:
		output.reason = ExitReason::memory_fault;
		break;
	case StopReason::budget_exhausted:
		output.reason = ExitReason::instruction_budget;
		break;
	case StopReason::illegal_instruction:
		output.reason = ExitReason::illegal_instruction;
		break;
	case StopReason::unsupported_instruction:
		/* A valid encoding that the strict interpreter does not implement is
		 * an emulator coverage failure, not a guest-visible undefined
		 * instruction. Preserve the pre-refactor fatal/debug boundary.
		 */
		output.reason = ExitReason::fatal;
		break;
	case StopReason::breakpoint:
		output.reason = ExitReason::breakpoint;
		break;
	case StopReason::gcs_fault:
		output.reason = ExitReason::gcs_fault;
		break;
	case StopReason::floating_point_trap:
		output.reason = ExitReason::floating_point_trap;
		break;
	}
	return output;
}

class InterpreterEngine final : public CpuEngine {
public:
	explicit InterpreterEngine(EngineConfiguration configuration) noexcept
		: host_capabilities_{configuration.host_capabilities}
	{
	}

	[[nodiscard]] EngineKind kind() const noexcept override
	{
		return EngineKind::interpreter;
	}

	[[nodiscard]] ExecutionResult run(ExecutionRequest &request) noexcept override
	{
		std::size_t retired = 0;
		try {
			const HostCapabilitiesScope capabilities_scope{host_capabilities_};
			if (request.interrupt.pending())
				return {.reason = ExitReason::interrupt};
			if (request.instruction_budget == 0)
				return {.reason = ExitReason::instruction_budget};
			const Dispatcher dispatcher;
			for (;;) {
				if (request.interrupt.pending())
					return {.reason = ExitReason::interrupt,
						.retired_instructions = retired};
				const auto result = dispatcher.step(
					request.cpu, request.memory);
				retired += result.instructions;
				if (request.boundary != nullptr &&
					request.boundary(request.cpu,
						request.boundary_context))
					return {.reason = ExitReason::system_call,
						.retired_instructions = retired,
						.syndrome = result.syndrome};
				if (result.reason != StopReason::continued)
					return interpreter_result(
						result, retired, request.memory);
				if (retired >= request.instruction_budget)
					return {.reason = ExitReason::instruction_budget,
						.retired_instructions = retired};
			}
		} catch (...) {
			return {.reason = ExitReason::fatal,
				.retired_instructions = retired};
		}
	}

private:
	HostCapabilities host_capabilities_;
};

} // namespace

std::unique_ptr<CpuEngine> make_interpreter_engine(
	EngineConfiguration configuration)
{
	return std::make_unique<InterpreterEngine>(configuration);
}

} // namespace uml::cpu::arm64::detail
