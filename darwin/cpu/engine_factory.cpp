/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine_factory.hpp"

#include "cpu/arm64/engine_adapters.hpp"
#include "cpu/arm64/fp_environment.hpp"

namespace uml::cpu {

std::unique_ptr<CpuEngine> make_cpu_engine(
	EngineKind kind, EngineConfiguration configuration)
{
	switch (kind) {
	case EngineKind::interpreter:
		return arm64::detail::make_interpreter_engine(configuration);
	case EngineKind::gadgets:
		return arm64::detail::make_gadget_engine(configuration);
	case EngineKind::jit:
		return arm64::detail::make_jit_engine(configuration);
	}
	return nullptr;
}

bool cpu_engine_available(EngineKind kind) noexcept
{
	switch (kind) {
	case EngineKind::interpreter:
	case EngineKind::gadgets:
		return true;
	case EngineKind::jit:
		return arm64::detail::native_jit_engine_available();
	}
	return false;
}

void materialize_architectural_state(arm64::CpuState &cpu) noexcept
{
	arm64::restore_native_fp_environment(cpu);
}

} // namespace uml::cpu
