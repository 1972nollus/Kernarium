// SPDX-License-Identifier: GPL-2.0-or-later
#include "shared_programs.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <unordered_map>

namespace uml::cpu::arm64::gadgets {
namespace {

#ifndef UML_GADGET_SHARED_BUDGET_BYTES
#define UML_GADGET_SHARED_BUDGET_BYTES (128UL * 1024UL * 1024UL)
#endif
constexpr std::size_t shared_budget_bytes = UML_GADGET_SHARED_BUDGET_BYTES;
/* Enough shards that the copy under a shard lock is uncontended at the measured
 * translation rate, and not so many that each one's share of the budget is too
 * small to hold a working set. */
constexpr std::size_t shard_count = 64;
constexpr std::size_t shard_budget_bytes = shared_budget_bytes / shard_count;
/* How many entries one reclamation may spare before it must evict. */
constexpr std::size_t max_reprieves = 8;

constexpr std::uint64_t mix_prime = 0x9e3779b97f4a7c15ULL;

[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t value) noexcept
{
	value ^= value >> 33;
	value *= 0xff51afd7ed558ccdULL;
	value ^= value >> 29;
	value *= 0xc4ceb9fe1a85ec53ULL;
	return value ^ (value >> 32);
}

std::atomic<unsigned> live_executors;

} // namespace

SharedKey SharedKey::of(std::uint64_t guest_pc,
			std::span<const std::uint32_t> window,
			std::uint64_t profile) noexcept
{
	return {mix(guest_pc ^ (profile * mix_prime)), guest_pc, profile, window};
}

class SharedProgramStore::Shard final {
public:
	[[nodiscard]] bool adopt(const SharedKey &key,
				 std::pmr::vector<std::uintptr_t> &words,
				 SharedTranslation &translation)
	{
		const std::lock_guard guard{lock_};
		++lookups_;
		const auto found = entries_.find(key.hash);
		if (found == entries_.end() || !found->second.describes(key))
			return false;
		auto &entry = found->second;
		entry.used = true;
		words.assign(entry.words.begin(), entry.words.end());
		translation = entry.translation;
		++adoptions_;
		return true;
	}

	void publish(const SharedKey &key, const BuildResult &result,
		     const LinkRelocations &relocations)
	{
		const auto words = result.program.words();
		const auto retired = result.program.instruction_count();
		/* What the build read: everything it retired, plus the one word
		 * it stopped on without retiring. This is the same rule the
		 * per-executor cache validates a retained program against. */
		const auto consumed = std::min(
			retired + static_cast<std::size_t>(!result.complete),
			key.window.size());
		const std::lock_guard guard{lock_};
		const auto [entry, inserted] = entries_.try_emplace(key.hash);
		if (!inserted) {
			if (entry->second.describes(key)) {
				/* Already here. */
				++rejections_;
				return;
			}
			/* The address holds different code now, or two addresses
			 * collided. Either way the incumbent is the one nobody
			 * is asking for, so the newer translation takes the
			 * place rather than being locked out of it -- a guest
			 * that regenerates code at one address would otherwise
			 * never share any of it again. */
			bytes_ -= entry->second.bytes();
			++replacements_;
		} else {
			order_.push_back(key.hash);
		}
		entry->second.words.assign(words.begin(), words.end());
		/* Canonical means neutral in every cell naming executor-owned
		 * state, so nothing here can be branched through by accident if
		 * a consumer ever failed to resolve one. */
		for (std::size_t index = 0; index < relocations.count; ++index)
			entry->second.words[relocations.entries[index].word] = 0;
		entry->second.consumed.assign(key.window.begin(),
			key.window.begin() + static_cast<std::ptrdiff_t>(consumed));
		entry->second.guest_pc = key.guest_pc;
		entry->second.profile = key.profile;
		entry->second.window_size =
			static_cast<std::uint32_t>(key.window.size());
		entry->second.window_bounded =
			result.complete && retired == key.window.size();
		entry->second.translation = {
			.relocations = relocations,
			.instruction_count = retired,
			.stop_address = result.stop_address,
			.stop_instruction = result.stop_instruction,
			.complete = result.complete,
			.accesses_fp_state = result.program.accesses_fp_state(),
		};
		entry->second.used = true;
		bytes_ += entry->second.bytes();
		++publications_;
		reclaim();
	}

	void accumulate(SharedProgramCounters &counters) const
	{
		const std::lock_guard guard{lock_};
		counters.lookups += lookups_;
		counters.adoptions += adoptions_;
		counters.publications += publications_;
		counters.rejections += rejections_;
		counters.evictions += evictions_;
		counters.replacements += replacements_;
		counters.entries += entries_.size();
		counters.bytes += bytes_;
	}

	void clear()
	{
		const std::lock_guard guard{lock_};
		entries_.clear();
		order_.clear();
		bytes_ = 0;
		lookups_ = adoptions_ = publications_ = rejections_ =
			evictions_ = replacements_ = 0;
	}

private:
	struct Entry final {
		std::vector<std::uintptr_t> words;
		/* Only the words the build read, which is what every decision
		 * in it came from. The cache's own revalidation already treats
		 * these as the program's identity. */
		std::vector<std::uint32_t> consumed;
		std::uint64_t guest_pc{};
		std::uint64_t profile{};
		std::uint32_t window_size{};
		/* The build stopped only because its window ran out, not
		 * because the code told it to. Such a program describes a
		 * shorter block than the same code would produce from a longer
		 * window, so it is offered only to a window of the same
		 * length. */
		bool window_bounded{};
		SharedTranslation translation;
		bool used{};

		[[nodiscard]] bool describes(const SharedKey &key) const noexcept
		{
			if (guest_pc != key.guest_pc || profile != key.profile)
				return false;
			if (window_bounded ? key.window.size() != window_size
					   : key.window.size() < consumed.size())
				return false;
			return consumed.empty() ||
				std::memcmp(consumed.data(), key.window.data(),
					consumed.size() *
						sizeof(std::uint32_t)) == 0;
		}

		[[nodiscard]] std::size_t bytes() const noexcept
		{
			return words.capacity() * sizeof(std::uintptr_t) +
				consumed.capacity() * sizeof(std::uint32_t) +
				sizeof(Entry);
		}
	};

	/* Second-chance FIFO. A translation that has been adopted since it was
	 * last considered is worth another pass; one that has not is what this
	 * store exists to forget, because the guest keeps generating code and
	 * the alternative -- refusing to publish once full -- would freeze the
	 * store around whatever ran first.
	 *
	 * The number of reprieves is bounded per call, and that bound is the
	 * point rather than a detail. A shard holds thousands of entries and the
	 * hit rate is high enough that most of them carry a recent adoption, so
	 * an unbounded sweep would rotate through nearly all of them under the
	 * shard lock before finding a victim -- once per publication, tens of
	 * thousands of times a second, against the lock every adoption needs.
	 * Measured that way the store gave up 90% of translation work and still
	 * lost frame rate. Past the bound this degrades to plain FIFO, which on
	 * a store this oversubscribed is a fair description of what it should do
	 * anyway.
	 */
	void reclaim()
	{
		auto reprieves = max_reprieves;
		while (bytes_ > shard_budget_bytes && !order_.empty()) {
			const auto hash = order_.front();
			order_.pop_front();
			const auto found = entries_.find(hash);
			if (found == entries_.end())
				continue;
			if (found->second.used && reprieves > 0) {
				found->second.used = false;
				order_.push_back(hash);
				--reprieves;
				continue;
			}
			bytes_ -= found->second.bytes();
			entries_.erase(found);
			++evictions_;
		}
	}

	mutable std::mutex lock_;
	std::unordered_map<std::uint64_t, Entry> entries_;
	std::deque<std::uint64_t> order_;
	std::size_t bytes_{};
	std::uint64_t lookups_{}, adoptions_{}, publications_{}, rejections_{},
		evictions_{}, replacements_{};
};

SharedProgramStore::SharedProgramStore()
	: shards_{std::make_unique<Shard[]>(shard_count)}
{
}

SharedProgramStore::~SharedProgramStore() = default;

SharedProgramStore &SharedProgramStore::instance() noexcept
{
	/* Deliberately never destroyed: an executor may outlive static
	 * destruction order on a shutdown path, and an entry is only ever
	 * copied out of here. */
	static auto *store = new SharedProgramStore{};
	return *store;
}

bool SharedProgramStore::adopt(const SharedKey &key,
			       std::pmr::vector<std::uintptr_t> &words,
			       SharedTranslation &translation)
{
	return shards_[key.hash & (shard_count - 1)].adopt(
		key, words, translation);
}

void SharedProgramStore::publish(const SharedKey &key,
				 const BuildResult &result,
				 const LinkRelocations &relocations)
{
	if (relocations.overflowed || result.program.words().empty())
		return;
	/* Refuse a record that does not describe this stream here rather than
	 * leaving consumers to discover it: an entry that cannot be relocated is
	 * one nobody can ever adopt, and it would hold its key against the
	 * translation that can. */
	for (std::size_t index = 0; index < relocations.count; ++index)
		if (relocations.entries[index].word >=
		    result.program.words().size())
			return;
	shards_[key.hash & (shard_count - 1)].publish(
		key, result, relocations);
}

SharedProgramCounters SharedProgramStore::counters() const
{
	SharedProgramCounters counters{};
	for (std::size_t shard = 0; shard < shard_count; ++shard)
		shards_[shard].accumulate(counters);
	return counters;
}

void SharedProgramStore::clear()
{
	for (std::size_t shard = 0; shard < shard_count; ++shard)
		shards_[shard].clear();
}

void SharedProgramStore::executor_created() noexcept
{
	live_executors.fetch_add(1, std::memory_order_relaxed);
}

void SharedProgramStore::executor_destroyed() noexcept
{
	live_executors.fetch_sub(1, std::memory_order_relaxed);
}

bool SharedProgramStore::sharing_worthwhile() noexcept
{
	if constexpr (shared_budget_bytes == 0)
		return false;
	return live_executors.load(std::memory_order_relaxed) >= 2;
}

} // namespace uml::cpu::arm64::gadgets
