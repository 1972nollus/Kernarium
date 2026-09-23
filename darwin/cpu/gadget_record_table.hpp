// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace uml::cpu::gadget_cache {

/* Records that translated streams point at, found by a 64-bit key.
 *
 * A link or indirect record is named by address from generated code, so it may
 * never move, and it lives until the address space that owns it is retired.
 * Nothing is ever erased on its own. That makes a node-based hash map the wrong
 * shape twice over: every record costs a node header and a bucket, and a
 * pooled allocator behind it rounds the node up to a power of two and keeps
 * every bucket array a rehash ever outgrew. Measured on a multi-core compile,
 * a 64-byte arm64 link record cost 234 bytes that way.
 *
 * Here records are carved from chunks that are released together, and the
 * index is one open-addressed array of pointers that is replaced whole when it
 * grows. The key is read back out of the record (`KeyOf`), so an index entry
 * is a single word: the pointer, with the top bits of the key's hash beside it
 * so that a probe only dereferences a record whose tag already matches.
 */
template <typename Record, typename KeyOf>
class StableRecordTable final {
	static_assert(std::is_trivially_destructible_v<Record>,
		"records are released with their chunk, without a destructor");
	static_assert(alignof(Record) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
		"chunks come from the ordinary array allocation");

public:
	StableRecordTable() = default;
	StableRecordTable(const StableRecordTable &) = delete;
	StableRecordTable &operator=(const StableRecordTable &) = delete;
	StableRecordTable(StableRecordTable &&) = delete;
	StableRecordTable &operator=(StableRecordTable &&) = delete;
	~StableRecordTable() = default;

	[[nodiscard]] Record *find(std::uint64_t key) const noexcept
	{
		if (index_ == nullptr)
			return nullptr;
		const auto hash = mix(key);
		const auto tag = tag_of(hash);
		for (auto position = hash & mask_;; position = (position + 1) & mask_) {
			const auto entry = index_[position];
			if (entry == 0)
				return nullptr;
			if ((entry & tag_mask) == tag) {
				auto *record = pointer_of(entry);
				if (KeyOf{}(*record) == key)
					return record;
			}
		}
	}

	/* The record for `key`, constructed from `arguments` when absent. */
	template <typename... Arguments>
	[[nodiscard]] std::pair<Record *, bool> try_emplace(
		std::uint64_t key, Arguments &&...arguments)
	{
		if (auto *found = find(key); found != nullptr)
			return {found, false};
		if ((size_ + 1) * 4 > capacity() * 3)
			grow();
		auto *record = new (carve()) Record(
			std::forward<Arguments>(arguments)...);
		assert(KeyOf{}(*record) == key);
		insert(record, mix(key));
		++size_;
		return {record, true};
	}

	[[nodiscard]] std::size_t size() const noexcept { return size_; }

	/* What the table holds: its chunks and its index. */
	[[nodiscard]] std::size_t bytes() const noexcept
	{
		std::size_t total = capacity() * sizeof(std::uint64_t) +
			chunks_.capacity() * sizeof(Chunk);
		for (const auto &chunk : chunks_)
			total += chunk.count * sizeof(Record);
		return total;
	}

	template <typename Visitor>
	void for_each(Visitor &&visit) const
	{
		for (const auto &chunk : chunks_) {
			const auto used = &chunk == &chunks_.back() ?
				used_ : chunk.count;
			for (std::size_t index = 0; index < used; ++index)
				visit(*std::launder(reinterpret_cast<const Record *>(
					chunk.storage.get() + index * sizeof(Record))));
		}
	}

private:
	/* Records per chunk: small for the many short-lived address spaces a
	 * desktop guest creates, doubling to a bound so a long-lived one wastes at
	 * most one partly used chunk of that size. */
	static constexpr std::size_t first_chunk = 32;
	static constexpr std::size_t largest_chunk =
		std::max<std::size_t>(first_chunk, (std::size_t{128} << 10) / sizeof(Record));
	static constexpr std::size_t first_capacity = 64;
	/* Host user addresses fit in 48 bits on every supported product, which
	 * leaves the top sixteen for a tag. */
	static constexpr unsigned pointer_bits = 48;
	static constexpr std::uint64_t tag_mask =
		~((std::uint64_t{1} << pointer_bits) - 1U);

	struct Chunk final {
		std::unique_ptr<std::byte[]> storage;
		std::size_t count;
	};

	[[nodiscard]] static std::uint64_t mix(std::uint64_t key) noexcept
	{
		key ^= key >> 31;
		key *= 0x9e3779b97f4a7c15ULL;
		return key ^ (key >> 29);
	}

	[[nodiscard]] static std::uint64_t tag_of(std::uint64_t hash) noexcept
	{
		/* Never zero, so no entry is ever the empty word. */
		return (hash | (std::uint64_t{1} << 63)) & tag_mask;
	}

	[[nodiscard]] static Record *pointer_of(std::uint64_t entry) noexcept
	{
		return reinterpret_cast<Record *>(
			static_cast<std::uintptr_t>(entry & ~tag_mask));
	}

	[[nodiscard]] std::size_t capacity() const noexcept
	{
		return index_ == nullptr ? 0 : mask_ + 1;
	}

	void insert(Record *record, std::uint64_t hash) noexcept
	{
		const auto address = reinterpret_cast<std::uintptr_t>(record);
		assert((address & tag_mask) == 0);
		auto position = hash & mask_;
		while (index_[position] != 0)
			position = (position + 1) & mask_;
		index_[position] = tag_of(hash) | address;
	}

	void grow()
	{
		const auto next = std::max<std::size_t>(first_capacity, capacity() * 2);
		auto replacement = std::make_unique<std::uint64_t[]>(next);
		auto previous = std::move(index_);
		const auto previous_capacity = previous == nullptr ? 0 : mask_ + 1;
		index_ = std::move(replacement);
		mask_ = next - 1;
		for (std::size_t position = 0; position < previous_capacity; ++position)
			if (const auto entry = previous[position]; entry != 0) {
				auto *record = pointer_of(entry);
				insert(record, mix(KeyOf{}(*record)));
			}
	}

	[[nodiscard]] void *carve()
	{
		if (chunks_.empty() || used_ == chunks_.back().count) {
			const auto count = chunks_.empty() ? first_chunk :
				std::min(largest_chunk, chunks_.back().count * 2);
			/* Reserve before allocating, so a failed push cannot leak
			 * the chunk. */
			if (chunks_.size() == chunks_.capacity())
				chunks_.reserve(std::max<std::size_t>(
					8, chunks_.size() * 2));
			chunks_.push_back({std::make_unique_for_overwrite<std::byte[]>(
				count * sizeof(Record)), count});
			used_ = 0;
		}
		return chunks_.back().storage.get() + used_++ * sizeof(Record);
	}

	std::vector<Chunk> chunks_;
	std::size_t used_{};
	std::unique_ptr<std::uint64_t[]> index_;
	std::size_t mask_{};
	std::size_t size_{};
};

} // namespace uml::cpu::gadget_cache
