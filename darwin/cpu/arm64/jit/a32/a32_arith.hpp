// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/cpu_state.hpp"

namespace uml::cpu::arm64::jit::a32 {

/* Whether the compiler routes the instruction to the slow helper as an
 * arithmetic form: the parallel add and subtract family, SSAT16, USAT16 and
 * USAD8/USADA8. */
[[nodiscard]] bool is_helper_arithmetic(
    const aarch32::Instruction &inst) noexcept;

/* Executes such an instruction on the register file: Rd, and GE or Q where
 * the form sets them. The caller advances the PC key and the ITSTATE. */
void run_helper_arithmetic(CpuState &cpu,
                           const aarch32::Instruction &inst) noexcept;

} // namespace uml::cpu::arm64::jit::a32
