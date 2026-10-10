// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

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
 * nothing. Only the memory and exclusive classes are routed here. */
extern "C" std::uint64_t uml_arm64_jit_a32_execute(
    CpuState *cpu, ::uml::memory::Tlb *tlb, std::uint32_t word,
    std::uint64_t key, std::uint32_t itstate) noexcept;

} // namespace uml::cpu::arm64
