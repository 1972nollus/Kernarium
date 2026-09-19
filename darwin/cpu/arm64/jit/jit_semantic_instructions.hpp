// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/memory.hpp"

#include <cstddef>
#include <cstdint>

/*
 * The JIT's semantic slow path. Generated code leaves a fixed set of
 * instruction classes to the helpers in jit_semantic_memory.cpp: dynamic
 * cases its native memory lowering declines (unsupported shapes, tag checks,
 * misalignment, exact fault metadata), the software exclusive monitor, and
 * the pointer-authentication, guarded-control-stack, and MOPS families. Each
 * handler executes exactly one instruction of its class against CpuState and
 * a GuestMemory view of the executor's TLB.
 *
 * The class selectors recognize only the words the compiler routes to each
 * helper. Any other word has no handler, and the helper reports it as an
 * internal error instead of interpreting it.
 */

namespace uml::cpu::arm64::jit::semantic {

enum class StopReason : std::uint8_t {
  continued,
  memory_fault,
  gcs_fault,
  illegal_instruction,
  unsupported_instruction,
};

struct RunResult final {
  StopReason reason{StopReason::illegal_instruction};
  std::uint64_t address{};
  std::uint32_t instruction{};
  std::size_t instructions{};
  bool write{};
  bool alignment{};
};

/* Alignment faults precede translation and must never be repaired as missing
 * pages. Preserve the effective address without retiring the instruction. */
[[nodiscard]] constexpr RunResult alignment_fault(std::uint64_t address,
                                                  std::uint32_t instruction,
                                                  bool write) noexcept {
  return {StopReason::memory_fault, address, instruction, 0, write, true};
}

[[nodiscard]] constexpr RunResult continue_after(std::uint64_t address,
                                                 std::uint32_t instruction) {
  return {StopReason::continued, address, instruction, 1, false};
}

using Handler = RunResult (*)(CpuState &, GuestMemory &, std::uint32_t);

/* uml_arm64_jit_execute_memory: ordered and exact-fault memory forms, MOPS,
 * LS64, FEAT_LSFE atomics, and pointer authentication. */
[[nodiscard]] Handler memory_handler(std::uint32_t instruction) noexcept;
/* uml_arm64_jit_execute_exclusive: exclusives, compare-and-swap, LSE and
 * LSUI atomics, FEAT_LSE128, and read-check-write. */
[[nodiscard]] Handler exclusive_handler(std::uint32_t instruction) noexcept;
/* uml_arm64_jit_execute_ordered: the store half of the ordered scalar
 * load/store family, STLR and STLLR. */
[[nodiscard]] Handler ordered_store_handler(std::uint32_t instruction) noexcept;
/* uml_arm64_jit_execute_gcs: unprivileged guarded-control-stack operations. */
[[nodiscard]] Handler gcs_handler(std::uint32_t instruction) noexcept;

[[nodiscard]] RunResult load_store_64b(CpuState &, GuestMemory &,
                                       std::uint32_t);
[[nodiscard]] RunResult memory_copy_set(CpuState &, GuestMemory &,
                                        std::uint32_t);
[[nodiscard]] RunResult load_store_unsigned(CpuState &, GuestMemory &,
                                            std::uint32_t);
[[nodiscard]] RunResult load_literal(CpuState &, GuestMemory &,
                                     std::uint32_t);
[[nodiscard]] RunResult load_store_other(CpuState &, GuestMemory &,
                                         std::uint32_t);
[[nodiscard]] RunResult load_pointer_authenticated(CpuState &, GuestMemory &,
                                                   std::uint32_t);
[[nodiscard]] RunResult load_store_simd_ordered(CpuState &, GuestMemory &,
                                                std::uint32_t);
[[nodiscard]] RunResult load_store_exclusive(CpuState &, GuestMemory &,
                                             std::uint32_t);
[[nodiscard]] RunResult load_store_compare_swap(CpuState &, GuestMemory &,
                                                std::uint32_t);
[[nodiscard]] RunResult load_store_compare_swap_pair(CpuState &,
                                                     GuestMemory &,
                                                     std::uint32_t);
[[nodiscard]] RunResult load_store_atomic(CpuState &, GuestMemory &,
                                          std::uint32_t);
[[nodiscard]] RunResult load_store_atomic_floating(CpuState &, GuestMemory &,
                                                   std::uint32_t);
[[nodiscard]] RunResult load_store_atomic_pair_128(CpuState &, GuestMemory &,
                                                   std::uint32_t);
[[nodiscard]] RunResult load_store_rcw(CpuState &, GuestMemory &,
                                       std::uint32_t);
[[nodiscard]] RunResult load_store_rcw_pair(CpuState &, GuestMemory &,
                                            std::uint32_t);
[[nodiscard]] RunResult load_store_rcpc(CpuState &, GuestMemory &,
                                        std::uint32_t);
[[nodiscard]] RunResult load_store_ordered_pair(CpuState &, GuestMemory &,
                                                std::uint32_t);
[[nodiscard]] RunResult load_store_ordered_writeback(CpuState &,
                                                     GuestMemory &,
                                                     std::uint32_t);
[[nodiscard]] RunResult guarded_control_stack(CpuState &, GuestMemory &,
                                              std::uint32_t);
[[nodiscard]] RunResult pointer_auth_strip(CpuState &, GuestMemory &,
                                           std::uint32_t);
[[nodiscard]] RunResult pointer_auth_code(CpuState &, GuestMemory &,
                                          std::uint32_t);
[[nodiscard]] RunResult pointer_auth_generic(CpuState &, GuestMemory &,
                                             std::uint32_t);
[[nodiscard]] RunResult branch_authenticated(CpuState &, GuestMemory &,
                                             std::uint32_t);

} // namespace uml::cpu::arm64::jit::semantic
