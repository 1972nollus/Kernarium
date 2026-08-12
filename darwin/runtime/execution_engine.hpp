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

/* The maintained kernel clamps ncpus= to CONFIG_NR_CPUS=64. Keep the host's
 * cache-budget divisor on that same effective range.
 */
inline constexpr unsigned maximum_guest_cpu_count = 64;

[[nodiscard]] bool execution_engine_available(
	ExecutionEngine engine) noexcept;

/* Publishes the whole-process budget for translated-code caches, split across
 * the guest's CPUs. The first valid pair owns process-wide geometry; identical
 * repeats succeed and incompatible later runs fail. Composition roots reach
 * the CPU layer only through here.
 */
[[nodiscard]] bool set_translation_cache_budget(std::size_t total_bytes,
	unsigned executors) noexcept;

[[nodiscard]] bool acquire_translation_cache_run_session() noexcept;
void release_translation_cache_run_session() noexcept;

/* Parses a byte count with an optional K/M/G suffix. Zero if unparsable. */
[[nodiscard]] std::size_t parse_translation_cache_quantity(
	const char *text) noexcept;

/* The maintained Darwin kernel selects the online host processor count when
 * ncpus= is absent, capped by CONFIG_NR_CPUS. The emulator uses the same value
 * before constructing per-vCPU translation caches.
 */
[[nodiscard]] unsigned default_guest_cpu_count() noexcept;

} // namespace uml::runtime
