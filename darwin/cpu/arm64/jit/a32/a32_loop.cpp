// SPDX-License-Identifier: GPL-2.0-or-later

#include "a32_loop.hpp"

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/engine_execution.hpp"
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

/* What the shared entry loop (engine_execution.hpp) takes from an AArch32
 * task of the JIT: cpu.pc is a key for the whole entry, the host loads the
 * task's D registers and FPSCR before it, and a fault is reported at its
 * architectural 32-bit address. */
struct JitA32Entry : detail::EntryDefaults {
  [[nodiscard]] static constexpr std::uint64_t guest_pc(
      std::uint64_t key) noexcept {
    return aarch32::key_pc(key);
  }

  /* The PC key is checked at the block boundary, as the A64 entry checks
   * its alignment: bits 1:0 of a valid key are clear, so a set bit is an
   * A32 PC that is not word aligned. The fault address is the PC. */
  [[nodiscard]] static constexpr bool native_pc(std::uint64_t key) noexcept {
    return aarch32::key_is_aarch32(key);
  }
  [[nodiscard]] static constexpr bool aligned_pc(std::uint64_t key) noexcept {
    return aarch32::key_is_canonical(key);
  }

  /* The host loads an AArch32 task's D registers and FPSCR before the entry
   * (they are deferred, as an A64 task's are), and the exit saves them from
   * CpuState; load them now so no block has to know whether it needs them. */
  static void prepare(CpuState &cpu) { cpu.access_fp_state(); }

  [[nodiscard]] static EngineEvent run_block(
      JitExecutor &executor, CpuState &cpu, memory::StaticGuestMemory &guest,
      unsigned long generation, std::size_t &instructions) {
    return executor.run_a32_block(cpu, guest, generation, instructions);
  }

  [[nodiscard]] static std::uint32_t breakpoint_syndrome(
      JitExecutor &, const ExecutionRequest &request,
      memory::StaticGuestMemory &guest) {
    return breakpoint_immediate(guest, request.cpu.pc);
  }
};

} // namespace

ExecutionResult run_a32_loop(JitExecutor &executor, ExecutionRequest &request) {
  return detail::run_translated_entry<JitA32Entry>(executor, request);
}

} // namespace uml::cpu::arm64::jit::a32
