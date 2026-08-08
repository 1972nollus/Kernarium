// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "memory/tlb.hpp"

#include <cstddef>
#include <cstdint>

extern "C" std::uint32_t uml_arm64_memory_transfer(
    ::uml::memory::Tlb *tlb, std::uint64_t address, void *data,
    std::size_t size, std::uint32_t access) noexcept;

extern "C" std::uint32_t uml_arm64_simd_structure_memory_transfer(
    ::uml::cpu::arm64::CpuState *cpu, ::uml::memory::Tlb *tlb,
    std::uint64_t tagged_address, void *data, std::size_t size,
    std::uint32_t access, std::uint32_t base_sp,
    std::uint32_t options) noexcept;

extern "C" std::byte *uml_arm64_atomic_floating_pointer(
    ::uml::cpu::arm64::CpuState *cpu, ::uml::memory::Tlb *tlb,
    std::uint64_t tagged_address, std::size_t size,
    std::uint32_t base_sp) noexcept;

extern "C" void uml_arm64_alignment_fault(
    ::uml::memory::Tlb *tlb, std::uint64_t address,
    std::uint32_t access) noexcept;

extern "C" void *uml_arm64_tlb_miss(
    ::uml::memory::Tlb *tlb, std::uint64_t address,
    std::uint32_t access);
