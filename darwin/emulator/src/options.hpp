/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "runtime/execution_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uml::emulator {

inline constexpr std::size_t default_jit_executor_cache_budget =
	256U * 1024U * 1024U;
inline constexpr std::size_t maximum_jit_executor_cache_budget =
	256U * 1024U * 1024U;

/*
 * Kernel command-line policy is shared by every composition root.  Keeping
 * it outside the public C ABI makes the policy testable without constructing
 * an emulator or linking a kernel image.
 */
struct EmulatorOptions final {
	std::vector<std::string> arguments;
	::uml::runtime::ExecutionEngine engine{
		::uml::runtime::ExecutionEngine::automatic};
	/* Whole-process budget for translated-code caches. Zero selects half of
	 * guest memory, which is what the guest's own mem= argument reports;
	 * --jit-cache-budget= overrides it. Private executor caches are capped
	 * separately so the shared tier receives the remaining budget.
	 */
	std::size_t jit_cache_budget{};
	std::size_t jit_executor_cache_budget{
		default_jit_executor_cache_budget};
	/* --gadget-cache-budget=: the process's byte budget for what the gadget
	 * engines keep per address space. Zero derives it from guest memory and
	 * the host's limit on the process (runtime::set_gadget_cache_budget). */
	std::size_t gadget_cache_budget{};
	/* --jit-tier=: the instructions an x86-64 address space runs on the
	 * gadget engine before the x86-64 JIT takes it over. Zero is the JIT from
	 * the first instruction. */
	std::uint64_t jit_tier_instructions{};
	std::size_t guest_memory_bytes{};
	unsigned guest_cpus{1};
	bool top_byte_ignore{};
	bool informational{};
	/* uml_dir= is the host's own, so the run prepares and removes the
	 * console's location; a caller's directory is left to the caller. */
	bool host_console_directory{};
	bool valid{true};
};

[[nodiscard]] EmulatorOptions prepare_emulator_options(
	int argc, const char *const argv[], std::string_view rootfs);

} // namespace uml::emulator
