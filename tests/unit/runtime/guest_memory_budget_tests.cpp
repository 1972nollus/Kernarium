// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What the guest side of the process holds beside the guest's RAM as the vCPU
 * count grows, the most RAM a process limit leaves a guest and the RAM a new
 * guest gets: the cache cap per vCPU count, the slot tags, E(v, G), G_max(v)
 * and the default at the iPad mini 6's 2,867 MiB and other limits, under the
 * launch set and the maximum set. The expected rows were derived on their own,
 * in integer bytes, from the measured ceilings (see the table comment) and by
 * the search in theDefaultIsTheLargestStepThatLeavesTheFloor.
 */
#include "named_tests.hpp"
#include "require.hpp"
#include "runtime/guest_memory_budget.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace {

using uml::testing::require;
namespace budget = uml::runtime::guest_memory;

constexpr std::uint64_t mib = 1024ULL * 1024ULL;
constexpr std::uint64_t ipad_limit = 2867 * mib;

/* The set that counts none of A, E_rest and S, and the conservative launch
 * set, as (A, E_rest, S) in MiB: S is a transient of 45, 40 past the GPU's
 * pool and 64 of growth. */
constexpr auto no_terms = budget::launch_memory_mib(0, 0, 0);
constexpr auto conservative = budget::launch_memory_mib(64, 320, 45 + 40 + 64);
/* Three other sets, to hold the formula to its shape for any terms. */
constexpr auto light_set = budget::launch_memory_mib(56, 563, 64);
constexpr auto middle_set = budget::launch_memory_mib(64, 604, 128);
constexpr auto heavy_set = budget::launch_memory_mib(80, 644, 192);
constexpr std::array all_sets{no_terms, conservative, light_set, middle_set,
	heavy_set};

/* The worst gadget-cache ceiling of any measured session at 1..8 vCPUs, MiB. */
constexpr std::array<double, 8> measured_ceiling_mib{
	135.3, 173.6, 208.0, 244.9, 262.9, 267.5, 299.3, 298.3};

constexpr std::array<std::uint32_t, 8> expected_cap_mib{
	128, 160, 192, 224, 240, 240, 272, 272};

using Row = std::array<std::uint32_t, 8>;

Row maximumRow(std::uint64_t limit, const budget::LaunchMemorySet &set)
{
	Row row{};
	for (unsigned executors = 1; executors <= row.size(); ++executors)
		row[executors - 1] =
			budget::maximum_guest_memory_mib(limit, executors, set);
	return row;
}

void theCapIsSevenEighthsOfTheMeasuredCeilingRoundedUp()
{
	for (unsigned executors = 1; executors <= 8; ++executors) {
		const double ceiling = measured_ceiling_mib[executors - 1];
		const double seven_eighths = ceiling * 7.0 / 8.0;
		auto rounded = static_cast<std::uint32_t>(seven_eighths);
		if (static_cast<double>(rounded) < seven_eighths)
			++rounded;
		const std::uint32_t sixteens = (rounded + 15U) / 16U * 16U;
		require(sixteens == expected_cap_mib[executors - 1],
			"cap = 7/8 of the ceiling up to 16 MiB");
		require(budget::gadget_cache_cap(executors) ==
			expected_cap_mib[executors - 1] * mib);
		require(budget::gadget_cache_cap(executors) <
			static_cast<std::size_t>(ceiling * static_cast<double>(mib)),
			"under every measured ceiling, so it binds there");
	}
}

void aCountOfZeroIsOneAndFurtherVCPUsAddSixteenEach()
{
	require(budget::gadget_cache_cap(0) == budget::gadget_cache_cap(1));
	require(budget::gadget_cache_cap(9) == 288 * mib);
	require(budget::gadget_cache_cap(10) == 304 * mib);
	require(budget::gadget_cache_cap(64) == (272 + 16 * 56) * mib);
	for (unsigned executors = 1; executors < 64; ++executors)
		require(budget::gadget_cache_cap(executors) <=
			budget::gadget_cache_cap(executors + 1), "never falls");
}

void theSlotTagsAreFourMebibytesEach()
{
	require(budget::slot_tag_bytes_per_executor == 4 * mib);
	require(budget::slot_tag_bytes(1) == 4 * mib);
	require(budget::slot_tag_bytes(6) == 24 * mib);
	require(budget::slot_tag_bytes(0) == 4 * mib, "zero counts as one");
}

/* The cap bounds the cache only where the host sets a limit. */
void theCacheBudgetIsCappedOnlyUnderALimit()
{
	constexpr std::size_t gib = std::size_t{1} << 30;
	/* No limit: a quarter of the guest, uncapped, as before the cap. */
	require(budget::gadget_cache_budget(8 * gib, 0, 1) == 2 * gib);
	require(budget::gadget_cache_budget(512 * mib, 0, 8) == 128 * mib);
	/* A roomy limit leaves the cap as the bound: 1024 MiB at 4096 MiB. */
	require(budget::gadget_cache_budget(4096 * mib, 64 * gib, 1) == 128 * mib);
	require(budget::gadget_cache_budget(4096 * mib, 64 * gib, 8) == 272 * mib);
	/* An eighth of 2867 MiB is 358.375, above the cap up to 8 vCPUs. */
	for (unsigned executors = 1; executors <= 8; ++executors)
		require(budget::gadget_cache_budget(2240 * mib, ipad_limit,
				executors) == expected_cap_mib[executors - 1] * mib);
	/* The cap is no floor: a small guest keeps its 128 MiB at any count, a
	 * tight limit its eighth. */
	require(budget::gadget_cache_budget(512 * mib, ipad_limit, 8) == 128 * mib);
	require(budget::gadget_cache_budget(512 * mib, ipad_limit, 1) == 128 * mib);
	require(budget::gadget_cache_budget(2048 * mib, 800 * mib, 8) == 100 * mib);
	/* More vCPUs than the table measured raise the cap, then the eighth of
	 * the limit bounds the cache. */
	require(budget::gadget_cache_budget(4096 * mib, 64 * gib, 12) ==
		(272 + 16 * 4) * mib);
}

void theEngineIsTheCacheTheSlotTagsAndTheRest()
{
	require(budget::engine_memory(300 * mib, 4, 0) == 316 * mib);
	require(budget::engine_memory(300 * mib, 4, 604 * mib) == 920 * mib);
	require(budget::engine_memory(0, 0, 0) == 4 * mib, "a count of zero is one");
	constexpr auto unlimited = std::numeric_limits<std::uint64_t>::max();
	require(budget::engine_memory(unlimited, 2, 1) == unlimited);
	require(budget::engine_memory(1, 2, unlimited) == unlimited);
	require(budget::engine_memory(unlimited - 4 * mib, 1, 0) == unlimited);
}

/* The launch set is the conservative one the default and the GPU's budget are
 * measured with; the maximum set counts none of A, E_rest and S. */
void theLaunchSetIsConservativeAndTheMaximumSetCountsNone()
{
	require(budget::launch_memory_set.application == 64 * mib);
	require(budget::launch_memory_set.engine_rest == 320 * mib);
	require(budget::launch_memory_set.headroom == 149 * mib);
	require(budget::maximum_memory_set.application == 0);
	require(budget::maximum_memory_set.engine_rest == 0);
	require(budget::maximum_memory_set.headroom == 0);
}

void theMaximumOnAnIPadFollowsTheVCPUCountForEverySet()
{
	require(maximumRow(ipad_limit, no_terms) ==
		Row{2304, 2304, 2240, 2240, 2176, 2176, 2176, 2176}, "zero set");
	require(maximumRow(ipad_limit, conservative) ==
		Row{1792, 1728, 1728, 1664, 1664, 1664, 1600, 1600},
		"launch set");
	require(maximumRow(ipad_limit, light_set) ==
		Row{1664, 1600, 1536, 1536, 1536, 1536, 1472, 1472}, "light");
	require(maximumRow(ipad_limit, middle_set) ==
		Row{1536, 1472, 1472, 1408, 1408, 1408, 1344, 1344}, "middle");
	require(maximumRow(ipad_limit, heavy_set) ==
		Row{1408, 1344, 1344, 1280, 1280, 1280, 1216, 1216}, "heavy");
	/* The maximum a person may choose counts none of A, E_rest and S, so
	 * it is no lower than the launch set's and the default argument. */
	require(budget::maximum_guest_memory_mib(ipad_limit, 6) ==
		budget::maximum_guest_memory_mib(ipad_limit, 6, no_terms));
	require(budget::maximum_guest_memory_mib(ipad_limit, 6) == 2176,
		"2048 MiB stays selectable on an iPad mini 6");
	require(budget::maximum_guest_memory_mib(ipad_limit, 6) >
		budget::maximum_guest_memory_mib(ipad_limit, 6,
			budget::launch_memory_set));
	/* The same evaluation as a constant expression. */
	static_assert(budget::maximum_guest_memory_mib(ipad_limit, 2,
			middle_set) == 1472);
	static_assert(budget::maximum_guest_memory_mib(ipad_limit, 6) == 2176);
}

/* Every maximum leaves the host GPU its floor at its own size and not at the
 * next one up, the invariant G_max is defined by. */
void theMaximumIsTheLargestSizeThatLeavesTheFloor()
{
	for (const auto &set : all_sets)
		for (unsigned executors = 1; executors <= 8; ++executors) {
			const auto fits = [&](std::uint32_t guest_mib) {
				const std::uint64_t guest = guest_mib * mib;
				return !uml::platform::compute_gpu_budget({
					.process_limit = ipad_limit,
					.guest_memory = guest,
					.application = set.application,
					.engine = budget::engine_memory(
						budget::gadget_cache_budget(guest,
							ipad_limit, executors),
						executors, set.engine_rest),
					.headroom = set.headroom,
				}).short_of_floor;
			};
			const auto maximum = budget::maximum_guest_memory_mib(
				ipad_limit, executors, set);
			require(maximum % 64U == 0);
			require(fits(maximum));
			if (maximum < 4096)
				require(!fits(maximum + 64U),
					"the next size up is short of the floor");
		}
}

void aMoreVCPUsNeverRaiseTheMaximum()
{
	for (const auto &set : all_sets)
		for (std::uint64_t limit = 1024; limit <= 6144; limit += 256)
			for (unsigned executors = 1; executors < 16; ++executors)
				require(budget::maximum_guest_memory_mib(
						limit * mib, executors + 1, set) <=
					budget::maximum_guest_memory_mib(
						limit * mib, executors, set));
}

void noLimitOffersTheWholeRange()
{
	for (const auto &set : all_sets)
		for (unsigned executors : {0U, 1U, 4U, 8U, 64U})
			require(budget::maximum_guest_memory_mib(0, executors,
					set) == 4096);
}

/* A limit that leaves even the smallest guest less than the floor still
 * offers the smallest; the launch warns, as it does today. */
void aTightLimitOffersTheSmallestGuest()
{
	require(budget::maximum_guest_memory_mib(512 * mib, 4) == 512);
	require(budget::maximum_guest_memory_mib(1, 1) == 512);
	require(budget::maximum_guest_memory_mib(1024 * mib, 8,
			heavy_set) == 512);
	/* A roomier limit offers more, and steps down with the vCPU count. */
	require(budget::maximum_guest_memory_mib(4096 * mib, 1) == 3520);
	require(budget::maximum_guest_memory_mib(4096 * mib, 8) == 3392);
}

using DefaultRow = std::array<std::uint32_t, 8>;

DefaultRow defaultRow(std::uint64_t limit)
{
	DefaultRow row{};
	for (unsigned executors = 1; executors <= row.size(); ++executors)
		row[executors - 1] = budget::default_guest_memory_mib(limit, executors);
	return row;
}

/* An iPad mini 6 starts a guest at 1.5 GiB at every vCPU count: the launch
 * set's G_max(v), 1792 down to 1600 MiB, rounds down to 1536 and the range's
 * bottom holds it there. The guest of 2048 MiB the maximum still offers
 * (2176 at six vCPUs) is above it. */
void theDefaultOnAnIPadMiniIsOneAndAHalfGiBAtEveryVCPUCount()
{
	require(defaultRow(ipad_limit) ==
		DefaultRow{1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536});
	for (unsigned executors = 1; executors <= 8; ++executors) {
		require(budget::default_guest_memory_mib(ipad_limit, executors) <
			2048, "2048 MiB is above the default");
		require(budget::maximum_guest_memory_mib(ipad_limit, executors) >=
			2048, "and still selectable");
		require(budget::default_guest_memory_mib(ipad_limit, executors,
				conservative) ==
			budget::default_guest_memory_mib(ipad_limit, executors),
			"the launch set is the default argument");
	}
	static_assert(budget::default_guest_memory_mib(ipad_limit, 6) == 1536);
}

/* Other limits, with the advertised limit as the only input: the launch set's
 * G_max(v) rounded down to 512 MiB, held within 1536 to 4096, and never above
 * the maximum the settings offer. */
void theDefaultFollowsTheLimitInHalfGiBSteps()
{
	for (const auto &[limit_mib, expected] : {
		     std::pair{3584U, DefaultRow{2048, 2048, 2048, 2048, 2048,
				     2048, 2048, 2048}},
		     std::pair{4096U, DefaultRow{2560, 2560, 2560, 2560, 2560,
				     2560, 2560, 2560}},
		     std::pair{5120U, DefaultRow{3584, 3584, 3584, 3584, 3584,
				     3584, 3584, 3584}},
		     std::pair{6144U, DefaultRow{4096, 4096, 4096, 4096, 4096,
				     4096, 4096, 4096}},
		     std::pair{8192U, DefaultRow{4096, 4096, 4096, 4096, 4096,
				     4096, 4096, 4096}},
		     /* The range's bottom holds 1792..1600 down at 1536. */
		     std::pair{2304U, DefaultRow{1536, 1536, 1536, 1536, 1536,
				     1536, 1536, 1536}},
		     /* A limit that leaves less than the bottom of the range:
		      * the maximum the settings offer wins, on 64 MiB steps. */
		     std::pair{2048U, DefaultRow{1472, 1472, 1408, 1408, 1344,
				     1344, 1344, 1344}},
		     std::pair{1536U, DefaultRow{960, 960, 896, 896, 896, 896,
				     896, 896}},
	     })
		require(defaultRow(limit_mib * mib) == expected,
			std::to_string(limit_mib).c_str());
	/* No limit: the whole range, whatever the vCPU count. */
	for (unsigned executors : {0U, 1U, 4U, 8U, 64U})
		require(budget::default_guest_memory_mib(0, executors) == 4096);
}

/* The default is the largest multiple of 512 MiB for which the launch set
 * leaves the host GPU its floor, searched here from the sums in bytes rather
 * than from G_max, then held within 1536 to 4096 and under the maximum, which
 * is searched the same way with the set that counts none. */
void theDefaultIsTheLargestStepThatLeavesTheFloor()
{
	const auto fits = [](std::uint64_t limit, unsigned executors,
				  std::uint32_t guest_mib,
				  const budget::LaunchMemorySet &set) {
		const std::uint64_t guest = guest_mib * mib;
		return !uml::platform::compute_gpu_budget({
			.process_limit = limit,
			.guest_memory = guest,
			.application = set.application,
			.engine = budget::engine_memory(
				budget::gadget_cache_budget(guest, limit, executors),
				executors, set.engine_rest),
			.headroom = set.headroom,
		}).short_of_floor;
	};
	for (std::uint64_t limit_mib = 1024; limit_mib <= 8192; limit_mib += 128)
		for (unsigned executors = 1; executors <= 8; ++executors) {
			const std::uint64_t limit = limit_mib * mib;
			std::uint32_t stepped = 512;
			for (std::uint32_t guest = 512; guest <= 4096; guest += 512)
				if (fits(limit, executors, guest, conservative))
					stepped = guest;
			std::uint32_t most = 512;
			for (std::uint32_t guest = 512; guest <= 4096; guest += 64)
				if (fits(limit, executors, guest, no_terms))
					most = guest;
			const auto expected = std::min(
				std::clamp(stepped, 1536U, 4096U), most);
			const auto actual =
				budget::default_guest_memory_mib(limit, executors);
			require(actual == expected, std::to_string(limit_mib).c_str());
			require(actual <= budget::maximum_guest_memory_mib(limit, executors));
			require(actual % 64U == 0, "on the settings' steps");
			if (actual < most)
				require(actual % 512U == 0,
					"whole half gibibytes wherever the maximum did not bind");
			if (stepped >= 1536)
				require(fits(limit, executors, actual, conservative),
					"the launch set leaves the GPU its floor at the default");
		}
}

/* More vCPUs hold more of the process, so the default never rises with them. */
void aMoreVCPUsNeverRaiseTheDefault()
{
	for (std::uint64_t limit = 1024; limit <= 8192; limit += 128)
		for (unsigned executors = 1; executors < 16; ++executors)
			require(budget::default_guest_memory_mib(limit * mib,
					executors + 1) <=
				budget::default_guest_memory_mib(limit * mib, executors));
}

/* The warning threshold the applications use is the default itself: a guest
 * above it is the one the process is likely to be ended with. On an iPad mini 6
 * that is every size from 1600 MiB up, 2048 MiB included, and none of the
 * sizes up to the default. */
void aGuestAboveTheDefaultIsWhatTheApplicationsWarnAbout()
{
	for (unsigned executors = 1; executors <= 8; ++executors) {
		const auto warn_from =
			budget::default_guest_memory_mib(ipad_limit, executors);
		const auto maximum =
			budget::maximum_guest_memory_mib(ipad_limit, executors);
		require(warn_from == 1536);
		require(maximum > warn_from, "there are sizes to warn about");
		require(2048 > warn_from && 2048 <= maximum);
	}
}

/* The applications default to and offer every physical core, at most the
 * count the cache cap and G_max are measured for. */
void theApplicationLimitIsThePhysicalCoresAtMostTheMeasuredCount()
{
	static_assert(budget::measured_vcpu_maximum == 8);
	static_assert(budget::measured_vcpu_maximum ==
		budget::gadget_cache_cap_mib.size());
	/* An iPad mini 6, an eight-core host, the ten-core devices and a Mac
	 * with twenty. */
	require(budget::application_vcpu_limit(6) == 6);
	require(budget::application_vcpu_limit(8) == 8);
	require(budget::application_vcpu_limit(10) == 8);
	require(budget::application_vcpu_limit(20) == 8);
	require(budget::application_vcpu_limit(1) == 1);
	require(budget::application_vcpu_limit(0) == 1, "zero counts as one");
	require(budget::application_vcpu_limit(
			std::numeric_limits<unsigned>::max()) == 8);
	for (unsigned physical = 0; physical <= 64; ++physical) {
		const auto limit = budget::application_vcpu_limit(physical);
		require(limit >= 1 && limit <= budget::measured_vcpu_maximum);
		require(limit <= std::max(physical, 1U));
		/* No limit lies beyond a measured row of the cap and of G_max. */
		require(budget::gadget_cache_cap(limit) ==
			budget::gadget_cache_cap_mib[limit - 1] * mib);
	}
}

} // namespace

int main()
{
	static constexpr uml::testing::NamedTest tests[] {
		{"the-cap-is-seven-eighths-of-the-measured-ceiling-rounded-up",
			theCapIsSevenEighthsOfTheMeasuredCeilingRoundedUp},
		{"a-count-of-zero-is-one-and-further-vcpus-add-sixteen-each",
			aCountOfZeroIsOneAndFurtherVCPUsAddSixteenEach},
		{"the-slot-tags-are-four-mebibytes-each",
			theSlotTagsAreFourMebibytesEach},
		{"the-cache-budget-is-capped-only-under-a-limit",
			theCacheBudgetIsCappedOnlyUnderALimit},
		{"the-engine-is-the-cache-the-slot-tags-and-the-rest",
			theEngineIsTheCacheTheSlotTagsAndTheRest},
		{"the-launch-set-is-conservative-and-the-maximum-set-counts-none",
			theLaunchSetIsConservativeAndTheMaximumSetCountsNone},
		{"the-maximum-on-an-ipad-follows-the-vcpu-count-for-every-set",
			theMaximumOnAnIPadFollowsTheVCPUCountForEverySet},
		{"the-maximum-is-the-largest-size-that-leaves-the-floor",
			theMaximumIsTheLargestSizeThatLeavesTheFloor},
		{"more-vcpus-never-raise-the-maximum", aMoreVCPUsNeverRaiseTheMaximum},
		{"the-default-on-an-ipad-mini-is-one-and-a-half-gib-at-every-vcpu-count",
			theDefaultOnAnIPadMiniIsOneAndAHalfGiBAtEveryVCPUCount},
		{"the-default-follows-the-limit-in-half-gib-steps",
			theDefaultFollowsTheLimitInHalfGiBSteps},
		{"the-default-is-the-largest-step-that-leaves-the-floor",
			theDefaultIsTheLargestStepThatLeavesTheFloor},
		{"more-vcpus-never-raise-the-default", aMoreVCPUsNeverRaiseTheDefault},
		{"a-guest-above-the-default-is-what-the-applications-warn-about",
			aGuestAboveTheDefaultIsWhatTheApplicationsWarnAbout},
		{"no-limit-offers-the-whole-range", noLimitOffersTheWholeRange},
		{"a-tight-limit-offers-the-smallest-guest",
			aTightLimitOffersTheSmallestGuest},
		{"the-application-limit-is-the-physical-cores-at-most-the-measured-count",
			theApplicationLimitIsThePhysicalCoresAtMostTheMeasuredCount},
	};
	return uml::testing::runNamedTests(tests);
}
