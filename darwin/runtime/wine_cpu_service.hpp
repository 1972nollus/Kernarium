// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if DARWIN_UML_WINE_CPU_SERVICE

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/execution_types.hpp"
#include "runtime/execution_state.hpp"

#include <cstddef>

namespace uml::runtime {

enum class WineCpuHostcallDisposition {
	not_handled,
	resume,
	memory_fault,
};

struct WineCpuHostcallResult final {
	WineCpuHostcallDisposition disposition{
		WineCpuHostcallDisposition::not_handled};
	::uml::cpu::FaultInfo fault{};
	std::size_t retired_instructions{};
};

[[nodiscard]] WineCpuHostcallResult handle_wine_cpu_hostcall(
	::uml::cpu::arm64::CpuState &arm64_cpu,
	ThreadExecutionState &runtime, unsigned long generation,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt) noexcept;

} // namespace uml::runtime

#endif
