/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine_factory.hpp"

#include "cpu/arm64/engine_adapters.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
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


bool set_translation_cache_budget(std::size_t total_bytes,
	unsigned executors, std::size_t per_executor_bytes) noexcept
{
	return arm64::set_jit_cache_budget(
		total_bytes, executors, per_executor_bytes);
}

bool acquire_translation_cache_run_session() noexcept
{
	return arm64::acquire_jit_cache_run_session();
}

void release_translation_cache_run_session() noexcept
{
	arm64::release_jit_cache_run_session();
}

std::size_t parse_translation_cache_quantity(const char *text) noexcept
{
	return arm64::parse_byte_quantity(text);
}

} // namespace uml::cpu
