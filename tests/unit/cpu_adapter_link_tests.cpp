// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/engine_adapters.hpp"
#include "cpu/engine_factory.hpp"

#include <memory>

#ifndef UML_ISOLATED_ENGINE_KIND
#error "UML_ISOLATED_ENGINE_KIND must select the adapter under test"
#endif

int main()
{
	std::unique_ptr<uml::cpu::CpuEngine> engine;
	uml::cpu::EngineKind expected = uml::cpu::EngineKind::interpreter;
#if UML_ISOLATED_ENGINE_KIND == 0
	engine = uml::cpu::arm64::detail::make_interpreter_engine({});
#elif UML_ISOLATED_ENGINE_KIND == 1
	expected = uml::cpu::EngineKind::gadgets;
	engine = uml::cpu::arm64::detail::make_gadget_engine({});
#elif UML_ISOLATED_ENGINE_KIND == 2
	expected = uml::cpu::EngineKind::jit;
	engine = uml::cpu::arm64::detail::make_jit_engine({});
#elif UML_ISOLATED_ENGINE_KIND == 3
	expected = uml::cpu::EngineKind::gadgets;
	engine = uml::cpu::make_cpu_engine(expected);
#else
#error "unknown isolated engine selection"
#endif
	return engine != nullptr && engine->kind() == expected ? 0 : 1;
}
