// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/a32_encodings.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "memory/tlb.hpp"

#include <cstdint>

namespace uml::cpu::arm64 {

/* The terminal slow path of the AArch32 translation: it executes the one
 * instruction `word` (an A32 word, or hw1 << 16 | hw2 of a T32 instruction)
 * at the PC key `key`, under ITSTATE `itstate`, on the register file that the
 * generated code spilled, and returns a packed JIT result for it. The
 * instruction is either retired, with CpuState's PC key and ITSTATE
 * advanced past it or to its branch target, or it faults and changes
 * nothing. The exclusives run here; every other instruction runs in the
 * AArch32 semantic library, and the compiler routes only the memory and
 * exclusive classes and the library forms of a32_operations.hpp to it. */
extern "C" std::uint64_t uml_arm64_jit_a32_execute(
    CpuState *cpu, ::uml::memory::Tlb *tlb, std::uint32_t word,
    std::uint64_t key, std::uint32_t itstate) noexcept;

} // namespace uml::cpu::arm64

namespace uml::cpu::arm64::jit::a32 {

/* Library-exit counters, off by default: with counting enabled every call of
 * uml_arm64_jit_a32_execute adds to the count of the form it executed, so
 * that a test can require a lowered form to stay inline from a clean state
 * (the SVE counters' counterpart). */
void count_helper_exits(bool enable) noexcept;
[[nodiscard]] std::uint64_t helper_exits(aarch32::Form form) noexcept;
[[nodiscard]] std::uint64_t helper_exit_total() noexcept;
void reset_helper_exits() noexcept;

} // namespace uml::cpu::arm64::jit::a32
