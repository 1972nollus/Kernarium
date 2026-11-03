// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The host GPU's budget as a launch plans it: the arguments it is given, the
 * gadget cache the run will set, and the limit the launch states. The macOS
 * launcher and the iOS runtime both ask this one function.
 */
#include "gpu_budget_plan.hpp"
#include "named_tests.hpp"
#include "options.hpp"
#include "platform/host_environment.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using uml::testing::require;
namespace emulator = uml::emulator;
namespace platform = uml::platform;

constexpr std::uint64_t mib = 1024ULL * 1024ULL;

/* Leaves the environment and the launch option as the case found them. */
class LimitScope final {
public:
	LimitScope()
	{
		::unsetenv(platform::memory_limit_variable);
		platform::set_configured_process_memory_limit(0);
	}

	~LimitScope()
	{
		platform::set_configured_process_memory_limit(0);
		::unsetenv(platform::memory_limit_variable);
	}

	LimitScope(const LimitScope &) = delete;
	LimitScope &operator=(const LimitScope &) = delete;
};

void aLaunchParameterIsANumberWithAnOptionalSuffix()
{
	std::uint64_t bytes = 7;
	require(emulator::parse_gpu_budget_bytes("0", bytes) && bytes == 0);
	require(emulator::parse_gpu_budget_bytes("512", bytes) && bytes == 512);
	require(emulator::parse_gpu_budget_bytes("4k", bytes) && bytes == 4096);
	require(emulator::parse_gpu_budget_bytes("300M", bytes) &&
		bytes == 300 * mib);
	require(emulator::parse_gpu_budget_bytes("2g", bytes) &&
		bytes == 2048 * mib);
	bytes = 7;
	for (const char *refused : {"", "M", "-1", "1.5M", "12MB", "0x10", " 5",
		     "99999999999999999999", "18446744073709551615G"})
		require(!emulator::parse_gpu_budget_bytes(refused, bytes), refused);
	require(bytes == 7, "a refused parameter leaves the value alone");
}

void theArgumentsStateTheLimitAndTheCache()
{
	const LimitScope scope;
	const std::vector<std::string> arguments{
		"uml-darwin", "mem=1536M", "--memory-limit=2867M",
		"--gadget-cache-budget=300M", "ncpus=4"};
	require(emulator::configured_option_bytes(arguments, "--memory-limit=") ==
		2867 * mib);
	require(emulator::configured_option_bytes(arguments,
			"--gadget-cache-budget=") == 300 * mib);
	require(emulator::configured_option_bytes(arguments, "--jit-tier=") == 0);

	const auto plan = emulator::plan_gpu_budget(arguments, {});
	require(plan.guest_memory == 1536 * mib);
	require(plan.guest_cpus == 4);
	require(plan.process_limit == 2867 * mib);
	require(plan.engine == 316 * mib,
		"the cache the launch states and four vCPUs' slot tags");
	require(plan.budget.available == 2867 * mib - 1536 * mib - 316 * mib);
	require(plan.budget.bytes == 768 * mib);
	require(!plan.budget.short_of_floor);
}

void theVCPUCountComesFromTheLastValidArgument()
{
	require(emulator::configured_guest_cpu_count({"uml-darwin", "ncpus=4"}) ==
		4);
	require(emulator::configured_guest_cpu_count(
			{"uml-darwin", "ncpus=2", "ncpus=6"}) == 6);
	require(emulator::configured_guest_cpu_count(
			{"uml-darwin", "ncpus=3", "ncpus=lots"}) == 3,
		"a malformed value leaves the valid one");
	require(emulator::configured_guest_cpu_count(
			{"uml-darwin", "ncpus=9999"}) ==
			uml::runtime::maximum_guest_cpu_count,
		"clamped to the guest maximum");
	require(emulator::configured_guest_cpu_count({"uml-darwin"}) >= 1,
		"the host's default");
}

/* Without a cache option the run derives the budget from the guest, the limit
 * and the vCPU count; the plan counts that figure, not its own. */
void theDerivedGadgetCacheCountsAsEngineMemory()
{
	const LimitScope scope;
	const auto planned = [](const char *limit, const char *cpus) {
		return emulator::plan_gpu_budget(
			std::vector<std::string>{"uml-darwin", "mem=1536M", limit,
				cpus},
			{});
	};
	/* A quarter of 1536 MiB is 384, and an eighth of the limit is 358: one
	 * vCPU's cap is under both, and sixteen vCPUs' above both. */
	const auto one = planned("--memory-limit=2867M", "ncpus=1");
	require(one.engine == (128 + 4) * mib, "one vCPU's cap and slot tags");
	const auto sixteen = planned("--memory-limit=2867M", "ncpus=16");
	require(sixteen.engine == 2867 * mib / 8 + 64 * mib,
		"an eighth of the limit and sixteen vCPUs' slot tags");
	const auto roomy = planned("--memory-limit=8192M", "ncpus=16");
	require(roomy.engine == (384 + 64) * mib, "a quarter of the guest");
	const auto roomy_one = planned("--memory-limit=8192M", "ncpus=1");
	require(roomy_one.engine == (128 + 4) * mib, "one vCPU's cap");
}

/* The safe RAM of the application's settings is the largest guest for which
 * this plan is not short of the floor: the plan and the bound agree. */
void thePlanAgreesWithTheMaximumGuestMemory()
{
	const LimitScope scope;
	if (platform::process_memory_limit_bytes() != 0)
		return; /* a host that sets its own limit */
	for (const unsigned cpus : {1U, 2U, 3U, 4U, 6U, 8U}) {
		const auto maximum =
			uml::runtime::guest_memory::maximum_guest_memory_mib(
				2867 * mib, cpus);
		const auto at = emulator::plan_gpu_budget(
			std::uint64_t{maximum} * mib, 2867 * mib, 0, cpus, {});
		require(at.process_limit == 2867 * mib);
		require(!at.budget.short_of_floor, "the maximum fits");
		require(at.budget.available >= 384 * mib);
		const auto above = emulator::plan_gpu_budget(
			std::uint64_t{maximum + 64U} * mib, 2867 * mib, 0, cpus, {});
		require(above.budget.short_of_floor,
			"the next size up is short of the floor");
	}
}

void theVCPUCountMovesTheBudget()
{
	const LimitScope scope;
	if (platform::process_memory_limit_bytes() != 0)
		return; /* a host that sets its own limit */
	const auto plan = [](unsigned cpus) {
		return emulator::plan_gpu_budget(2240 * mib, 2867 * mib, 0, cpus,
			{});
	};
	/* Two vCPUs: a 160 MiB cache and 8 MiB of tags leave 459 MiB. */
	require(plan(2).engine == 168 * mib);
	require(plan(2).budget.available == 459 * mib);
	require(!plan(2).budget.short_of_floor);
	/* Eight: 272 MiB and 32 MiB leave 323, under the floor. */
	require(plan(8).engine == 304 * mib);
	require(plan(8).budget.available == 323 * mib);
	require(plan(8).budget.short_of_floor);
	require(plan(8).budget.bytes == 384 * mib);
}

void theLaunchParametersDefaultToTheLaunchSet()
{
	const emulator::GpuBudgetParameters defaults;
	const auto &set = uml::runtime::guest_memory::launch_memory_set;
	require(defaults.application == set.application);
	require(defaults.engine_rest == set.engine_rest);
	require(defaults.headroom == set.headroom);
}

void theLaunchParametersAreSubtracted()
{
	const LimitScope scope;
	const std::vector<std::string> arguments{"uml-darwin", "mem=1536M",
		"--memory-limit=2867M", "--gadget-cache-budget=358M", "ncpus=2"};
	const auto plan = emulator::plan_gpu_budget(arguments,
		{.application = 300 * mib, .engine_rest = 50 * mib,
			.headroom = 100 * mib});
	require(plan.engine == 416 * mib, "E is the cache, the tags and E_rest");
	require(plan.budget.available ==
		(2867 - 1536 - 416 - 300 - 100) * mib);
	require(plan.budget.bytes == 515 * mib, "the remainder");
	require(!plan.budget.short_of_floor);

	const auto exact = emulator::plan_gpu_budget(arguments,
		{.application = 300 * mib, .engine_rest = 50 * mib,
			.headroom = 231 * mib});
	require(exact.budget.available == 384 * mib);
	require(exact.budget.bytes == 384 * mib, "the floor");
	require(!exact.budget.short_of_floor, "the floor itself is not short");

	const auto starved = emulator::plan_gpu_budget(arguments,
		{.application = 400 * mib, .engine_rest = 50 * mib,
			.headroom = 200 * mib});
	require(starved.budget.short_of_floor);
	require(starved.budget.bytes == 384 * mib);
	const auto line = emulator::describe_gpu_budget_shortfall(starved);
	require(line.find('\n') == std::string::npos, "one line");
	require(line.find("2867 MiB") != std::string::npos, line.c_str());
	require(line.find("384 MiB") != std::string::npos, line.c_str());
}

void noLimitLeavesHalfTheGuest()
{
	const LimitScope scope;
	if (platform::process_memory_limit_bytes() != 0)
		return; /* a host that sets its own limit */
	const auto plan = emulator::plan_gpu_budget(
		std::vector<std::string>{"uml-darwin", "mem=2048M"},
		{.application = 500 * mib, .headroom = 500 * mib});
	require(plan.process_limit == 0);
	require(plan.budget.bytes == 1024 * mib);
	require(!plan.budget.short_of_floor);
}

} // namespace

int main()
{
	static constexpr uml::testing::NamedTest tests[] {
		{"a-launch-parameter-is-a-number-with-an-optional-suffix",
			aLaunchParameterIsANumberWithAnOptionalSuffix},
		{"the-arguments-state-the-limit-and-the-cache",
			theArgumentsStateTheLimitAndTheCache},
		{"the-vcpu-count-comes-from-the-last-valid-argument",
			theVCPUCountComesFromTheLastValidArgument},
		{"the-derived-gadget-cache-counts-as-engine-memory",
			theDerivedGadgetCacheCountsAsEngineMemory},
		{"the-plan-agrees-with-the-maximum-guest-memory",
			thePlanAgreesWithTheMaximumGuestMemory},
		{"the-vcpu-count-moves-the-budget", theVCPUCountMovesTheBudget},
		{"the-launch-parameters-default-to-the-launch-set",
			theLaunchParametersDefaultToTheLaunchSet},
		{"the-launch-parameters-are-subtracted",
			theLaunchParametersAreSubtracted},
		{"no-limit-leaves-half-the-guest", noLimitLeavesHalfTheGuest},
	};
	return uml::testing::runNamedTests(tests);
}
