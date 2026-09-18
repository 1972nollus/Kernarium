// SPDX-License-Identifier: GPL-2.0-or-later
#include "code_page_registry.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_map>

namespace uml::cpu::x86_64::gadgets {
namespace {

std::mutex table_mutex;
std::unordered_map<std::uint64_t, std::weak_ptr<CodePageRegistry>> &table()
{
	static auto *instance = new std::unordered_map<
		std::uint64_t, std::weak_ptr<CodePageRegistry>>{};
	return *instance;
}

/* A participant leaves the engine at the latest when its run exhausts the
 * instruction window, which is microseconds; the bound only guards against a
 * carrier thread stopped inside the engine by the host. */
constexpr auto quiescence_bound = std::chrono::milliseconds{200};

} // namespace

std::shared_ptr<CodePageRegistry> CodePageRegistry::acquire(
	std::uint64_t generation)
{
	const std::lock_guard lock{table_mutex};
	auto &entries = table();
	auto [entry, inserted] = entries.try_emplace(generation);
	if (!inserted)
		if (auto existing = entry->second.lock())
			return existing;
	auto created = std::make_shared<CodePageRegistry>();
	entry->second = created;
	return created;
}

CodePageRegistry::CodePageRegistry()
	: bits_{std::make_unique<std::atomic<std::uint64_t>[]>(bit_count / 64U)},
	  counts_{std::make_unique<std::uint8_t[]>(bit_count)}
{
	for (std::size_t word = 0; word < bit_count / 64U; ++word)
		bits_[word].store(0, std::memory_order_relaxed);
	std::fill_n(counts_.get(), bit_count, std::uint8_t{0});
}

CodePageRegistry::~CodePageRegistry() = default;

void CodePageRegistry::join(Participant &participant)
{
	const std::lock_guard lock{mutex_};
	if (std::find(participants_.begin(), participants_.end(),
		    &participant) == participants_.end())
		participants_.push_back(&participant);
}

void CodePageRegistry::leave(Participant &participant) noexcept
{
	const std::lock_guard lock{mutex_};
	const auto found = std::find(
		participants_.begin(), participants_.end(), &participant);
	if (found != participants_.end())
		participants_.erase(found);
}

void CodePageRegistry::set_conservative(bool value) noexcept
{
	conservative_.store(value, std::memory_order_relaxed);
	if (!value)
		return;
	const std::lock_guard lock{mutex_};
	for (std::size_t word = 0; word < bit_count / 64U; ++word)
		bits_[word].store(~std::uint64_t{0}, std::memory_order_relaxed);
	changes_.fetch_add(1, std::memory_order_acq_rel);
}

void CodePageRegistry::add(std::uint64_t page, const Participant *self) noexcept
{
	const auto bit = bit_index(page);
	std::unique_lock lock{mutex_};
	auto &count = counts_[bit];
	if (count == 0xff)
		return;
	if (count++ != 0)
		return;
	bits_[bit >> 6U].fetch_or(std::uint64_t{1} << (bit & 63U),
		std::memory_order_release);
	changes_.fetch_add(1, std::memory_order_acq_rel);
	/* Order the count against the retirement below: a miss handler that read
	 * the old count before this fence sees the new count after its own. */
	std::atomic_thread_fence(std::memory_order_seq_cst);
	const auto index = native_tlb_index(page);
	for (auto *participant : participants_) {
		if (participant->write_translations == nullptr)
			continue;
		std::atomic_ref<std::uint64_t>{
			participant->write_translations[index].write_page}
			.store(1, std::memory_order_relaxed);
	}
	/* Participants may be inside the engine with a store already translated
	 * through the entry just retired. Wait for each to leave once so that
	 * store lands before the translation reads the page. */
	lock.unlock();
	wait_for_participants(self);
}

void CodePageRegistry::wait_for_participants(const Participant *self) noexcept
{
	std::vector<std::pair<Participant *, std::uint64_t>> waiting;
	{
		const std::lock_guard lock{mutex_};
		for (auto *participant : participants_) {
			if (participant == self)
				continue;
			const auto epoch = participant->engine_epoch.load(
				std::memory_order_acquire);
			if ((epoch & 1U) != 0)
				waiting.emplace_back(participant, epoch);
		}
	}
	if (waiting.empty())
		return;
	const auto deadline = std::chrono::steady_clock::now() + quiescence_bound;
	for (const auto &[participant, epoch] : waiting) {
		while (participant->engine_epoch.load(std::memory_order_acquire) ==
			epoch) {
			if (std::chrono::steady_clock::now() >= deadline)
				return;
			std::this_thread::yield();
		}
	}
}

void CodePageRegistry::remove(std::uint64_t page) noexcept
{
	const auto bit = bit_index(page);
	const std::lock_guard lock{mutex_};
	auto &count = counts_[bit];
	if (count == 0 || count == 0xff)
		return;
	if (--count != 0 || conservative_.load(std::memory_order_relaxed))
		return;
	bits_[bit >> 6U].fetch_and(~(std::uint64_t{1} << (bit & 63U)),
		std::memory_order_release);
	/* A write translation tagged while the page held code would keep
	 * publishing; retire it so the next store refills it untagged. A
	 * translation retired under a store in flight only publishes once more. */
	const auto index = native_tlb_index(page);
	for (auto *participant : participants_) {
		if (participant->write_translations == nullptr)
			continue;
		std::atomic_ref<std::uint64_t>{
			participant->write_translations[index].write_page}
			.store(1, std::memory_order_relaxed);
	}
}

} // namespace uml::cpu::x86_64::gadgets
