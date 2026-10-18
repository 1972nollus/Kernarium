// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>

namespace uml::memory {

/* A generation's page translations as a radix table of atomic words, for the
 * software-TLB fill of an entered execution context.
 *
 * The fill is the hottest reader of a generation's mappings. Through the
 * mapping map it took the reader lock, a cache line every vCPU writes, and
 * then missed on a hash node of its own for every page, so a 4 KiB guest,
 * which refills about twice as often as a 16 KiB one, spent most of each fill
 * waiting for those two lines. A leaf here holds the host address and the
 * guest's permissions of 64 neighbouring pages, one word each, and is read
 * without a lock.
 *
 * The table mirrors the map, and only an entered context may read it, which
 * is what makes the missing lock sound: everything that removes a page or
 * changes one quiesces execution first, so it can never run beside such a
 * reader, and a mapping added beside one only fills a word nobody can have
 * read as present. A word is absent, never stale. Absence proves nothing,
 * since a node the writer could not allocate stays absent, so a reader that
 * misses asks the map.
 *
 * A guest's mappings come in short runs, so small leaves keep the table well
 * below the map's own size. A leaf whose last page goes is freed at once,
 * which the same quiescence makes safe; the nodes above it stay until
 * clear().
 *
 * Writers hold the generation's mapping lock. clear() frees every node and so
 * also needs execution quiesced.
 */
class FillIndex final {
public:
	static constexpr std::uintptr_t readable = 1;
	static constexpr std::uintptr_t writable = 2;
	static constexpr std::uintptr_t executable = 4;
	static constexpr std::uintptr_t permissions =
		readable | writable | executable;

	FillIndex() noexcept = default;
	FillIndex(const FillIndex &) = delete;
	FillIndex &operator=(const FillIndex &) = delete;
	~FillIndex() { clear(); }

	/* The host address of `page` with its permission bits, or 0. */
	[[nodiscard]] std::uintptr_t find(std::uint64_t page) const noexcept
	{
		const auto number = page >> page_shift;
		if ((number >> page_bits) != 0)
			return 0;
		const auto *upper = roots_[number >> upper_shift].load(
			std::memory_order_acquire);
		if (upper == nullptr)
			return 0;
		const auto *middle = upper->children[index(number, middle_shift)]
			.load(std::memory_order_acquire);
		if (middle == nullptr)
			return 0;
		const auto *lower = middle->children[index(number, lower_shift)]
			.load(std::memory_order_acquire);
		if (lower == nullptr)
			return 0;
		const auto *leaf = lower->children[index(number, leaf_bits)].load(
			std::memory_order_acquire);
		if (leaf == nullptr)
			return 0;
		return leaf->entries[number & leaf_mask].load(
			std::memory_order_acquire);
	}

	/* Records `entry` for `page`; 0 removes it. A node that cannot be
	 * allocated leaves the page absent. */
	void assign(std::uint64_t page, std::uintptr_t entry) noexcept
	{
		const auto number = page >> page_shift;
		if ((number >> page_bits) != 0)
			return;
		const bool create = entry != 0;
		auto *upper = child(roots_[number >> upper_shift], create);
		if (upper == nullptr)
			return;
		auto *middle = child(
			upper->children[index(number, middle_shift)], create);
		if (middle == nullptr)
			return;
		auto *lower = child(
			middle->children[index(number, lower_shift)], create);
		if (lower == nullptr)
			return;
		auto &leaf_slot = lower->children[index(number, leaf_bits)];
		auto *leaf = child(leaf_slot, create);
		if (leaf == nullptr)
			return;
		auto &word = leaf->entries[number & leaf_mask];
		const auto previous = word.load(std::memory_order_relaxed);
		word.store(entry, std::memory_order_release);
		if (previous == 0 && entry != 0) {
			++leaf->used;
		} else if (previous != 0 && entry == 0 && --leaf->used == 0) {
			/* A removal, so no entered context can hold the leaf. */
			leaf_slot.store(nullptr, std::memory_order_relaxed);
			delete leaf;
		}
	}

	void erase(std::uint64_t page) noexcept
	{
		assign(page, 0);
	}

	/* Writers only, with execution quiesced: frees every node. */
	void clear() noexcept
	{
		for (auto &root : roots_) {
			auto *upper = root.load(std::memory_order_relaxed);
			if (upper == nullptr)
				continue;
			for (auto &middle_slot : upper->children) {
				auto *middle = middle_slot.load(std::memory_order_relaxed);
				if (middle == nullptr)
					continue;
				for (auto &lower_slot : middle->children) {
					auto *lower =
						lower_slot.load(std::memory_order_relaxed);
					if (lower == nullptr)
						continue;
					for (auto &leaf : lower->children)
						delete leaf.load(std::memory_order_relaxed);
					delete lower;
				}
				delete middle;
			}
			delete upper;
			root.store(nullptr, std::memory_order_relaxed);
		}
	}

private:
	static constexpr unsigned page_shift = DARWIN_UML_GUEST_PAGE_SHIFT;
	static constexpr unsigned leaf_bits = 6;
	static constexpr unsigned node_bits = 8;
	static constexpr std::size_t leaf_entries = std::size_t{1} << leaf_bits;
	static constexpr std::size_t node_entries = std::size_t{1} << node_bits;
	static constexpr std::uint64_t leaf_mask = leaf_entries - 1U;
	static constexpr std::uint64_t node_mask = node_entries - 1U;
	static constexpr unsigned lower_shift = leaf_bits + node_bits;
	static constexpr unsigned middle_shift = lower_shift + node_bits;
	static constexpr unsigned upper_shift = middle_shift + node_bits;
	/* Every guest address lies below the guest limit. */
	static constexpr unsigned page_bits = static_cast<unsigned>(std::bit_width(
		std::uint64_t{DARWIN_UML_GUEST_ADDRESS_LIMIT} - 1U)) - page_shift;
	static_assert(page_bits > upper_shift && page_bits <= upper_shift + 10U,
		"three nodes and a leaf below a root of at most 1024 entries");
	static constexpr std::size_t root_entries =
		std::size_t{1} << (page_bits - upper_shift);

	[[nodiscard]] static constexpr std::size_t index(
		std::uint64_t number, unsigned shift) noexcept
	{
		return static_cast<std::size_t>((number >> shift) & node_mask);
	}

	struct Leaf final {
		std::array<std::atomic_uintptr_t, leaf_entries> entries{};
		/* Writers only: the entries that are not 0. */
		std::uint32_t used{};
	};
	template <typename Child>
	struct Node final {
		std::array<std::atomic<Child *>, node_entries> children{};
	};
	using Lower = Node<Leaf>;
	using Middle = Node<Lower>;
	using Upper = Node<Middle>;

	template <typename Child>
	[[nodiscard]] static Child *child(std::atomic<Child *> &slot,
		bool create) noexcept
	{
		auto *found = slot.load(std::memory_order_relaxed);
		if (found != nullptr || !create)
			return found;
		/* Published whole: a reader that sees the pointer sees zeroes. */
		found = new (std::nothrow) Child{};
		if (found != nullptr)
			slot.store(found, std::memory_order_release);
		return found;
	}

	std::array<std::atomic<Upper *>, root_entries> roots_{};
};

} // namespace uml::memory
