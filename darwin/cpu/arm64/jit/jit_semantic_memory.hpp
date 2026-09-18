// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_runtime_contract.hpp"
#include "memory/tlb.hpp"

namespace uml::cpu::arm64 {

void activate_native_fp_environment(CpuState &cpu) noexcept;
void activate_native_fpmr_environment(CpuState &cpu) noexcept;

using ::uml::memory::Tlb;

extern "C" std::uint64_t
uml_arm64_jit_alignment_fault(CpuState *, Tlb *, std::uint32_t) noexcept;
extern "C" void *
uml_arm64_jit_tlb_translate(Tlb *, std::uint64_t, unsigned) noexcept;
extern "C" void *
uml_arm64_jit_tlb_translate_preserving(Tlb *, std::uint64_t,
                                       unsigned) noexcept;
extern "C" void *
uml_arm64_jit_tlb_translate_preserving_fpmr(Tlb *, std::uint64_t,
                                            unsigned) noexcept;
extern "C" std::uint64_t
uml_arm64_jit_execute_exclusive(CpuState *, Tlb *, std::uint32_t) noexcept;
extern "C" std::uint64_t
uml_arm64_jit_execute_memory(CpuState *, Tlb *, std::uint32_t) noexcept;
extern "C" std::uint64_t
uml_arm64_jit_execute_gcs(CpuState *, Tlb *, std::uint32_t) noexcept;
extern "C" std::uint64_t
uml_arm64_jit_execute_pair(CpuState *, Tlb *, std::uint32_t) noexcept;
extern "C" std::uint64_t
uml_arm64_jit_execute_ordered(CpuState *, Tlb *, std::uint32_t) noexcept;
extern "C" std::uint64_t
uml_arm64_jit_execute_simd_structure(CpuState *, Tlb *,
                                     std::uint32_t) noexcept;

} // namespace uml::cpu::arm64
