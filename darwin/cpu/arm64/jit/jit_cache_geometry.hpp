// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

namespace uml::cpu::arm64 {

/*
 * How much host memory the JIT may spend, and how it is divided.
 *
 * Every structure below used to be a fixed per-executor constant, so the total
 * multiplied by the guest's CPU count with nothing bounding it: at 131072 sets
 * and a 512 MiB arena a ten-vCPU guest could reach 9 GiB of host memory before
 * counting guest RAM. Sizing from a budget makes the total the thing that is
 * chosen and the per-executor geometry the thing that is derived.
 *
 * The budget is a whole-process figure split evenly across the expected
 * executors, one per guest CPU. It defaults to half of guest memory, which the
 * emulator reads from the guest's own `mem=` argument, and
 * `--jit-cache-budget=` overrides it.
 */
struct JitCacheGeometry final {
	std::size_t cache_sets{};
	std::size_t block_links{};
	std::size_t indirect_links{};
	std::size_t arena_bytes{};
	std::size_t tracked_store_arena_bytes{};

	[[nodiscard]] std::size_t total_bytes() const noexcept;
};

/* Publishes the whole-process budget. Called once during startup, before any
 * executor exists; later calls are ignored so a running guest cannot have the
 * geometry change underneath its caches. A zero budget or zero executor count
 * keeps the built-in default.
 */
void set_jit_cache_budget(std::size_t total_bytes,
	unsigned executors) noexcept;

/* The per-executor geometry derived from the published budget. */
[[nodiscard]] JitCacheGeometry jit_cache_geometry() noexcept;

/* Parses a byte count with an optional K/M/G suffix, as used by the guest's
 * `mem=` argument and by `--jit-cache-budget=`. Returns zero if unparsable.
 */
[[nodiscard]] std::size_t parse_byte_quantity(const char *text) noexcept;

} // namespace uml::cpu::arm64
