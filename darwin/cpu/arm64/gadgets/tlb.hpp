// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "memory/tlb.hpp"

namespace uml::cpu::arm64 {
struct CpuState;
}

extern "C" std::uint32_t uml_arm64_guarded_call_record(
	uml::cpu::arm64::CpuState *cpu,
	::uml::memory::Tlb *tlb,
	std::uint64_t return_address) noexcept;

extern "C" std::uint32_t uml_arm64_guarded_return_record(
	uml::cpu::arm64::CpuState *cpu,
	::uml::memory::Tlb *tlb,
	std::uint64_t target) noexcept;
