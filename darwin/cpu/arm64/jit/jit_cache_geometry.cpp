// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_cache_geometry.hpp"

#include "jit_cache.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>

namespace uml::cpu::arm64 {

namespace {

/* Used when nothing published a budget: unit tests, embedders that never call
 * the setter, and any path that creates an executor before startup has parsed
 * the guest's memory size. Generous enough that a single executor still holds
 * a real working set, small enough that a forgotten setter cannot reach the
 * multi-gigabyte totals this type exists to prevent.
 */
constexpr std::size_t default_total_bytes = 512U * 1024U * 1024U;
constexpr unsigned default_executors = 1;

/* Shares of the per-executor budget. The block cache and its saved guest words
 * decide how much of the working set stays resident, the arena decides how
 * much translated code survives before a reset discards the cache, and the
 * link tables only have to cover the blocks the cache can hold.
 */
constexpr double lookup_share = 0.30;
constexpr double arena_share = 0.45;
constexpr double tracked_store_share = 0.08;

constexpr std::size_t minimum_cache_sets = 1024;
constexpr std::size_t maximum_cache_sets = 131072;
constexpr std::size_t minimum_arena_bytes = 16U * 1024U * 1024U;
constexpr std::size_t maximum_arena_bytes = 512U * 1024U * 1024U;
constexpr std::size_t minimum_tracked_store_bytes = 4U * 1024U * 1024U;
constexpr std::size_t maximum_tracked_store_bytes = 32U * 1024U * 1024U;

std::atomic<std::size_t> published_total_bytes{};
std::atomic<unsigned> published_executors{};

[[nodiscard]] std::size_t round_down_to_power_of_two(
	std::size_t value) noexcept
{
	if (value == 0)
		return 0;
	std::size_t result = 1;
	while (result <= value / 2U)
		result *= 2U;
	return result;
}

} // namespace

std::size_t JitCacheGeometry::total_bytes() const noexcept
{
	return cache_sets * cache_ways *
		       (sizeof(CompiledBlock) + sizeof(SavedGuestWords)) +
	       block_links * sizeof(JitBlockLink) +
	       indirect_links * sizeof(JitIndirectLink) + arena_bytes +
	       tracked_store_arena_bytes;
}

void set_jit_cache_budget(std::size_t total_bytes,
	unsigned executors) noexcept
{
	if (total_bytes == 0 || executors == 0)
		return;
	std::size_t unset = 0;
	if (published_total_bytes.compare_exchange_strong(unset, total_bytes,
		    std::memory_order_relaxed))
		published_executors.store(executors, std::memory_order_relaxed);
}

JitCacheGeometry jit_cache_geometry() noexcept
{
	const auto total =
		published_total_bytes.load(std::memory_order_relaxed);
	const auto executors =
		published_executors.load(std::memory_order_relaxed);
	const auto budget =
		(total == 0 ? default_total_bytes : total) /
		std::max(executors == 0 ? default_executors : executors, 1U);

	const auto per_set =
		cache_ways * (sizeof(CompiledBlock) + sizeof(SavedGuestWords));
	const auto lookup_bytes =
		static_cast<std::size_t>(static_cast<double>(budget) *
			lookup_share);
	const auto sets = std::clamp(
		round_down_to_power_of_two(lookup_bytes / per_set),
		minimum_cache_sets, maximum_cache_sets);

	/* One link record per cached block, rather than the two per block the
	 * former constant reserved. A block the cache cannot hold cannot be a
	 * link target either, so the extra reservation was never reachable.
	 */
	const auto links = sets * cache_ways;

	const auto arena = std::clamp(
		static_cast<std::size_t>(static_cast<double>(budget) *
			arena_share),
		minimum_arena_bytes, maximum_arena_bytes);
	const auto tracked_store = std::clamp(
		static_cast<std::size_t>(static_cast<double>(budget) *
			tracked_store_share),
		minimum_tracked_store_bytes, maximum_tracked_store_bytes);

	return {.cache_sets = sets,
		.block_links = links,
		.indirect_links = links,
		.arena_bytes = arena,
		.tracked_store_arena_bytes = tracked_store};
}

std::size_t parse_byte_quantity(const char *text) noexcept
{
	if (text == nullptr || *text == '\0')
		return 0;
	std::uint64_t value = 0;
	const char *cursor = text;
	for (; *cursor >= '0' && *cursor <= '9'; ++cursor) {
		const auto digit =
			static_cast<std::uint64_t>(*cursor - '0');
		if (value > (~std::uint64_t{} - digit) / 10U)
			return 0;
		value = value * 10U + digit;
	}
	if (cursor == text)
		return 0;
	std::uint64_t multiplier = 1;
	switch (std::tolower(static_cast<unsigned char>(*cursor))) {
	case 'k':
		multiplier = 1024U;
		++cursor;
		break;
	case 'm':
		multiplier = 1024U * 1024U;
		++cursor;
		break;
	case 'g':
		multiplier = 1024U * 1024U * 1024U;
		++cursor;
		break;
	default:
		break;
	}
	if (*cursor != '\0')
		return 0;
	if (value > ~std::uint64_t{} / multiplier)
		return 0;
	return static_cast<std::size_t>(value * multiplier);
}

} // namespace uml::cpu::arm64
