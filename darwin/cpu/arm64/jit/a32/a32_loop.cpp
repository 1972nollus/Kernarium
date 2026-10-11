// SPDX-License-Identifier: GPL-2.0-or-later

#include "a32_loop.hpp"

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::jit::a32 {

namespace {

using ::uml::cpu::EngineEvent;
namespace memory = ::uml::memory;

/* The immediate of the BKPT the task's PC key names: 16 bits in A32, the low
 * 8 in T32. It is read back from the guest here, on the cold exit, rather
 * than carried by the translation. */
[[nodiscard]] std::uint32_t breakpoint_immediate(
    memory::StaticGuestMemory &guest, std::uint64_t key) {
  std::array<std::byte, 4> bytes{};
  const bool thumb = aarch32::key_is_thumb(key);
  const std::size_t size = thumb ? 2U : 4U;
  if (!guest.fetch(aarch32::key_pc(key),
                   std::span<std::byte>{bytes.data(), size}))
    return 0;
  std::uint32_t word = 0;
  for (std::size_t index = 0; index < size; ++index)
    word |= std::to_integer<std::uint32_t>(bytes[index]) << (8U * index);
  if (thumb)
    return word & 0xffU;
  return (word >> 4U & 0xfff0U) | (word & 0xfU);
}

template <bool PollCallback>
[[nodiscard]] ExecutionResult run_loop(JitExecutor &executor,
                                       ExecutionRequest &request,
                                       memory::StaticGuestMemory &guest) {
  if (!executor.begin_execution(request.generation,
                                aarch32::key_pc(request.cpu.pc)))
    return {.reason = ExitReason::fatal};
  struct ExecutionScope final {
    JitExecutor &executor;
    ~ExecutionScope() noexcept { executor.end_execution(); }
  };
  [[maybe_unused]] const ExecutionScope execution_scope{executor};
  executor.set_single_step(request.single_step);
  /* The host loads an AArch32 task's D registers and FPSCR before the entry
   * (they are deferred, as an A64 task's are), and the exit saves them from
   * CpuState; load them now so no block has to know whether it needs them. */
  request.cpu.access_fp_state();
  const auto interrupt_pending = [&]() noexcept {
    if (request.interrupt.state_pending())
      return true;
    if constexpr (PollCallback)
      return request.interrupt.callback_pending();
    return false;
  };
  const auto finish = [&](ExitReason reason, std::size_t retired,
                          std::uint64_t address = 0,
                          memory::MemoryAccess access =
                              memory::MemoryAccess::read,
                          std::uint32_t syndrome = 0, bool alignment = false) {
    arm64::restore_native_fp_environment(request.cpu);
    return ExecutionResult{
        .reason = reason,
        .retired_instructions = retired,
        .fault = {.address = address, .access = access, .alignment = alignment},
        .syndrome = syndrome,
    };
  };

  std::size_t retired = 0;
  try {
    for (;;) {
      if (interrupt_pending())
        return finish(ExitReason::interrupt, retired);
      /* The PC key is checked at the block boundary, as the A64 entry checks
       * its alignment: bits 1:0 of a valid key are clear, so a set bit is an
       * A32 PC that is not word aligned. The fault address is the PC. */
      const std::uint64_t key = request.cpu.pc;
      if (!aarch32::key_is_aarch32(key))
        return finish(ExitReason::fatal, retired);
      if (!aarch32::key_is_canonical(key))
        return finish(ExitReason::pc_alignment, retired, aarch32::key_pc(key),
                      memory::MemoryAccess::execute);
      std::size_t block_instructions = 0;
      EngineEvent event;
      try {
        event = executor.run_a32_block(request.cpu, guest, request.generation,
                                       block_instructions);
      } catch (...) {
        retired += block_instructions;
        return finish(ExitReason::fatal, retired);
      }
      retired += block_instructions;
      if (executor.preempted())
        return finish(ExitReason::interrupt, retired);
      if (request.boundary != nullptr &&
          request.boundary(request.cpu, request.boundary_context))
        return finish(ExitReason::system_call, retired);
      if (request.single_step && block_instructions != 0 &&
          (event == EngineEvent::continued || event == EngineEvent::interrupt))
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
                      memory::guest_access(executor.fault_access()), 0,
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
                      memory::MemoryAccess::read,
                      breakpoint_immediate(guest, request.cpu.pc));
      case EngineEvent::floating_point_trap:
        return finish(ExitReason::floating_point_trap, retired, 0,
                      memory::MemoryAccess::read,
                      executor.floating_point_syndrome());
      case EngineEvent::pc_alignment:
        return finish(ExitReason::pc_alignment, retired,
                      aarch32::key_pc(request.cpu.pc),
                      memory::MemoryAccess::execute);
      case EngineEvent::sme_access:
        return finish(ExitReason::sme_access, retired);
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

} // namespace

ExecutionResult run_a32_loop(JitExecutor &executor, ExecutionRequest &request) {
  if (request.translated_memory == nullptr ||
      request.instruction_budget == 0)
    return {.reason = request.translated_memory == nullptr
                          ? ExitReason::fatal
                          : ExitReason::instruction_budget};
  if (request.interrupt.poll != nullptr)
    return run_loop<true>(executor, request, *request.translated_memory);
  return run_loop<false>(executor, request, *request.translated_memory);
}

} // namespace uml::cpu::arm64::jit::a32
