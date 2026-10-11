// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace uml::cpu::gadget_cache {

/* Records that translated streams point at, found by a 64-bit key.
 *
 * A link or indirect record is named by address from generated code, so it may
 * never move. That makes a node-based hash map the wrong shape twice over:
 * every record costs a node header and a bucket, and a pooled allocator behind
 * it rounds the node up to a power of two and keeps every bucket array a
 * rehash ever outgrew. Measured on a multi-core compile, a 64-byte arm64 link
 * record cost 234 bytes that way.
 *
 * Here records are carved from chunks, and the index is one open-addressed
 * array that is replaced whole when it grows or is swept. The key is read back
 * out of the record (`KeyOf`), so an index entry is 32 bits: the record's
 * number -- its place in the order the chunks carve records -- with the top
 * bits of the key's hash beside it so that a probe only resolves a record
 * whose tag already matches. Chunk sizes follow a fixed sequence, so a number
 * names its chunk and slot arithmetically.
 *
 * Nothing is erased one record at a time, because only the owner knows what
 * still names a record. It sweeps instead: it marks every record anything it
 * holds may name (`Marks`), and `sweep` releases the rest. A released place is
 * reused, lowest number first, by the next record made, and chunks at the end
 * that hold no record any more are returned, so the table follows what is
 * named rather than everything that ever was.
 */
template <typename Record, typename KeyOf>
class StableRecordTable final {
	static_assert(std::is_trivially_destructible_v<Record>,
		"records are released with their chunk, without a destructor");
	static_assert(alignof(Record) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
		"chunks come from the ordinary array allocation");

public:
	/* The records a sweep keeps, found from their addresses. */
	class Marks final {
	public:
		/* Keep the record that begins at `address`. Any other value --
		 * one inside a record, past the records made, or outside the
		 * table -- is ignored, so a caller may pass every value that
		 * might name a record, at the cost of keeping a few that
		 * nothing names. */
		void mark(std::uintptr_t address) noexcept
		{
			if (const auto ordinal = ordinal_of(address);
			    ordinal != absent)
				bits_[ordinal / 64] |= std::uint64_t{1} << (ordinal % 64);
		}

		void mark(const Record *record) noexcept
		{
			mark(reinterpret_cast<std::uintptr_t>(record));
		}

		[[nodiscard]] bool marked(const Record *record) const noexcept
		{
			const auto ordinal = ordinal_of(
				reinterpret_cast<std::uintptr_t>(record));
			return ordinal != absent && test(ordinal);
		}

	private:
		friend class StableRecordTable;

		struct Range final {
			std::uintptr_t begin;
			std::uintptr_t end;
			std::size_t first;
		};

		static constexpr std::size_t absent = ~std::size_t{};

		[[nodiscard]] std::size_t ordinal_of(
			std::uintptr_t address) const noexcept
		{
			if (address < lowest_ || address >= highest_)
				return absent;
			auto found = std::upper_bound(ranges_.begin(), ranges_.end(),
				address, [](std::uintptr_t value, const Range &range) {
					return value < range.begin;
				});
			if (found == ranges_.begin())
				return absent;
			--found;
			if (address >= found->end)
				return absent;
			const auto offset = address - found->begin;
			if (offset % sizeof(Record) != 0)
				return absent;
			return found->first + offset / sizeof(Record);
		}

		[[nodiscard]] bool test(std::size_t ordinal) const noexcept
		{
			return (bits_[ordinal / 64] >> (ordinal % 64) & 1U) != 0;
		}

		/* The chunks by address, each over the records it has carved. */
		std::vector<Range> ranges_;
		std::uintptr_t lowest_{};
		std::uintptr_t highest_{};
		std::vector<std::uint64_t> bits_;
	};

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
				auto *record = record_at(entry & ~tag_mask);
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
		/* A place is taken before its record is constructed, which a
		 * constructor that threw would leave taken and unindexed. */
		static_assert(std::is_nothrow_constructible_v<Record, Arguments...>);
		if (auto *found = find(key); found != nullptr)
			return {found, false};
		if ((size_ + 1) * 4 > capacity() * 3)
			grow();
		const auto ordinal = take();
		auto *record = new (place(ordinal)) Record(
			std::forward<Arguments>(arguments)...);
		assert(KeyOf{}(*record) == key);
		insert(static_cast<std::uint32_t>(ordinal + 1U), mix(key));
		++size_;
		return {record, true};
	}

	[[nodiscard]] std::size_t size() const noexcept { return size_; }

	/* What the table holds: its chunks, its index and its map of the
	 * places a sweep left free. */
	[[nodiscard]] std::size_t bytes() const noexcept
	{
		std::size_t total = capacity() * sizeof(std::uint32_t) +
			chunks_.capacity() * sizeof(Chunk) +
			vacant_.capacity() * sizeof(std::uint64_t);
		for (const auto &chunk : chunks_)
			total += chunk.count * sizeof(Record);
		return total;
	}

	/* Every record, in the order of their numbers. */
	template <typename Visitor>
	void for_each(Visitor &&visit) const
	{
		for (std::size_t ordinal = 0; ordinal < carved_; ++ordinal)
			if (!vacant(ordinal))
				visit(*std::launder(reinterpret_cast<const Record *>(
					place(ordinal))));
	}

	template <typename Visitor>
	void for_each(Visitor &&visit)
	{
		for (std::size_t ordinal = 0; ordinal < carved_; ++ordinal)
			if (!vacant(ordinal))
				visit(*std::launder(reinterpret_cast<Record *>(
					place(ordinal))));
	}

	/* Marks over the records made so far, none of them kept yet. They do
	 * not cover a record made after them, so no record may be made
	 * between taking them and sweeping with them. */
	[[nodiscard]] Marks marks() const
	{
		Marks marks;
		marks.bits_.assign((carved_ + 63) / 64, 0);
		marks.ranges_.reserve(chunks_.size());
		std::size_t first = 0;
		for (const auto &chunk : chunks_) {
			const auto begin = reinterpret_cast<std::uintptr_t>(
				chunk.storage.get());
			const auto used = &chunk == &chunks_.back() ?
				used_ : chunk.count;
			marks.ranges_.push_back({begin, begin + used * sizeof(Record),
				first});
			first += chunk.count;
		}
		std::sort(marks.ranges_.begin(), marks.ranges_.end(),
			[](const auto &left, const auto &right) {
				return left.begin < right.begin;
			});
		if (!marks.ranges_.empty()) {
			marks.lowest_ = marks.ranges_.front().begin;
			for (const auto &range : marks.ranges_)
				marks.highest_ = std::max(marks.highest_, range.end);
		}
		return marks;
	}

	/* Release every record `marks` does not keep, and return how many went.
	 * A released record is zeroed, so a reference its owner failed to mark
	 * faults rather than reaching whatever is made there next. The sweep
	 * happens whole or, when its new index cannot be allocated, not at all.
	 */
	std::size_t sweep(const Marks &marks)
	{
		assert(marks.bits_.size() == (carved_ + 63) / 64);
		std::vector<std::uint32_t> kept;
		kept.reserve(size_);
		for (std::size_t position = 0; position < capacity(); ++position)
			if (const auto entry = index_[position]; entry != 0 &&
			    marks.test((entry & ~tag_mask) - 1U))
				kept.push_back(entry & ~tag_mask);
		const auto released = size_ - kept.size();
		if (released == 0)
			return 0;
		std::size_t next = first_capacity;
		while (kept.size() * 4 > next * 3)
			next *= 2;
		auto replacement = kept.empty() ? nullptr :
			std::make_unique<std::uint32_t[]>(next);

		for (std::size_t position = 0; position < capacity(); ++position)
			if (const auto entry = index_[position]; entry != 0 &&
			    !marks.test((entry & ~tag_mask) - 1U))
				release((entry & ~tag_mask) - 1U);
		index_ = std::move(replacement);
		mask_ = index_ == nullptr ? 0 : next - 1;
		size_ = kept.size();
		for (const auto number : kept)
			insert(number, mix(KeyOf{}(*record_at(number))));
		lowest_vacant_ = 0;
		trim();
		return released;
	}

private:
	/* Records per chunk: small for the many short-lived address spaces a
	 * desktop guest creates, doubling to a bound so a long-lived one wastes at
	 * most one partly used chunk of that size. The chunks that double before
	 * reaching the bound hold `doubling_records` between them; every chunk
	 * after them holds `largest_chunk`. */
	static constexpr std::size_t first_chunk = 32;
	static constexpr std::size_t largest_chunk =
		std::max<std::size_t>(first_chunk, (std::size_t{128} << 10) / sizeof(Record));
	static constexpr unsigned doubling_chunks = static_cast<unsigned>(
		std::bit_width((largest_chunk - 1U) / first_chunk));
	static constexpr std::size_t doubling_records =
		first_chunk * ((std::size_t{1} << doubling_chunks) - 1U);
	static constexpr std::size_t first_capacity = 64;
	/* An entry is a record number, one-based so that zero is empty, below a
	 * six-bit tag. That numbers more records than a table could hold in
	 * any memory the host would give it -- 2.5 GiB of 40-byte ones -- while
	 * a probe still resolves only one mismatched record in 64. */
	static constexpr unsigned number_bits = 26;
	static constexpr std::uint32_t tag_mask =
		~((std::uint32_t{1} << number_bits) - 1U);
	static constexpr std::size_t maximum_records =
		(std::size_t{1} << number_bits) - 1U;

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

	[[nodiscard]] static std::uint32_t tag_of(std::uint64_t hash) noexcept
	{
		return static_cast<std::uint32_t>(hash >> 32) & tag_mask;
	}

	/* The ordinal of the first record chunk `chunk` holds. */
	[[nodiscard]] static std::size_t chunk_first(std::size_t chunk) noexcept
	{
		return chunk < doubling_chunks ?
			first_chunk * ((std::size_t{1} << chunk) - 1U) :
			doubling_records + (chunk - doubling_chunks) * largest_chunk;
	}

	/* Where the record with zero-based ordinal `ordinal` lives. */
	[[nodiscard]] std::byte *place(std::size_t ordinal) const noexcept
	{
		std::size_t chunk = 0;
		std::size_t slot = 0;
		if (ordinal < doubling_records) {
			/* Chunk k begins at first_chunk * (2^k - 1). */
			chunk = static_cast<std::size_t>(
				std::bit_width(ordinal / first_chunk + 1U)) - 1U;
			slot = ordinal - first_chunk * ((std::size_t{1} << chunk) - 1U);
		} else {
			const auto beyond = ordinal - doubling_records;
			chunk = doubling_chunks + beyond / largest_chunk;
			slot = beyond % largest_chunk;
		}
		return chunks_[chunk].storage.get() + slot * sizeof(Record);
	}

	/* The record with one-based number `number`. */
	[[nodiscard]] Record *record_at(std::uint32_t number) const noexcept
	{
		return std::launder(reinterpret_cast<Record *>(place(number - 1U)));
	}

	[[nodiscard]] bool vacant(std::size_t ordinal) const noexcept
	{
		return (vacant_[ordinal / 64] >> (ordinal % 64) & 1U) != 0;
	}

	[[nodiscard]] std::size_t capacity() const noexcept
	{
		return index_ == nullptr ? 0 : mask_ + 1;
	}

	void insert(std::uint32_t number, std::uint64_t hash) noexcept
	{
		assert(number != 0 && (number & tag_mask) == 0);
		auto position = hash & mask_;
		while (index_[position] != 0)
			position = (position + 1) & mask_;
		index_[position] = tag_of(hash) | number;
	}

	void grow()
	{
		const auto next = std::max<std::size_t>(first_capacity, capacity() * 2);
		auto replacement = std::make_unique<std::uint32_t[]>(next);
		auto previous = std::move(index_);
		const auto previous_capacity = previous == nullptr ? 0 : mask_ + 1;
		index_ = std::move(replacement);
		mask_ = next - 1;
		for (std::size_t position = 0; position < previous_capacity; ++position)
			if (const auto entry = previous[position]; entry != 0) {
				const auto number = entry & ~tag_mask;
				insert(number, mix(KeyOf{}(*record_at(number))));
			}
	}

	/* Take a place for a record: the lowest one a sweep left free, or else
	 * a new one carved past the others. */
	[[nodiscard]] std::size_t take()
	{
		if (vacancies_ != 0) {
			auto word = lowest_vacant_;
			while (vacant_[word] == 0)
				++word;
			lowest_vacant_ = word;
			const auto bit = static_cast<std::size_t>(
				std::countr_zero(vacant_[word]));
			vacant_[word] &= vacant_[word] - 1U;
			--vacancies_;
			return word * 64 + bit;
		}
		if (carved_ == maximum_records)
			throw std::bad_alloc{};
		/* Everything that can fail comes before the place is taken. */
		if (carved_ / 64 == vacant_.size())
			vacant_.push_back(0);
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
		++used_;
		return carved_++;
	}

	void release(std::size_t ordinal) noexcept
	{
		std::memset(static_cast<void *>(place(ordinal)), 0, sizeof(Record));
		vacant_[ordinal / 64] |= std::uint64_t{1} << (ordinal % 64);
		++vacancies_;
	}

	/* Return the chunks at the end that no longer hold a record. */
	void trim() noexcept
	{
		while (!chunks_.empty()) {
			const auto first = chunk_first(chunks_.size() - 1U);
			for (auto ordinal = first; ordinal < carved_; ++ordinal)
				if (!vacant(ordinal))
					return;
			for (auto ordinal = first; ordinal < carved_; ++ordinal)
				vacant_[ordinal / 64] &=
					~(std::uint64_t{1} << (ordinal % 64));
			vacancies_ -= carved_ - first;
			carved_ = first;
			chunks_.pop_back();
			used_ = chunks_.empty() ? 0 : chunks_.back().count;
			vacant_.resize((carved_ + 63) / 64);
		}
	}

	std::vector<Chunk> chunks_;
	/* Places carved from the last chunk, and from all of them. */
	std::size_t used_{};
	std::size_t carved_{};
	/* A bit per carved place, set while it is free; the lowest word that
	 * may have one set, and how many are. */
	std::vector<std::uint64_t> vacant_;
	std::size_t lowest_vacant_{};
	std::size_t vacancies_{};
	std::unique_ptr<std::uint32_t[]> index_;
	std::size_t mask_{};
	std::size_t size_{};
};

} // namespace uml::cpu::gadget_cache
