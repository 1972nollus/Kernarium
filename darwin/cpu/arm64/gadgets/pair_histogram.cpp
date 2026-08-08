// SPDX-License-Identifier: GPL-2.0-or-later
#include "pair_histogram.hpp"

#if UML_GADGET_PAIR_HISTOGRAM

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uml::cpu::arm64::gadgets::diagnostics {
namespace {

using Pair = std::pair<const void *, const void *>;

struct PairHash final {
	[[nodiscard]] std::size_t operator()(const Pair &pair) const noexcept
	{
		const auto left = reinterpret_cast<std::uintptr_t>(pair.first);
		const auto right = reinterpret_cast<std::uintptr_t>(pair.second);
		auto value = left * 0x9e37'79b9'7f4a'7c15ULL ^ (right + 0x1234'5678ULL);
		value ^= value >> 29;
		return static_cast<std::size_t>(value);
	}
};

using Counts = std::unordered_map<Pair, std::uint64_t, PairHash>;

std::mutex &registry_mutex()
{
	static std::mutex mutex;
	return mutex;
}

std::vector<Counts *> &registry()
{
	static std::vector<Counts *> tables;
	return tables;
}

/* Per-thread counting keeps translation lock-free; the tables are registered so
 * the dump can merge them, and deliberately leaked so a thread exiting during
 * teardown cannot race the dump. */
Counts &thread_counts()
{
	static thread_local Counts *counts = [] {
		auto *table = new Counts;
		const std::scoped_lock lock{registry_mutex()};
		registry().push_back(table);
		return table;
	}();
	return *counts;
}

/* dladdr is far too slow to call per entry per dump once the table reaches
 * twenty thousand pairs, and the dump repeats over the whole table each time.
 * Addresses repeat heavily across pairs, so one lookup each is enough. */
std::unordered_map<const void *, std::string> &symbol_cache()
{
	static std::unordered_map<const void *, std::string> cache;
	return cache;
}

[[nodiscard]] std::string resolve_symbol_name(const void *address)
{
	Dl_info info{};
	if (address != nullptr && dladdr(address, &info) != 0 &&
	    info.dli_sname != nullptr) {
		std::string name = info.dli_sname;
		/* Strip the Mach-O leading underscore so the names match the ones
		 * written in the .inc sources. */
		if (!name.empty() && name.front() == '_')
			name.erase(name.begin());
		return name;
	}
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%p", address);
	return buffer;
}

[[nodiscard]] const std::string &symbol_name(const void *address)
{
	auto &cache = symbol_cache();
	if (const auto found = cache.find(address); found != cache.end())
		return found->second;
	return cache.emplace(address, resolve_symbol_name(address)).first->second;
}

void dump()
{
	const char *path = std::getenv("DARWIN_UML_GADGET_PAIR_HISTOGRAM");
	if (path == nullptr)
		return;
	Counts merged;
	{
		const std::scoped_lock lock{registry_mutex()};
		for (const auto *table : registry())
			for (const auto &[pair, count] : *table)
				merged[pair] += count;
	}
	std::vector<std::pair<Pair, std::uint64_t>> ordered{
		merged.begin(), merged.end()};
	std::ranges::sort(ordered, [](const auto &left, const auto &right) {
		return left.second > right.second;
	});
	std::FILE *out = std::fopen(path, "w");
	if (out == nullptr)
		return;
	std::fprintf(out, "count\tfirst\tsecond\n");
	for (const auto &[pair, count] : ordered)
		std::fprintf(out, "%llu\t%s\t%s\n",
			static_cast<unsigned long long>(count),
			symbol_name(pair.first).c_str(),
			symbol_name(pair.second).c_str());
	std::fclose(out);
}

const bool dump_registered = [] {
	std::atexit(dump);
	return true;
}();

/* atexit alone is not enough: the harness shuts the emulator down with
 * `poweroff -f` and then SIGTERM, and a run that ends on a signal never runs
 * exit handlers - which is exactly how the first capture attempt produced an
 * empty file. Rewrite the file periodically so the result survives any exit
 * path. Cost is irrelevant here because this code only exists in the
 * diagnostic build. */
constexpr std::uint64_t dump_interval = 1U << 16;
std::atomic_uint64_t recorded_since_dump{};

} // namespace

void record_gadget_pair(const void *previous, const void *current) noexcept
{
	if (previous == nullptr || current == nullptr)
		return;
	try {
		++thread_counts()[Pair{previous, current}];
		if (recorded_since_dump.fetch_add(1, std::memory_order_relaxed) %
			    dump_interval == dump_interval - 1)
			dump();
	} catch (...) {
		/* A diagnostic must never change what the translator does. */
	}
}

} // namespace uml::cpu::arm64::gadgets::diagnostics

#endif
