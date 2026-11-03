/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "platform/gpu_budget.hpp"
#include "runtime/guest_memory_budget.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uml::emulator {

/* The launch parameters of the host GPU's fixed budget
 * (platform/gpu_budget.hpp): A, the application's expected ceiling; E_rest,
 * the guest side's other memory beyond the gadget cache and the slot tags; and
 * S, the headroom. Each defaults to the launch set
 * (runtime/guest_memory_budget.hpp), whose values are not chosen yet. */
struct GpuBudgetParameters final {
	std::uint64_t application{
		::uml::runtime::guest_memory::launch_memory_set.application};
	std::uint64_t engine_rest{
		::uml::runtime::guest_memory::launch_memory_set.engine_rest};
	std::uint64_t headroom{
		::uml::runtime::guest_memory::launch_memory_set.headroom};

	[[nodiscard]] bool operator==(const GpuBudgetParameters &) const = default;
};

/* The budget of one launch, with the figures it derives from. */
struct GpuBudgetPlan final {
	platform::GpuBudget budget;
	/* L: the process's memory limit, zero for none. */
	std::uint64_t process_limit{};
	/* G: the guest's RAM. */
	std::uint64_t guest_memory{};
	/* v: the guest's vCPUs. */
	unsigned guest_cpus{1};
	/* E: the gadget-cache budget the run will set
	 * (runtime::set_gadget_cache_budget), the slot tags of the vCPUs and
	 * E_rest (runtime/guest_memory_budget.hpp). */
	std::uint64_t engine{};
	GpuBudgetParameters parameters;
};

/* Evaluates the budget for a guest of `guest_memory` bytes and `guest_cpus`
 * vCPUs whose launch states `memory_limit_option` as the process's limit
 * (--memory-limit=, zero for none) and `gadget_cache_option` as the gadget
 * cache's budget (--gadget-cache-budget=, zero to derive it). The limit option
 * is published as the run will publish it, so the plan sees the limit the run
 * will.
 *
 * Both composition roots use this one function, before the GPU service is
 * created, so the macOS launcher and the iOS runtime cannot disagree. */
[[nodiscard]] GpuBudgetPlan plan_gpu_budget(std::uint64_t guest_memory,
	std::size_t memory_limit_option, std::size_t gadget_cache_option,
	unsigned guest_cpus, const GpuBudgetParameters &parameters);

/* The same, reading mem=, ncpus=, --memory-limit= and --gadget-cache-budget=
 * from the arguments the emulator is about to be given. */
[[nodiscard]] GpuBudgetPlan plan_gpu_budget(
	const std::vector<std::string> &arguments,
	const GpuBudgetParameters &parameters);

/* The byte count the last `option` ("--x=") argument states as the emulator
 * reads it (a number with an optional K, M or G suffix), or zero when it is
 * absent or unreadable. */
[[nodiscard]] std::size_t configured_option_bytes(
	const std::vector<std::string> &arguments, std::string_view option);

/* A launch option's byte count for the host GPU budget: a whole number with
 * an optional K, M or G suffix, where zero is a value too. False when `text`
 * is anything else. */
[[nodiscard]] bool parse_gpu_budget_bytes(
	std::string_view text, std::uint64_t &bytes) noexcept;

/* The one line a launch prints when the process has less than the floor left
 * for the GPU (GpuBudget::short_of_floor), without a trailing newline. */
[[nodiscard]] std::string describe_gpu_budget_shortfall(
	const GpuBudgetPlan &plan);

} // namespace uml::emulator
