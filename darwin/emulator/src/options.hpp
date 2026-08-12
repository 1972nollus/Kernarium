/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "runtime/execution_engine.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace uml::emulator {

inline constexpr std::size_t default_jit_executor_cache_budget =
	64U * 1024U * 1024U;
inline constexpr std::size_t maximum_jit_executor_cache_budget =
	64U * 1024U * 1024U;

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
	unsigned guest_cpus{1};
	bool informational{};
	bool valid{true};
};

[[nodiscard]] EmulatorOptions prepare_emulator_options(
	int argc, const char *const argv[], std::string_view rootfs,
	bool default_user_network);

} // namespace uml::emulator
