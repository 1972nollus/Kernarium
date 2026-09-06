// SPDX-License-Identifier: GPL-2.0-or-later
#include "execution_histogram.hpp"

#if UML_GADGET_EXECUTION_HISTOGRAM
#include "executor.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uml::cpu::arm64::gadgets::diagnostics {
namespace {

using Counters = std::array<std::atomic_uint64_t, execution_histogram_capacity>;
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
static_assert(sizeof(std::atomic_uint64_t) == sizeof(std::uint64_t));
static_assert(std::atomic_uint64_t::is_always_lock_free);

[[nodiscard]] std::string resolve_symbol_name(const void *address)
{
	Dl_info info{};
	if (address != nullptr && dladdr(address, &info) != 0 &&
	    info.dli_sname != nullptr) {
		std::string name = info.dli_sname;
		if (!name.empty() && name.front() == '_')
			name.erase(name.begin());
		if (info.dli_saddr != address) {
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "+%#llx",
				static_cast<unsigned long long>(
					reinterpret_cast<std::uintptr_t>(address) -
					reinterpret_cast<std::uintptr_t>(info.dli_saddr)));
			name += buffer;
		}
		return name;
	}
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%p", address);
	return buffer;
}

/* Each carrier has exactly one writer. Aligned A64 LDR/STR implements the
 * relaxed atomic operations used by the threaded counter gadget; the dumper
 * reads with relaxed loads, so a snapshot is not one atomic instant. Tables
 * outlive carriers and remain valid at exit. */
class Histogram final {
public:
	Histogram()
	{
		const auto *path = std::getenv("DARWIN_UML_GADGET_EXECUTION_HISTOGRAM");
		if (path == nullptr || *path == '\0')
			return;
		path_ = path;
		worker_ = std::thread{[this] {
			while (!stop_.load(std::memory_order_relaxed)) {
				dump();
				std::this_thread::sleep_for(std::chrono::seconds{2});
			}
		}};
	}
	~Histogram()
	{
		stop_.store(true, std::memory_order_relaxed);
		if (worker_.joinable())
			worker_.join();
		if (!path_.empty())
			dump();
	}
	void note_executor(const GadgetCacheDiagnostics *diagnostics)
	{
		const std::scoped_lock lock{mutex_};
		if (std::find(executors_.begin(), executors_.end(), diagnostics) ==
		    executors_.end())
			executors_.push_back(diagnostics);
	}
	Counters *register_thread()
	{
		auto *result = new Counters{};
		const std::scoped_lock lock{mutex_};
		tables_.push_back(result);
		return result;
	}
	std::size_t slot(const void *previous, const void *gadget)
	{
		const Pair pair{previous, gadget};
		const std::scoped_lock lock{mutex_};
		const auto found = index_.find(pair);
		if (found != index_.end())
			return (2 + found->second) * sizeof(std::uint64_t);
		if (2 + entries_.size() >= execution_histogram_capacity)
			return sizeof(std::uint64_t);
		const auto index = static_cast<std::uint32_t>(entries_.size());
		entries_.push_back(pair);
		index_.emplace(pair, index);
		return (2 + index) * sizeof(std::uint64_t);
	}

private:
	void dump()
	{
		std::vector<std::uint64_t> merged(execution_histogram_capacity);
		std::vector<Pair> entries;
		GadgetCacheDiagnostics executors{};
		{
			const std::scoped_lock lock{mutex_};
			for (const auto *table : tables_)
				for (std::size_t i = 0; i < merged.size(); ++i)
					merged[i] += (*table)[i].load(std::memory_order_relaxed);
			entries = entries_;
			for (const auto *d : executors_) {
				executors.translations += d->translations;
				executors.block_entries += d->block_entries;
				executors.cache_misses += d->cache_misses;
				executors.exits_chain_limit += d->exits_chain_limit;
				executors.exits_indirect_miss += d->exits_indirect_miss;
				executors.exits_indirect_miss_return +=
					d->exits_indirect_miss_return;
				for (std::size_t i = 0; i < executors.miss_by_site_targets.size(); ++i)
					executors.miss_by_site_targets[i] += d->miss_by_site_targets[i];
				executors.exits_unlinked += d->exits_unlinked;
				executors.exits_syscall += d->exits_syscall;
				executors.exits_fault += d->exits_fault;
				executors.exits_interrupt += d->exits_interrupt;
				executors.exits_other += d->exits_other;
			}
		}
		std::vector<std::pair<std::uint64_t, Pair>> ordered;
		ordered.reserve(entries.size());
		for (std::size_t i = 0; i < entries.size(); ++i)
			ordered.emplace_back(merged[2 + i], entries[i]);
		std::ranges::sort(ordered, [](const auto &left, const auto &right) {
			return left.first > right.first;
		});
		std::FILE *out = std::fopen(path_.c_str(), "w");
		if (out == nullptr)
			return;
		std::fprintf(out, "count\tsymbol\tpredecessor\n");
		std::fprintf(out, "%llu\t@entered_guest_words\t\n",
			static_cast<unsigned long long>(merged[0]));
		std::fprintf(out, "%llu\t@unindexed_groups\t\n",
			static_cast<unsigned long long>(merged[1]));
		const std::pair<const char *, std::uint64_t> executor_rows[] = {
			{"@executor_block_entries", executors.block_entries},
			{"@executor_cache_misses", executors.cache_misses},
			{"@executor_translations", executors.translations},
			{"@exit_chain_limit", executors.exits_chain_limit},
			{"@exit_indirect_miss", executors.exits_indirect_miss},
			{"@exit_indirect_miss_return",
				executors.exits_indirect_miss_return},
			{"@exit_unlinked", executors.exits_unlinked},
			{"@exit_syscall", executors.exits_syscall},
			{"@exit_fault", executors.exits_fault},
			{"@exit_interrupt", executors.exits_interrupt},
			{"@exit_other", executors.exits_other},
		};
		for (const auto &[label, value] : executor_rows)
			std::fprintf(out, "%llu\t%s\t\n",
				static_cast<unsigned long long>(value), label);
		static const char *const buckets[] = {"1", "2", "3", "4", "5-8", "9-16", "17-64", "65+"};
		for (std::size_t i = 0; i < 8; ++i)
			std::fprintf(out, "%llu\t@miss_at_sites_with_%s_targets\t\n",
				static_cast<unsigned long long>(executors.miss_by_site_targets[i]), buckets[i]);
		for (const auto &[count, pair] : ordered) {
			if (count == 0)
				break;
			std::fprintf(out, "%llu\t%s\t%s\n",
				static_cast<unsigned long long>(count),
				symbol_name(pair.second).c_str(),
				pair.first == nullptr ? "@block_entry" :
					symbol_name(pair.first).c_str());
		}
		std::fclose(out);
	}
	[[nodiscard]] const std::string &symbol_name(const void *address)
	{
		if (const auto found = names_.find(address); found != names_.end())
			return found->second;
		return names_.emplace(address, resolve_symbol_name(address)).first->second;
	}

	std::mutex mutex_;
	std::vector<Counters *> tables_;
	std::unordered_map<Pair, std::uint32_t, PairHash> index_;
	std::vector<Pair> entries_;
	std::vector<const GadgetCacheDiagnostics *> executors_;
	std::unordered_map<const void *, std::string> names_;
	std::atomic_bool stop_{};
	std::thread worker_;
	std::string path_;
};

Histogram &histogram()
{
	static Histogram instance;
	return instance;
}

} // namespace

void *execution_counters()
{
	static thread_local auto *counters = histogram().register_thread();
	return counters->data();
}

std::size_t execution_histogram_slot(const void *previous, const void *gadget)
{
	return histogram().slot(previous, gadget);
}

void execution_histogram_note_executor(const void *diagnostics)
{
	static thread_local const void *noted = nullptr;
	if (noted == diagnostics)
		return;
	noted = diagnostics;
	histogram().note_executor(
		static_cast<const GadgetCacheDiagnostics *>(diagnostics));
}

} // namespace uml::cpu::arm64::gadgets::diagnostics
#endif
