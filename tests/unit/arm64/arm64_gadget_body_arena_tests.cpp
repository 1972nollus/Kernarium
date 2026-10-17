// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/body_arena.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <memory_resource>
#include <random>
#include <vector>

#include <sys/mman.h>

namespace gadgets = uml::cpu::arm64::gadgets;
using uml::testing::require;

namespace {

/* What the arena maps for a chunk: whole chunks are its unit of return. */
constexpr std::size_t chunk_bytes = std::size_t{64} << 10;
constexpr std::size_t granule = 16;
/* Larger blocks are not carved out of a chunk. */
constexpr std::size_t largest_class = 1023 * granule;

struct Block final {
	std::byte *address;
	std::size_t bytes;
	std::uint8_t pattern;
};

/* An arena beside a model of what it hands out: every live block, its extent
 * and the byte it was filled with. */
class Model final {
public:
	explicit Model(std::uint64_t seed) : random_{seed} {}

	[[nodiscard]] gadgets::BodyArena &arena() { return *arena_; }
	[[nodiscard]] std::size_t live() const { return blocks_.size(); }

	void allocate(std::size_t bytes)
	{
		auto *address = static_cast<std::byte *>(arena_->allocate(bytes));
		const auto extent = rounded(bytes);
		/* No block may overlap another live block. */
		const auto next = extents_.lower_bound(
			reinterpret_cast<std::uintptr_t>(address));
		if (next != extents_.end())
			require(next->first >=
				reinterpret_cast<std::uintptr_t>(address) + extent);
		if (next != extents_.begin()) {
			const auto before = std::prev(next);
			require(before->first + before->second <=
				reinterpret_cast<std::uintptr_t>(address));
		}
		extents_[reinterpret_cast<std::uintptr_t>(address)] = extent;
		const auto pattern = static_cast<std::uint8_t>(++serial_ | 1U);
		std::memset(address, pattern, bytes);
		blocks_.push_back({address, bytes, pattern});
	}

	/* Free the block at `index`, which must still hold what it was given. */
	void release(std::size_t index)
	{
		const auto block = blocks_[index];
		check(block);
		extents_.erase(reinterpret_cast<std::uintptr_t>(block.address));
		arena_->deallocate(block.address, block.bytes);
		blocks_[index] = blocks_.back();
		blocks_.pop_back();
	}

	void release_random()
	{
		release(random_() % blocks_.size());
	}

	/* Every live block is intact, and the arena names it from any address
	 * inside it. */
	void verify()
	{
		for (const auto &block : blocks_) {
			check(block);
			const auto inside = random_() % block.bytes;
			require(arena_->owner(block.address + inside) ==
				block.address);
			require(arena_->owner(block.address) == block.address);
		}
	}

	[[nodiscard]] std::size_t random(std::size_t bound)
	{
		return static_cast<std::size_t>(random_() % bound);
	}

	/* A block size as the cache sees them: mostly some hundreds of bytes,
	 * sometimes much more, rarely too large for a chunk's classes. */
	[[nodiscard]] std::size_t body_bytes()
	{
		const auto pick = random(100);
		if (pick < 70)
			return 16 + random(480);
		if (pick < 95)
			return 16 + random(3000);
		if (pick < 99)
			return 16 + random(largest_class - 16);
		return largest_class + 1 + random(40000);
	}

	std::vector<Block> blocks_;

private:
	[[nodiscard]] static std::size_t rounded(std::size_t bytes)
	{
		return (std::max(bytes, granule) + granule - 1) & ~(granule - 1);
	}

	static void check(const Block &block)
	{
		for (std::size_t at = 0; at < block.bytes; ++at)
			require(static_cast<std::uint8_t>(block.address[at]) ==
				block.pattern);
	}

	std::mt19937_64 random_;
	std::uint32_t serial_{};
	std::map<std::uintptr_t, std::size_t> extents_;
	std::unique_ptr<gadgets::BodyArena> arena_ =
		std::make_unique<gadgets::BodyArena>(
			std::pmr::new_delete_resource());
};

/* An arena that grows over many chunks keeps every block intact and findable
 * while the chunks in front of it empty and are given back, and carves the
 * next blocks from what is left of what it mapped. */
void chunks_emptied_in_front_are_given_back()
{
	Model model{1};
	for (std::size_t count = 0; count < 6000; ++count)
		model.allocate(16 + model.random(992));
	require(model.arena().bytes() >= 40 * chunk_bytes);
	model.verify();

	/* The oldest third, which fills the first chunks whole. */
	const auto grown = model.arena().bytes();
	const auto third = model.live() / 3;
	for (std::size_t count = 0; count < third; ++count) {
		std::size_t oldest = 0;
		for (std::size_t index = 1; index < model.blocks_.size(); ++index)
			if (model.blocks_[index].address <
				model.blocks_[oldest].address)
				oldest = index;
		model.release(oldest);
	}
	require(model.arena().bytes() < grown);
	model.verify();

	/* Holes first, then fresh chunks, then holes again. */
	for (std::size_t count = 0; count < 4000; ++count)
		model.allocate(model.body_bytes());
	model.verify();

	while (model.live() != 0)
		model.release_random();
	/* The chunk being carved stays; the rest is returned. */
	require(model.arena().bytes() <= chunk_bytes);
}

/* Blocks are allocated and freed at random, in sizes that reach every class
 * and the blocks too large to be carved. */
void random_churn_keeps_every_block_intact()
{
	Model model{2};
	for (std::size_t step = 0; step < 300000; ++step) {
		const auto live = model.live();
		/* The live count wanders between a few and some thousands, so
		 * chunks fill, empty and fill again. */
		const auto freeing = live > 3500 ||
			(live > 40 && model.random(100) < 49);
		if (freeing)
			model.release_random();
		else
			model.allocate(model.body_bytes());
		if (step % 25000 == 24999)
			model.verify();
	}
	model.verify();
	while (model.live() != 0)
		model.release_random();
	require(model.arena().bytes() <= chunk_bytes);
}

/* An arena that is destroyed gives back what it still holds and nothing that
 * it gave back before: an address it released may be another mapping's by
 * then. The system decides whether the mappings made meanwhile take any of
 * the released addresses; with chunks gone from the middle of the arena's
 * range, a good share of them do. */
void destruction_leaves_released_addresses_alone(std::uint64_t seed)
{
	std::vector<void *> foreign;
	{
		Model model{seed};
		for (std::size_t count = 0; count < 4000; ++count)
			model.allocate(16 + model.random(1000));
		const auto first = std::min_element(model.blocks_.begin(),
			model.blocks_.end(), [](const Block &a, const Block &b) {
				return a.address < b.address;
			})->address;
		/* Free every block but those of every third stretch of four
		 * chunks, so chunks between them empty while the mappings
		 * that hold them stay in use. Walking down keeps the blocks
		 * still to look at where they were. */
		for (std::size_t index = model.blocks_.size(); index-- > 0;) {
			const auto stretch = static_cast<std::size_t>(
				model.blocks_[index].address - first) /
				(4 * chunk_bytes);
			if (stretch % 3 != 0)
				model.release(index);
		}
		model.verify();
		/* Mappings of our own, made while the arena holds gaps in its
		 * address range, may take some of them. */
		for (std::size_t count = 0; count < 256; ++count) {
			void *base = mmap(nullptr, chunk_bytes,
				PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
				-1, 0);
			require(base != MAP_FAILED);
			std::memset(base, 0xc3, chunk_bytes);
			foreign.push_back(base);
		}
	}
	/* The arena is gone; nothing of ours went with it. */
	for (auto *base : foreign) {
		const auto *bytes = static_cast<const std::uint8_t *>(base);
		for (std::size_t offset = 0; offset < chunk_bytes; offset += 4096)
			require(bytes[offset] == 0xc3);
		require(munmap(base, chunk_bytes) == 0);
	}
}

} // namespace

int main()
{
	chunks_emptied_in_front_are_given_back();
	random_churn_keeps_every_block_intact();
	for (std::uint64_t seed = 10; seed < 30; ++seed)
		destruction_leaves_released_addresses_alone(seed);
	return 0;
}
