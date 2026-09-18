// SPDX-License-Identifier: GPL-2.0-or-later
#include "reference_engine.hpp"

#include "cpu/arm64/dispatcher.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::emulator::tests {
namespace {

using ::uml::cpu::ExecutionResult;
using ::uml::cpu::ExitReason;
using ::uml::cpu::GuestAddressSpace;
using ::uml::cpu::arm64::Dispatcher;
using ::uml::cpu::arm64::RunResult;
using ::uml::cpu::arm64::StopReason;

[[nodiscard]] ExecutionResult reference_result(
    const RunResult &result, std::size_t retired,
    const GuestAddressSpace &memory) noexcept {
  const auto result_access =
      result.access == ::uml::memory::MemoryAccess::execute
          ? ::uml::memory::MemoryAccess::execute
      : result.write ? ::uml::memory::MemoryAccess::write
                     : ::uml::memory::MemoryAccess::read;
  ExecutionResult output{};
  output.retired_instructions = retired;
  output.fault = {
      .address = result.address,
      .access = result_access,
  };
  output.syndrome = result.syndrome;
  if (result.reason == StopReason::memory_fault) {
    if (memory.has_fault())
      output.fault.address = memory.last_fault().address;
    /* The static MMU reports the failed page, while Dispatcher::run retains
     * the architectural direction for compound operations such as atomic RMW.
     * Preserve that direction in the common result. */
    output.fault.access = result_access;
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
  case StopReason::unavailable_feature:
    output.reason = ExitReason::illegal_instruction;
    break;
  case StopReason::unsupported_instruction:
    /* An implementation gap is diagnostically distinct inside the dispatcher,
     * but it remains scoped to the guest task at the engine boundary. It must
     * not terminate the emulator instance. */
    output.reason = ExitReason::illegal_instruction;
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

} // namespace

ExecutionResult
ReferenceEngine::run(::uml::cpu::ExecutionRequest &request) noexcept {
  std::size_t retired = 0;
  try {
    const ::uml::cpu::HostCapabilitiesScope capabilities_scope{
        host_capabilities_};
    if (request.interrupt.pending())
      return {.reason = ExitReason::interrupt};
    if (request.instruction_budget == 0)
      return {.reason = ExitReason::instruction_budget};
    const Dispatcher dispatcher;
    for (;;) {
      if (request.interrupt.pending())
        return {.reason = ExitReason::interrupt,
                .retired_instructions = retired};
      /* BranchTo installs the target before the architecture checks
       * instruction-address alignment. Preserve any prior retired prefix, but
       * never attempt a misaligned host-side fetch. */
      if ((request.cpu.pc & (alignof(std::uint32_t) - 1U)) != 0U)
        return {
            .reason = ExitReason::pc_alignment,
            .retired_instructions = retired,
            .fault =
                {
                    .address = request.cpu.pc,
                    .access = ::uml::memory::MemoryAccess::execute,
                },
        };
      const auto result = dispatcher.step(request.cpu, request.memory);
      retired += result.instructions;
      if (request.boundary != nullptr &&
          request.boundary(request.cpu, request.boundary_context))
        return {.reason = ExitReason::system_call,
                .retired_instructions = retired,
                .syndrome = result.syndrome};
      if (result.reason != StopReason::continued)
        return reference_result(result, retired, request.memory);
      if (retired >= request.instruction_budget)
        return {.reason = ExitReason::instruction_budget,
                .retired_instructions = retired};
    }
  } catch (...) {
    return {.reason = ExitReason::fatal, .retired_instructions = retired};
  }
}

} // namespace uml::emulator::tests
