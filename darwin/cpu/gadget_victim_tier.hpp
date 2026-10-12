// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <memory>
#include <new>
#include <optional>
#include <span>

namespace uml::cpu::gadget_cache {

/* The executable's text, which every gadget entry lies in: a stream word
 * inside it names a gadget. */
struct TextRange final {
	std::uintptr_t begin{};
	std::uintptr_t end{};

	[[nodiscard]] bool contains(std::uintptr_t word) const noexcept
	{
		return word >= begin && word < end;
	}
};

/* The text of the image that holds `inside`, or an empty range. */
[[nodiscard]] inline TextRange text_of(const void *inside) noexcept
{
	Dl_info info{};
	if (dladdr(inside, &info) == 0 || info.dli_fbase == nullptr)
		return {};
	unsigned long size = 0;
	const auto *data = getsegmentdata(
		static_cast<const mach_header_64 *>(info.dli_fbase), "__TEXT",
		&size);
	const auto begin = reinterpret_cast<std::uintptr_t>(data);
	return {begin, begin + size};
}

/* What a translation is besides its key, its stream and its guest units: the
 * engine's own fields, which the tier keeps without interpreting them. */
struct PackedFields final {
	std::uint64_t stop_address{};
	std::uint32_t stop_instruction{};
	std::uint8_t instruction_count{};
	std::uint8_t flags{};

	bool operator==(const PackedFields &) const noexcept = default;
};

/* A translation taken back out of the tier. The spans name the tier's own
 * buffers, which the next keep() or take() reuses. */
struct Unpacked final {
	PackedFields fields;
	std::span<const std::uintptr_t> words;
	std::span<const std::byte> source;
};

/* Translations a cache let go of, packed, so that a miss on one can be served
 * by unpacking it rather than translating it again.
 *
 * A cache keeps what its ways evict here, in a ring of bytes: each new entry
 * overwrites the oldest, and an entry is taken out when a miss uses it, so the
 * tier behaves as an exclusive FIFO victim cache measured in bytes. An entry
 * is named by the guest PC and by a number the cache gives each address space
 * it holds; a cache that retires an address space numbers it afresh if it
 * returns, so entries it kept for the old one are never found again.
 *
 * The packing is exact and needs no knowledge of what a word means. Each word
 * gets two bits of tag, four to a byte ahead of their values:
 *
 * - 0: a word in the executable's text, a gadget, as its number in a
 *   dictionary of the text words this tier has seen, a varint;
 * - 1: any other word, zigzagged, a varint;
 * - 2: the difference from the last wide word, zigzagged, a varint, which
 *   is how pointers into one address space's records come out short;
 * - 3: the word as it is, eight bytes.
 *
 * A wide word is any word but a dictionary one that takes five or more bytes
 * as a plain varint; the first a tier ever packs is where every entry's
 * differences start from.
 *
 * Every word comes back bit for bit, so a stream that pointed at records the
 * address space still holds points at them again. The guest units it was
 * validated against are kept as they are, for the cache to validate them
 * again before it uses the stream.
 *
 * Nothing here is shared: one cache keeps and takes, on one thread.
 */
class VictimTier final {
public:
	static constexpr std::size_t maximum_words = 512;
	static constexpr std::size_t maximum_source_bytes = 512;

	VictimTier(std::size_t ring_bytes, TextRange text) noexcept
		: capacity_{ring_bytes}, text_{text}
	{
	}

	VictimTier(const VictimTier &) = delete;
	VictimTier &operator=(const VictimTier &) = delete;

	/* Keep a translation the cache is letting go of. One too large for
	 * the tier, or kept while its storage cannot be allocated, is
	 * dropped. */
	void keep(std::uint64_t pc, std::uint32_t space,
		const PackedFields &fields,
		std::span<const std::uintptr_t> words,
		std::span<const std::byte> source) noexcept
	{
		if (words.size() > maximum_words ||
		    source.size() > maximum_source_bytes || !allocate())
			return;
		const auto length = pack(pc, fields, words, source);
		const auto size = header_bytes + length;
		if (size > capacity_ / 4)
			return;
		auto position = head_;
		if (position % capacity_ + size > capacity_)
			position += capacity_ - position % capacity_;
		head_ = position + size;

		auto *at = ring_.get() + position % capacity_;
		store(at, pc);
		store(at + 8, space);
		store(at + 12, static_cast<std::uint16_t>(length));
		std::memcpy(at + header_bytes, scratch_.get(), length);

		const auto hash = mix(pc, space);
		auto *set = set_of(hash);
		const auto tag = tag_of(hash);
		std::uint64_t *chosen = nullptr;
		for (std::size_t way = 0; way < ways; ++way) {
			auto &entry = set[way];
			if (!live(entry)) {
				if (chosen == nullptr || live(*chosen))
					chosen = &entry;
				continue;
			}
			if ((entry & tag_mask) == tag && names(entry, pc, space)) {
				chosen = &entry;
				break;
			}
			if (chosen == nullptr ||
			    (live(*chosen) && position_of(entry) <
				    position_of(*chosen)))
				chosen = &entry;
		}
		*chosen = tag | (position + 1);
		++kept_;
	}

	/* Take the translation kept for `pc` in address space `space` out of
	 * the tier, if it is still there. */
	[[nodiscard]] std::optional<Unpacked> take(
		std::uint64_t pc, std::uint32_t space) noexcept
	{
		if (index_ == nullptr)
			return std::nullopt;
		const auto hash = mix(pc, space);
		auto *set = set_of(hash);
		const auto tag = tag_of(hash);
		for (std::size_t way = 0; way < ways; ++way) {
			auto &entry = set[way];
			if ((entry & tag_mask) != tag || !live(entry) ||
			    !names(entry, pc, space))
				continue;
			const auto *at = ring_.get() +
				position_of(entry) % capacity_;
			entry = 0;
			++taken_;
			return unpack(pc, at + header_bytes);
		}
		return std::nullopt;
	}

	/* Forget every translation kept so far. */
	void clear() noexcept
	{
		if (index_ != nullptr)
			std::fill_n(index_.get(), index_entries_, 0);
	}

	/* What the tier has allocated: its ring, its index, its dictionary
	 * and its buffers. */
	[[nodiscard]] std::size_t footprint() const noexcept
	{
		if (index_ == nullptr)
			return 0;
		return capacity_ + index_entries_ * sizeof(std::uint64_t) +
			dictionary_slots * (sizeof(std::uintptr_t) +
				sizeof(std::uint16_t)) +
			dictionary_entries * sizeof(std::uintptr_t) +
			scratch_bytes + maximum_words * sizeof(std::uintptr_t) +
			maximum_source_bytes;
	}

	[[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
	[[nodiscard]] std::uint64_t kept() const noexcept { return kept_; }
	[[nodiscard]] std::uint64_t taken() const noexcept { return taken_; }
	/* Bytes the ring has been asked to hold since it was made. */
	[[nodiscard]] std::uint64_t written() const noexcept { return head_; }

private:
	/* The PC, the address space's number and the packed length. */
	static constexpr std::size_t header_bytes = 14;
	/* A set is one 64-byte line of the index. */
	static constexpr std::size_t ways = 8;
	/* An index entry holds the entry's ring position plus one in its low
	 * bits and part of the key's hash in its top sixteen. */
	static constexpr std::uint64_t tag_mask = 0xffffULL << 48;
	static constexpr std::uint64_t position_mask = ~tag_mask;
	/* One index entry for every 64 bytes of ring: about twice as many
	 * as the entries a ring holds, so a set almost never drops one that
	 * is still there. */
	static constexpr std::size_t ring_bytes_per_entry = 64;
	static constexpr std::size_t dictionary_slots = 8192;
	static constexpr std::size_t dictionary_entries = 4096;
	static constexpr std::uint16_t no_entry = 0xffff;
	static constexpr std::size_t scratch_bytes = 32 +
		maximum_source_bytes + (maximum_words + 3) / 4 +
		maximum_words * 10;

	template <typename Value>
	static void store(std::byte *at, Value value) noexcept
	{
		std::memcpy(at, &value, sizeof(value));
	}

	template <typename Value>
	[[nodiscard]] static Value load(const std::byte *at) noexcept
	{
		Value value;
		std::memcpy(&value, at, sizeof(value));
		return value;
	}

	[[nodiscard]] static std::uint64_t mix(
		std::uint64_t pc, std::uint32_t space) noexcept
	{
		auto value = pc ^ (std::uint64_t{space} * 0x9e3779b97f4a7c15ULL);
		value ^= value >> 33;
		value *= 0xff51afd7ed558ccdULL;
		value ^= value >> 33;
		value *= 0xc4ceb9fe1a85ec53ULL;
		value ^= value >> 33;
		return value;
	}

	[[nodiscard]] static std::uint64_t tag_of(std::uint64_t hash) noexcept
	{
		return hash & tag_mask;
	}

	[[nodiscard]] std::uint64_t *set_of(std::uint64_t hash) const noexcept
	{
		return index_.get() + (hash & (index_entries_ / ways - 1)) * ways;
	}

	[[nodiscard]] static std::uint64_t position_of(
		std::uint64_t entry) noexcept
	{
		return (entry & position_mask) - 1;
	}

	/* An entry the ring has not overwritten since it was kept. */
	[[nodiscard]] bool live(std::uint64_t entry) const noexcept
	{
		return (entry & position_mask) != 0 &&
			position_of(entry) + capacity_ >= head_;
	}

	[[nodiscard]] bool names(std::uint64_t entry, std::uint64_t pc,
		std::uint32_t space) const noexcept
	{
		const auto *at = ring_.get() + position_of(entry) % capacity_;
		return load<std::uint64_t>(at) == pc &&
			load<std::uint32_t>(at + 8) == space;
	}

	[[nodiscard]] bool allocate() noexcept
	{
		if (index_ != nullptr)
			return true;
		if (capacity_ < 4096 || failed_)
			return false;
		index_entries_ = std::bit_ceil(std::max<std::size_t>(
			capacity_ / ring_bytes_per_entry, ways));
		ring_.reset(new (std::nothrow) std::byte[capacity_]);
		dictionary_keys_.reset(
			new (std::nothrow) std::uintptr_t[dictionary_slots]());
		dictionary_numbers_.reset(
			new (std::nothrow) std::uint16_t[dictionary_slots]);
		dictionary_words_.reset(
			new (std::nothrow) std::uintptr_t[dictionary_entries]);
		scratch_.reset(new (std::nothrow) std::byte[scratch_bytes]);
		words_.reset(new (std::nothrow) std::uintptr_t[maximum_words]);
		source_.reset(new (std::nothrow) std::byte[maximum_source_bytes]);
		std::unique_ptr<std::uint64_t[]> index{
			new (std::nothrow) std::uint64_t[index_entries_]()};
		if (ring_ == nullptr || dictionary_keys_ == nullptr ||
		    dictionary_numbers_ == nullptr ||
		    dictionary_words_ == nullptr || scratch_ == nullptr ||
		    words_ == nullptr || source_ == nullptr || index == nullptr) {
			failed_ = true;
			return false;
		}
		std::fill_n(dictionary_numbers_.get(), dictionary_slots,
			no_entry);
		index_ = std::move(index);
		return true;
	}

	/* The dictionary number of a text word, made on first sight, or
	 * no_entry once the dictionary is full. */
	[[nodiscard]] std::uint16_t number_of(std::uintptr_t word) noexcept
	{
		auto slot = static_cast<std::size_t>(
			(word >> 2) * 0x9e3779b97f4a7c15ULL >> 51) &
			(dictionary_slots - 1);
		for (;; slot = (slot + 1) & (dictionary_slots - 1)) {
			const auto number = dictionary_numbers_[slot];
			if (number == no_entry)
				break;
			if (dictionary_keys_[slot] == word)
				return number;
		}
		if (dictionary_used_ == dictionary_entries)
			return no_entry;
		const auto number = static_cast<std::uint16_t>(dictionary_used_++);
		dictionary_keys_[slot] = word;
		dictionary_numbers_[slot] = number;
		dictionary_words_[number] = word;
		return number;
	}

	[[nodiscard]] static std::uint64_t zigzag(std::uint64_t value) noexcept
	{
		return (value << 1) ^
			static_cast<std::uint64_t>(static_cast<std::int64_t>(value) >> 63);
	}

	[[nodiscard]] static std::uint64_t unzigzag(std::uint64_t value) noexcept
	{
		return (value >> 1) ^ (0 - (value & 1));
	}

	/* Five varint bytes or more: a pointer rather than an operand. */
	[[nodiscard]] static bool wide(std::uintptr_t word) noexcept
	{
		return zigzag(word) >= (std::uint64_t{1} << 28);
	}

	[[nodiscard]] static std::size_t varint_bytes(std::uint64_t value) noexcept
	{
		return value == 0 ? 1 :
			(static_cast<std::size_t>(std::bit_width(value)) + 6) / 7;
	}

	static std::byte *put_varint(std::byte *at, std::uint64_t value) noexcept
	{
		while (value >= 0x80) {
			*at++ = static_cast<std::byte>(value | 0x80);
			value >>= 7;
		}
		*at++ = static_cast<std::byte>(value);
		return at;
	}

	[[nodiscard]] static std::uint64_t get_varint(
		const std::byte *&at) noexcept
	{
		std::uint64_t value = 0;
		for (unsigned shift = 0;; shift += 7) {
			const auto byte = std::to_integer<std::uint64_t>(*at++);
			value |= (byte & 0x7f) << shift;
			if ((byte & 0x80) == 0)
				return value;
		}
	}

	/* Pack into the scratch buffer; returns the packed length. */
	[[nodiscard]] std::size_t pack(std::uint64_t pc,
		const PackedFields &fields,
		std::span<const std::uintptr_t> words,
		std::span<const std::byte> source) noexcept
	{
		auto *at = scratch_.get();
		at = put_varint(at, words.size());
		*at++ = static_cast<std::byte>(fields.instruction_count);
		*at++ = static_cast<std::byte>(fields.flags);
		at = put_varint(at, source.size());
		at = put_varint(at, zigzag(fields.stop_address - pc));
		at = put_varint(at, fields.stop_instruction);
		if (!source.empty())
			std::memcpy(at, source.data(), source.size());
		at += source.size();
		if (!based_)
			for (const auto word : words)
				if (!text_.contains(word) && wide(word)) {
					base_ = word;
					based_ = true;
					break;
				}
		auto previous = base_;
		std::byte *tags = nullptr;
		for (std::size_t index = 0; index < words.size(); ++index) {
			if (index % 4 == 0) {
				tags = at++;
				*tags = std::byte{0};
			}
			const auto word = words[index];
			unsigned tag = 3;
			if (text_.contains(word)) {
				if (const auto number = number_of(word);
				    number != no_entry) {
					tag = 0;
					at = put_varint(at, number);
				}
			}
			if (tag == 3) {
				const auto plain = zigzag(word);
				const auto delta = zigzag(word - previous);
				const auto plain_bytes = varint_bytes(plain);
				const auto delta_bytes = varint_bytes(delta);
				if (std::min(plain_bytes, delta_bytes) >= 9) {
					std::memcpy(at, &word, sizeof(word));
					at += sizeof(word);
				} else if (delta_bytes < plain_bytes) {
					tag = 2;
					at = put_varint(at, delta);
				} else {
					tag = 1;
					at = put_varint(at, plain);
				}
				if (wide(word))
					previous = word;
			}
			*tags |= static_cast<std::byte>(tag << (index % 4 * 2));
		}
		return static_cast<std::size_t>(at - scratch_.get());
	}

	[[nodiscard]] Unpacked unpack(std::uint64_t pc,
		const std::byte *at) noexcept
	{
		const auto count = static_cast<std::size_t>(get_varint(at));
		PackedFields fields;
		fields.instruction_count = std::to_integer<std::uint8_t>(*at++);
		fields.flags = std::to_integer<std::uint8_t>(*at++);
		const auto source_bytes = static_cast<std::size_t>(get_varint(at));
		fields.stop_address = pc + unzigzag(get_varint(at));
		fields.stop_instruction =
			static_cast<std::uint32_t>(get_varint(at));
		if (source_bytes != 0)
			std::memcpy(source_.get(), at, source_bytes);
		at += source_bytes;
		auto previous = base_;
		unsigned tags = 0;
		for (std::size_t index = 0; index < count; ++index) {
			if (index % 4 == 0)
				tags = std::to_integer<unsigned>(*at++);
			std::uintptr_t word = 0;
			switch (tags & 3) {
			case 0:
				word = dictionary_words_[get_varint(at)];
				break;
			case 1:
				word = unzigzag(get_varint(at));
				break;
			case 2:
				word = previous + unzigzag(get_varint(at));
				break;
			default:
				std::memcpy(&word, at, sizeof(word));
				at += sizeof(word);
				break;
			}
			if ((tags & 3) != 0 && wide(word))
				previous = word;
			tags >>= 2;
			words_[index] = word;
		}
		return {fields, {words_.get(), count},
			{source_.get(), source_bytes}};
	}

	std::size_t capacity_;
	TextRange text_;
	bool failed_{};
	bool based_{};
	std::uintptr_t base_{};
	/* Where the next entry goes, counted in bytes from the ring's first
	 * use: an entry at an earlier position is intact while fewer than
	 * the ring's capacity have been written after it. */
	std::uint64_t head_{};
	std::size_t index_entries_{};
	std::unique_ptr<std::byte[]> ring_;
	std::unique_ptr<std::uint64_t[]> index_;
	std::unique_ptr<std::uintptr_t[]> dictionary_keys_;
	std::unique_ptr<std::uint16_t[]> dictionary_numbers_;
	std::unique_ptr<std::uintptr_t[]> dictionary_words_;
	std::size_t dictionary_used_{};
	std::unique_ptr<std::byte[]> scratch_;
	std::unique_ptr<std::uintptr_t[]> words_;
	std::unique_ptr<std::byte[]> source_;
	std::uint64_t kept_{};
	std::uint64_t taken_{};
};

} // namespace uml::cpu::gadget_cache
