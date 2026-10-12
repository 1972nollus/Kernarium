// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/gadget_victim_tier.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace cache = uml::cpu::gadget_cache;

namespace {

using uml::testing::require;

constexpr cache::TextRange text{0x1'0000'0000ULL, 0x1'0400'0000ULL};
constexpr std::uintptr_t records = 0x6000'1234'5000ULL;

/* A stored translation as the tier is given it. */
struct Translation final {
	std::uint64_t pc{};
	std::uint32_t space{};
	cache::PackedFields fields;
	std::vector<std::uintptr_t> words;
	std::vector<std::byte> source;
};

/* Streams shaped like a cache's: gadget entries from a few hundred, small
 * operands of either sign, pointers into one address space's records, and
 * now and then a word with every bit in use. */
[[nodiscard]] Translation make(std::mt19937_64 &random, std::uint64_t pc,
	std::uint32_t space, std::size_t word_count)
{
	Translation made;
	made.pc = pc;
	made.space = space;
	made.fields = {
		.stop_address = pc + 4 * (random() % 65),
		.stop_instruction = random() % 4 == 0 ?
			static_cast<std::uint32_t>(random()) : 0U,
		.instruction_count = static_cast<std::uint8_t>(random() % 65),
		.flags = static_cast<std::uint8_t>(random() % 4),
	};
	for (std::size_t index = 0; index < word_count; ++index) {
		switch (random() % 8) {
		case 0:
		case 1:
		case 2:
			made.words.push_back(text.begin + 16 * (random() % 600));
			break;
		case 3:
		case 4:
			made.words.push_back(random() % 4096);
			break;
		case 5:
			made.words.push_back(
				static_cast<std::uintptr_t>(-1 -
					static_cast<std::int64_t>(random() % 300)));
			break;
		case 6:
			made.words.push_back(records + 48 * (random() % 50'000));
			break;
		default:
			made.words.push_back(random());
			break;
		}
	}
	made.source.resize(4 * (1 + random() % 65));
	for (auto &byte : made.source)
		byte = static_cast<std::byte>(random());
	return made;
}

void keep(cache::VictimTier &tier, const Translation &translation)
{
	tier.keep(translation.pc, translation.space, translation.fields,
		translation.words, translation.source);
}

[[nodiscard]] bool takes_back(cache::VictimTier &tier,
	const Translation &translation)
{
	const auto taken = tier.take(translation.pc, translation.space);
	if (!taken)
		return false;
	require(taken->fields == translation.fields,
		"the fields come back as they were kept");
	require(std::equal(taken->words.begin(), taken->words.end(),
			translation.words.begin(), translation.words.end()),
		"every word comes back bit for bit");
	require(std::equal(taken->source.begin(), taken->source.end(),
			translation.source.begin(), translation.source.end()),
		"the guest units come back as they were kept");
	return true;
}

/* Everything kept comes back exactly once, and smaller than it went in. */
void round_trip()
{
	std::mt19937_64 random{1};
	cache::VictimTier tier{8U << 20, text};
	std::vector<Translation> kept;
	std::size_t raw = 0;
	for (std::uint64_t index = 0; index < 5'000; ++index) {
		kept.push_back(make(random, 0x40'0000 + 4 * index, 1,
			1 + random() % 200));
		raw += 32 + kept.back().words.size() * 8 +
			kept.back().source.size();
		keep(tier, kept.back());
	}
	require(tier.kept() == kept.size());
	require(tier.written() < raw * 3 / 4,
		"the ring holds the streams packed");
	for (const auto &translation : kept)
		require(takes_back(tier, translation));
	for (const auto &translation : kept)
		require(!tier.take(translation.pc, translation.space),
			"a miss takes an entry out of the tier");
	require(tier.taken() == kept.size());
}

/* The same PC in two address spaces is two entries, and a key kept twice is
 * found as its newer translation. */
void keys()
{
	std::mt19937_64 random{2};
	cache::VictimTier tier{1U << 20, text};
	const auto first = make(random, 0x1000, 1, 20);
	const auto second = make(random, 0x1000, 2, 30);
	keep(tier, first);
	keep(tier, second);
	require(takes_back(tier, second));
	require(takes_back(tier, first));

	const auto older = make(random, 0x2000, 1, 10);
	const auto newer = make(random, 0x2000, 1, 12);
	keep(tier, older);
	keep(tier, newer);
	require(takes_back(tier, newer));
	require(!tier.take(0x2000, 1), "the older copy was replaced");

	keep(tier, first);
	tier.clear();
	require(!tier.take(first.pc, first.space), "clear forgets everything");
	keep(tier, first);
	require(takes_back(tier, first), "a cleared tier keeps again");
}

/* The ring overwrites the oldest entries: one is lost exactly when a ring's
 * worth of bytes has been written after it, whatever position it wrapped
 * at. */
void ring()
{
	std::mt19937_64 random{3};
	constexpr std::size_t capacity = 64U << 10;
	cache::VictimTier tier{capacity, text};
	std::vector<Translation> kept;
	std::vector<std::uint64_t> ends;
	for (std::uint64_t index = 0; index < 4'000; ++index) {
		kept.push_back(make(random, 0x10'0000 + 4 * index, 7,
			1 + random() % 60));
		keep(tier, kept.back());
		ends.push_back(tier.written());
	}
	std::size_t present = 0;
	for (std::size_t index = 0; index < kept.size(); ++index) {
		const bool expected = ends[index] + capacity >
			tier.written() + capacity / 4;
		const bool found = takes_back(tier, kept[index]);
		if (expected)
			require(found, "a recent entry is still in the ring");
		if (ends[index] + capacity <= tier.written())
			require(!found, "an overwritten entry is gone");
		present += found ? 1 : 0;
	}
	require(present > 100 && present < kept.size());
}

/* What the tier cannot hold it drops, and a full dictionary leaves the
 * packing exact. */
void limits()
{
	std::mt19937_64 random{4};
	cache::VictimTier tier{1U << 20, text};
	auto large = make(random, 0x3000, 1,
		cache::VictimTier::maximum_words + 1);
	keep(tier, large);
	require(!tier.take(large.pc, large.space));
	large.words.resize(cache::VictimTier::maximum_words);
	keep(tier, large);
	require(takes_back(tier, large));

	auto wide_source = make(random, 0x3004, 1, 4);
	wide_source.source.resize(cache::VictimTier::maximum_source_bytes + 2);
	keep(tier, wide_source);
	require(!tier.take(wide_source.pc, wide_source.space));

	/* Twice as many distinct gadgets as the dictionary numbers. */
	std::vector<Translation> kept;
	for (std::uint64_t index = 0; index < 1'000; ++index) {
		auto translation = make(random, 0x50'0000 + 4 * index, 3, 0);
		for (std::uint64_t word = 0; word < 8; ++word)
			translation.words.push_back(
				text.begin + 4 * (index * 8 + word));
		keep(tier, translation);
		kept.push_back(std::move(translation));
	}
	for (const auto &translation : kept)
		require(takes_back(tier, translation));

	cache::VictimTier tiny{1024, text};
	keep(tiny, kept.front());
	require(!tiny.take(kept.front().pc, kept.front().space),
		"a tier too small to hold anything stays off");
	require(tiny.footprint() == 0);
	cache::VictimTier unused{1U << 20, text};
	require(unused.footprint() == 0, "nothing is allocated until a keep");
}

} // namespace

int main()
{
	round_trip();
	keys();
	ring();
	limits();
	return 0;
}
