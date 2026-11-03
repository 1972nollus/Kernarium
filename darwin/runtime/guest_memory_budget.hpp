// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* What the guest side of the process holds besides the guest's RAM, as a
 * function of the vCPU count, and the most RAM a process limit leaves a guest.
 * The gadget-cache budget a run sets, the E of the host GPU's budget
 * (platform/gpu_budget.hpp), and the bound the application's settings put on
 * the guest's RAM all come from here, so none of them states the formula
 * itself. The vCPU count the applications default to and offer comes from here
 * too (application_vcpu_limit), since this header holds the count the budget is
 * measured for.
 *
 *     E(v, G) = c(v, G) + t * v + E_rest
 *     c(v, G) = min(derive_budget(G, L), cap(v))      (the cap only where L != 0)
 *     G_max(v) = the largest G in {512, 576, ..., 4096} MiB with
 *                L - G - A - E(v, G) - S >= 384 MiB
 *
 * L is the process's memory limit (platform::process_memory_limit_bytes), zero
 * for none. cap(v) is seven eighths of the cache's measured worst ceiling at v
 * vCPUs, rounded up to 16 MiB. t is the slot tags each executor keeps outside
 * the cache's ledger. A, E_rest and S are the launch parameters
 * (launch_memory_set).
 *
 * Header-only and standard library only, so the application's loader includes
 * it too and answers the settings without the runtime, which is loaded once
 * per process for the flavour chosen at launch. The includes are relative for
 * the same reason: the application's project has no search path into darwin/.
 */

#include "../cpu/gadget_cache_budget.hpp"
#include "../platform/gpu_budget.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace uml::runtime::guest_memory {

inline constexpr std::uint64_t mebibyte = std::uint64_t{1} << 20;

/* The guest RAM a launch offers: 512 through 4096 MiB in 64 MiB steps. */
inline constexpr std::uint32_t guest_memory_minimum_mib = 512;
inline constexpr std::uint32_t guest_memory_maximum_mib = 4096;
inline constexpr std::uint32_t guest_memory_step_mib = 64;

/* The most the arm64 gadget cache may hold at 1 through 8 vCPUs: seven eighths
 * of the worst ceiling any session reached in the Android and KDE loads that
 * were measured on the Mac and the iPad, rounded up to 16 MiB. The cap sits
 * under those loads' worst ceilings, so the budget's own shedding
 * (cpu/gadget_cache_budget.hpp) bounds the cache there. Geekbench's multi-core
 * peaks at 8 vCPUs, 71 to 94 MiB, stay far below it, and forcing the budget to
 * 64 MiB there cost no measurable score. */
inline constexpr std::array<std::uint32_t, 8> gadget_cache_cap_mib{
	128, 160, 192, 224, 240, 240, 272, 272};
/* Each vCPU above the last measured one adds this to the cap. */
inline constexpr std::uint32_t gadget_cache_cap_further_executor_mib = 16;

/* The most vCPUs the table above, and with it G_max(v), is measured for. */
inline constexpr unsigned measured_vcpu_maximum =
	static_cast<unsigned>(gadget_cache_cap_mib.size());

/* The vCPU count the applications (Kernarium, Andarium and the macOS
 * launchers) default to and offer at most, on a host with `physical_processors`
 * physical cores: every core, and never more than the count the memory budget
 * is measured for. An explicit ncpus= on a launcher keeps the runtime's own
 * range; a test states its own count. A count of zero is one. */
[[nodiscard]] constexpr unsigned application_vcpu_limit(
	unsigned physical_processors) noexcept
{
	return std::clamp(physical_processors, 1U, measured_vcpu_maximum);
}

/* cap(v), in bytes; a count of zero is one. */
[[nodiscard]] constexpr std::size_t gadget_cache_cap(unsigned executors) noexcept
{
	const std::size_t measured = measured_vcpu_maximum;
	const std::size_t count = std::max<std::size_t>(executors, 1U);
	const std::size_t mib = count <= measured ?
		gadget_cache_cap_mib[count - 1U] :
		gadget_cache_cap_mib.back() +
			gadget_cache_cap_further_executor_mib * (count - measured);
	return mib * static_cast<std::size_t>(mebibyte);
}

/* The tags of an executor's 262,144 slots, 16 bytes each: 4 MiB, charged to no
 * account because the slot count fixes them. It is the default slot count
 * (UML_GADGET_CACHE_SLOTS); a build that chooses another does not change what
 * the settings assume. */
inline constexpr std::size_t slot_tag_bytes_per_executor =
	std::size_t{262144} * 16U;

/* t * v, in bytes; a count of zero is one. */
[[nodiscard]] constexpr std::size_t slot_tag_bytes(unsigned executors) noexcept
{
	return slot_tag_bytes_per_executor * std::max(executors, 1U);
}

/* c(v, G): the budget the gadget cache gets for a guest of `guest_memory`
 * bytes under `memory_limit` bytes (zero for none) at `executors` vCPUs. Where
 * the host sets no limit the budget derives from the guest alone and is not
 * capped, so a host without a limit behaves as it did before the cap existed. */
[[nodiscard]] constexpr std::size_t gadget_cache_budget(
	std::size_t guest_memory, std::size_t memory_limit,
	unsigned executors) noexcept
{
	auto budget = ::uml::cpu::gadget_cache::derive_budget(
		guest_memory, memory_limit);
	if (memory_limit != 0)
		budget = std::min(budget, gadget_cache_cap(executors));
	return budget;
}

/* E(v, G): the gadget cache in effect, the slot tags of `executors` vCPUs and
 * `engine_rest`, saturating. */
[[nodiscard]] constexpr std::uint64_t engine_memory(std::uint64_t cache_budget,
	unsigned executors, std::uint64_t engine_rest) noexcept
{
	constexpr auto unlimited = std::numeric_limits<std::uint64_t>::max();
	const std::uint64_t cache_and_tags =
		cache_budget > unlimited - slot_tag_bytes(executors) ?
		unlimited : cache_budget + slot_tag_bytes(executors);
	return cache_and_tags > unlimited - engine_rest ? unlimited :
		cache_and_tags + engine_rest;
}

/* A, E_rest and S of the host GPU's budget, in bytes: the launch parameters.
 * E_rest is the guest side's memory beyond the cache and the slot tags. */
struct LaunchMemorySet final {
	std::uint64_t application;
	std::uint64_t engine_rest;
	std::uint64_t headroom;
};

[[nodiscard]] constexpr LaunchMemorySet launch_memory_mib(
	std::uint64_t application, std::uint64_t engine_rest,
	std::uint64_t headroom) noexcept
{
	return {application * mebibyte, engine_rest * mebibyte,
		headroom * mebibyte};
}

/* The set every launch states unless it states its own: the one place the
 * product owner's choice goes. Candidates, as (A, E_rest, S) in MiB:
 *   Measured            (56, 563, 64)
 *   Measured + growth   (64, 604, 128)
 *   Conservative        (80, 644, 192)
 * None is chosen yet, so each term adds nothing. */
inline constexpr LaunchMemorySet launch_memory_set = launch_memory_mib(0, 0, 0);

/* G_max(v), in MiB: the largest guest RAM of the offered range for which
 * `memory_limit` bytes (zero for none) leave the host GPU at least its floor
 * once the guest, A, E(v, G) and S are counted. Where the host sets no limit it
 * is the whole range. Where even the smallest guest leaves less than the floor
 * it is the smallest, and the launch says the process is likely to be ended. */
[[nodiscard]] constexpr std::uint32_t maximum_guest_memory_mib(
	std::uint64_t memory_limit, unsigned executors,
	const LaunchMemorySet &set = launch_memory_set) noexcept
{
	if (memory_limit == 0)
		return guest_memory_maximum_mib;
	for (auto mib = guest_memory_maximum_mib; mib > guest_memory_minimum_mib;
	     mib -= guest_memory_step_mib) {
		const std::uint64_t guest = std::uint64_t{mib} * mebibyte;
		const auto cache = gadget_cache_budget(
			static_cast<std::size_t>(guest),
			static_cast<std::size_t>(memory_limit), executors);
		const auto budget = ::uml::platform::compute_gpu_budget({
			.process_limit = memory_limit,
			.guest_memory = guest,
			.application = set.application,
			.engine = engine_memory(cache, executors, set.engine_rest),
			.headroom = set.headroom,
		});
		if (!budget.short_of_floor)
			return mib;
	}
	return guest_memory_minimum_mib;
}

} // namespace uml::runtime::guest_memory
