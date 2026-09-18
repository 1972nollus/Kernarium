// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "native_entry.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace uml::cpu::x86_64::gadgets {

/* Which guest pages hold a cached translation in any executor of one address
 * space.
 *
 * Linux marks every data page of a READ_IMPLIES_EXEC process executable, so
 * "this page may hold code" is true for the stack and the heap, and a store
 * that publishes on that predicate publishes on almost every store. What a
 * publication protects is a cached translation, and only the executors know
 * where those are. Every executor running the address space reports the pages
 * its cache covers here; the miss handler tags a write translation for
 * publication only when the page is on the list.
 *
 * The list is shared between vCPU threads, so two things keep it exact.
 * Adding a page retires every participant's write translation for it and
 * bumps a change count that the miss handler re-reads after filling, so no
 * untagged translation can outlive the moment the page gained code. And a
 * participant that is inside the engine may already have translated a store
 * it has not performed, so the adder waits until every such participant has
 * left the engine once -- bounded by the run's instruction window -- before
 * the translation reads the page's bytes.
 *
 * Membership is a count per hashed bit, which makes the query one load and
 * lets a page shared by several executors, or two pages sharing a bit, be
 * counted rather than tracked. A collision only makes a store publish.
 */
class CodePageRegistry final {
public:
	struct Participant final {
		NativeTlbEntry *write_translations{};
		/* Odd while the participant is inside the engine. */
		std::atomic<std::uint64_t> engine_epoch{0};
	};

	/* One registry per guest address-space generation, shared for as long as
	 * any executor holds it. */
	[[nodiscard]] static std::shared_ptr<CodePageRegistry> acquire(
		std::uint64_t generation);

	CodePageRegistry();
	~CodePageRegistry();
	CodePageRegistry(const CodePageRegistry &) = delete;
	CodePageRegistry &operator=(const CodePageRegistry &) = delete;

	void join(Participant &participant);
	void leave(Participant &participant) noexcept;

	[[nodiscard]] bool holds(std::uint64_t address) const noexcept
	{
		const auto bit = bit_index(address);
		return (bits_[bit >> 6U].load(std::memory_order_relaxed) &
			(std::uint64_t{1} << (bit & 63U))) != 0;
	}
	[[nodiscard]] std::uint64_t changes() const noexcept
	{
		return changes_.load(std::memory_order_acquire);
	}

	/* Called by the executor that is about to translate `page`, before it
	 * reads the page's bytes. */
	void add(std::uint64_t page, const Participant *self) noexcept;
	void remove(std::uint64_t page) noexcept;

	/* Every executable page counts as code. The Wine CPU service runs x86
	 * executors beside arm64 ones whose translations this registry does not
	 * see, so it keeps the unconditional publication. */
	void set_conservative(bool value) noexcept;

private:
	static constexpr unsigned bit_count_log2 = 18;
	static constexpr std::size_t bit_count = std::size_t{1} << bit_count_log2;

	[[nodiscard]] static std::size_t bit_index(std::uint64_t address) noexcept
	{
		const auto page = address >> 12U;
		return static_cast<std::size_t>(
			(page ^ (page >> bit_count_log2)) & (bit_count - 1U));
	}
	void wait_for_participants(const Participant *self) noexcept;

	std::mutex mutex_;
	std::vector<Participant *> participants_;
	std::unique_ptr<std::atomic<std::uint64_t>[]> bits_;
	std::unique_ptr<std::uint8_t[]> counts_;
	std::atomic<std::uint64_t> changes_{0};
	std::atomic<bool> conservative_{false};
};

} // namespace uml::cpu::x86_64::gadgets
