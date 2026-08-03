/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/engine.hpp"

namespace uml::cpu {

[[nodiscard]] std::unique_ptr<CpuEngine> make_cpu_engine(
	EngineKind kind, EngineConfiguration configuration = {});

[[nodiscard]] bool cpu_engine_available(EngineKind kind) noexcept;

/* Materialize any lazily resident architectural state before runtime exports
 * a guest task frame. Runtime must not know which concrete engine held it. */
void materialize_architectural_state(arm64::CpuState &cpu) noexcept;

} // namespace uml::cpu
