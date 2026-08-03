// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../detail/fp8.hpp"
#include "../cpu_state.hpp"

#include <cstdint>

namespace uml::cpu::arm64::gadgets::software {

using Fp8Software = void (*)(CpuState *, std::uintptr_t) noexcept;

[[nodiscard]] std::uintptr_t fp8_software_operand(
	const arm64::detail::Fp8Instruction &decoded) noexcept;

[[nodiscard]] Fp8Software fp8_software_function_for(
	const arm64::detail::Fp8Instruction &decoded) noexcept;

} // namespace uml::cpu::arm64::gadgets::software
