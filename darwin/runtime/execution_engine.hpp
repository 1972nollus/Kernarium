// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace uml::runtime {

/* Host-side policy only. Linux selects no execution engine through the fixed
 * C bridge; public handles and internal tests select it through Context. */
enum class ExecutionEngine : std::uint8_t {
	automatic = 0,
	gadgets = 1,
	jit = 2,
	interpreter = 3,
};

[[nodiscard]] bool execution_engine_available(
	ExecutionEngine engine) noexcept;

/* Publishes the whole-process budget for translated-code caches, split across
 * the guest's CPUs. Startup calls this once before any engine exists.
 * Composition roots reach the CPU layer only through here.
 */
void set_translation_cache_budget(std::size_t total_bytes,
	unsigned executors) noexcept;

/* Parses a byte count with an optional K/M/G suffix. Zero if unparsable. */
[[nodiscard]] std::size_t parse_translation_cache_quantity(
	const char *text) noexcept;

} // namespace uml::runtime
