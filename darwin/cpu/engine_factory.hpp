/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/engine.hpp"

#include <cstddef>

namespace uml::cpu {

[[nodiscard]] std::unique_ptr<CpuEngine> make_cpu_engine(
	EngineKind kind, EngineConfiguration configuration = {});

[[nodiscard]] bool cpu_engine_available(EngineKind kind) noexcept;

/* Publishes the whole-process budget for translated-code caches, split across
 * the guest's CPUs. Startup calls this once, before any engine exists; see
 * arm64/jit/jit_cache_geometry.hpp for what the budget buys.
 */
void set_translation_cache_budget(std::size_t total_bytes,
	unsigned executors) noexcept;

/* Parses a byte count with an optional K/M/G suffix. Zero if unparsable. */
[[nodiscard]] std::size_t parse_translation_cache_quantity(
	const char *text) noexcept;

/* Materialize any lazily resident architectural state before runtime exports
 * a guest task frame. Runtime must not know which concrete engine held it. */
void materialize_architectural_state(arm64::CpuState &cpu) noexcept;

} // namespace uml::cpu
