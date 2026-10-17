// SPDX-License-Identifier: GPL-2.0-or-later
#include "executor.hpp"

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/guest_validation.hpp"
#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/gadgets/a32/a32_builder.hpp"
#include "cpu/arm64/gadgets/a32_support.hpp"
#include "cpu/gadget_cache_geometry.hpp"
#include "cpu/gadget_record_table.hpp"
#if UML_GADGET_CACHE_CENSUS
#include "cpu/gadget_cache_census.hpp"
#endif
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#if UML_GADGET_CACHE_CENSUS
#include <cstdio>
#endif
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uml::cpu::arm64::gadgets {
using ::uml::memory::StaticGuestMemory;
#if UML_GADGET_CACHE_CENSUS
namespace census = ::uml::cpu::gadget_cache::census;
#endif
namespace {


/* darwin/cpu/CMakeLists.txt defines it for this translation unit, the only
 * one that reads it. */
#ifndef UML_GADGET_BLOCK_INSTRUCTIONS
#error "UML_GADGET_BLOCK_INSTRUCTIONS must come from the build"
#endif
constexpr std::size_t block_instruction_count = UML_GADGET_BLOCK_INSTRUCTIONS;
constexpr std::size_t guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE;
/* A T32 translation validates halfwords: two for each of its instructions,
 * and the two a stopped build read without retiring. */
constexpr std::size_t maximum_t32_validation_units =
	2 * block_instruction_count + 2;

/* A translation cache slot's tag holds the translation's key in its low 48
 * bits, under the id of the address space that owns it. An A64 key is a user
 * PC below the guest address limit (2^47, DARWIN_UML_GUEST_ADDRESS_LIMIT) and
 * is kept as it is; an AArch32 key keeps its 34 significant low bits under
 * two flags in the bits a PC of that width never uses, so no two canonical
 * keys share a compact form. A key outside both has none: it tags to a value
 * no slot holds, which is a miss. */
constexpr unsigned tag_owner_shift = 48;
constexpr std::uint64_t tag_key_mask =
	(std::uint64_t{1} << tag_owner_shift) - 1U;
constexpr unsigned a64_key_bits = 47;
constexpr std::uint64_t aarch32_flag = std::uint64_t{1} << a64_key_bits;
constexpr std::uint64_t thumb_flag = std::uint64_t{1} << (a64_key_bits - 1U);
constexpr std::uint64_t aarch32_address_mask =
	(std::uint64_t{1} << 34) - 1U;
constexpr std::uint64_t unmatchable_tag = ~std::uint64_t{};

[[nodiscard]] constexpr std::uint64_t compact_tag_key(
	std::uint64_t key) noexcept
{
	if ((key >> a64_key_bits) == 0U) [[likely]]
		return key;
	if (!arm64::aarch32::key_is_canonical(key) ||
	    !arm64::aarch32::key_is_aarch32(key))
		return unmatchable_tag;
	return aarch32_flag |
		(arm64::aarch32::key_is_thumb(key) ? thumb_flag : 0U) |
		(key & aarch32_address_mask);
}

[[nodiscard]] constexpr std::uint64_t expand_tag_key(
	std::uint64_t tag) noexcept
{
	tag &= tag_key_mask;
	if ((tag & aarch32_flag) == 0U)
		return tag;
	return arm64::aarch32::aarch32_key_bit |
		((tag & thumb_flag) != 0U ? arm64::aarch32::thumb_key_bit : 0U) |
		(tag & aarch32_address_mask);
}

static_assert(expand_tag_key(compact_tag_key(0x0000'7fff'ffff'fffcU)) ==
	0x0000'7fff'ffff'fffcU);
static_assert(expand_tag_key(compact_tag_key(0)) == 0);
static_assert(expand_tag_key(compact_tag_key(
	arm64::aarch32::a32_key(0xffff'fffcU))) ==
	arm64::aarch32::a32_key(0xffff'fffcU));
static_assert(expand_tag_key(compact_tag_key(
	arm64::aarch32::t32_key(0xffff'fffeU))) ==
	arm64::aarch32::t32_key(0xffff'fffeU));
static_assert(compact_tag_key(arm64::aarch32::a32_key(0x1000U)) !=
	compact_tag_key(arm64::aarch32::t32_key(0x1000U)));
static_assert(compact_tag_key(0x0000'8000'0000'1000U) == unmatchable_tag);
static_assert(compact_tag_key(arm64::aarch32::a32_key(0x1002U)) ==
	unmatchable_tag);

[[nodiscard]] std::size_t fetch_instruction_window(
	StaticGuestMemory &memory, std::uint64_t pc,
	std::span<std::uint32_t> instructions)
{
	std::size_t count = 0;
	auto bytes = std::as_writable_bytes(instructions);
	while (count < instructions.size()) {
		const auto address = pc + count * sizeof(std::uint32_t);
		const auto page_remaining =
			guest_page_size - (address & (guest_page_size - 1));
		auto page_instructions = std::min<std::size_t>(
			instructions.size() - count,
			static_cast<std::size_t>(
				page_remaining / sizeof(std::uint32_t)));
		/* A valid A64 PC is word aligned. Preserve deterministic fault
		 * handling for a malformed PC instead of constructing an empty span.
		 */
		if (page_instructions == 0)
			page_instructions = 1;
		const auto offset = count * sizeof(std::uint32_t);
		const auto length = page_instructions * sizeof(std::uint32_t);
		if (!memory.fetch(address, bytes.subspan(offset, length)))
			break;
		count += page_instructions;
	}
	return count;
}

/* The guest units of an AArch32 translation window starting at `address`,
 * fetched a page at a time as fetch_instruction_window does, in units of
 * `unit` bytes (a word for A32, a halfword for T32). A page that cannot be
 * fetched ends the window; none at all is zero. */
[[nodiscard]] std::size_t fetch_a32_units(StaticGuestMemory &memory,
	std::uint64_t address, std::size_t unit, std::span<std::byte> bytes)
{
	const std::size_t capacity = bytes.size() / unit;
	std::size_t count = 0;
	while (count < capacity) {
		const auto at = address + count * unit;
		const auto page_remaining =
			guest_page_size - (at & (guest_page_size - 1));
		auto run = std::min<std::size_t>(capacity - count,
			static_cast<std::size_t>(page_remaining / unit));
		if (run == 0)
			run = 1;
		if (!memory.fetch(at, bytes.subspan(count * unit, run * unit)))
			break;
		count += run;
	}
	return count;
}

/* BKPT's immediate, from the instruction at `key`: imm12:imm4 for A32 and
 * imm8 for T32. */
[[nodiscard]] std::uint32_t read_breakpoint_immediate(
	StaticGuestMemory &memory, std::uint64_t key) noexcept
{
	if (arm64::aarch32::key_is_thumb(key)) {
		std::uint16_t halfword = 0;
		if (!memory.fetch(arm64::aarch32::key_pc(key),
				  std::as_writable_bytes(std::span{&halfword, 1})))
			return 0;
		return halfword & 0xffU;
	}
	std::uint32_t word = 0;
	if (!memory.fetch(arm64::aarch32::key_pc(key),
			  std::as_writable_bytes(std::span{&word, 1})))
		return 0;
	return (word >> 4 & 0xfff0U) | (word & 0xfU);
}

void correct_terminal_instruction_count(
	const arm64::gadgets::Program &program, ::uml::cpu::EngineEvent event,
	std::uint64_t fault_pc, std::size_t &executed) noexcept
{
	if (!terminal_does_not_retire(event))
		return;

	/* program_position answers only inside the program, so the position
	 * is always below its instruction count. */
	const auto position = program_position(program, fault_pc);
	if (!position)
		return;
	const auto unretired = program.instruction_count() - *position;
	if (executed < unretired)
		return;

	/* The assembly engine precharges a block's full translated length.  A
	 * non-retiring terminal stores its own PC, allowing the cold C++ boundary
	 * to subtract the current block's unexecuted suffix. Counts accumulated
	 * before a linked successor are retained.
	 */
	executed -= unretired;
}

void correct_fpcr_enable_instruction_count(
	const arm64::gadgets::Program &program, ::uml::cpu::EngineEvent event,
	const arm64::CpuState &cpu, std::size_t &executed) noexcept
{
	if (event != ::uml::cpu::EngineEvent::continued ||
	    (cpu.effective_fpcr() &
	     arm64::detail::fpcr_exception_enable_mask) == 0U)
		return;

	/* The exit is the fallthrough of the FPCR write, so it is either inside
	 * the program or one past its end. */
	std::size_t retired = 0;
	if (const auto position = program_position(program, cpu.pc); position) {
		retired = *position;
	} else {
		if (cpu.pc != program.guest_address() +
				std::uint64_t{program.instruction_count()} *
				sizeof(std::uint32_t))
			return;
		retired = program.instruction_count();
	}
	if (retired == 0)
		return;
	const auto unretired = program.instruction_count() - retired;
	if (executed < unretired)
		return;

	/* An FPCR write that newly enables traps exits at its translated
	 * fallthrough PC. It retires, but later words in the precharged block do
	 * not. Unresolved control-flow exits cannot satisfy the enabled-state
	 * condition because enabled entries use the one-instruction cold path.
	 */
	executed -= unretired;
}

} // namespace

constexpr std::size_t uml_gadget_cache_slots = ::uml::cpu::gadget_cache::slots;
/* How many address spaces may hold translations at once.
 *
 * Sharing one pool without a bound is not a cache: a guest that starts and
 * exits processes accumulates every address space it ever ran, and the handful
 * that are actually live end up with a few dozen slots each. Reuse measured on
 * a browser guest returns to one of the last eight distinct address spaces
 * 94.5% of the time, so bounding residency there keeps capacity for the ones
 * still running while costing almost no returns.
 */
constexpr std::size_t uml_gadget_resident_generations =
	::uml::cpu::gadget_cache::resident_generations;

extern "C" void uml_arm64_gadget_link_exit();
extern "C" void uml_arm64_gadget_fp_link_entry();

class CountingResource final : public std::pmr::memory_resource {
public:
	[[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

private:
	void *do_allocate(std::size_t bytes, std::size_t alignment) override
	{
		bytes_ += bytes;
		return std::pmr::new_delete_resource()->allocate(bytes, alignment);
	}

	void do_deallocate(void *pointer, std::size_t bytes,
			   std::size_t alignment) override
	{
		bytes_ -= bytes;
		std::pmr::new_delete_resource()->deallocate(
			pointer, bytes, alignment);
	}

	[[nodiscard]] bool do_is_equal(
		const std::pmr::memory_resource &other) const noexcept override
	{
		return this == &other;
	}

	std::size_t bytes_{};
};

/* Storage for stored translations.
 *
 * A translation is one allocation: its body, the stream words behind it, and
 * the guest words it is validated against behind those, sized exactly. The
 * cache holds tens of thousands of them per executor, so a rounding allocator
 * costs more than the streams do: a pmr pool asked its upstream for 204 MiB to
 * hold 64 MiB of programs on a measured six-vCPU guest, between its own size
 * classes and the capacity a stream reached by doubling. Sixteen-byte classes
 * over bump-allocated chunks hold the same programs in 65 MiB.
 *
 * Freed bodies return to the class they came from, so retranslating a block
 * reuses its predecessor's bytes. Each address space has an arena of its own,
 * and its chunks are returned when the address space's translations are, so
 * retiring an address space returns its memory instead of leaving holes in
 * chunks other address spaces still use. Chunks start small and double, so an
 * address space that runs little code costs little. See G167.
 *
 * The arena also answers which live body holds an address. Each chunk keeps
 * one bit per granule, set at the first granule of every live block, so the
 * block holding an address is the nearest set bit at or below it. The cache
 * asks this when a terminal exit lands in a linked successor, which a
 * measured guest did about once per hundred and twenty stores; the index by
 * stream address that this replaces was maintained at every one of those
 * stores, five set installs per program. See G203.
 */
class BodyArena final {
public:
	explicit BodyArena(std::pmr::memory_resource *upstream) noexcept
		: upstream_{upstream}
	{}

	BodyArena(const BodyArena &) = delete;
	BodyArena &operator=(const BodyArena &) = delete;

	~BodyArena()
	{
		for (const auto &chunk : chunks_)
			upstream_->deallocate(chunk.base, chunk.bytes,
				alignof(std::max_align_t));
		for (const auto &direct : direct_)
			upstream_->deallocate(direct.base, direct.bytes,
				alignof(std::max_align_t));
	}

	[[nodiscard]] void *allocate(std::size_t bytes)
	{
		const auto size = rounded(bytes);
		if (size > largest_class) {
			/* Too big for a class to recycle, so it is not carved
			 * out of a chunk either; it is freed on its own. */
			auto *block = static_cast<std::byte *>(
				upstream_->allocate(size,
					alignof(std::max_align_t)));
			direct_.push_back({block, size});
			return block;
		}
		if (auto *block = free_[size / granule]; block != nullptr) {
			free_[size / granule] = next_of(block);
			mark(block, true);
			return block;
		}
		if (size > limit_ - used_) {
			const auto wanted = std::max(size, next_chunk_);
			next_chunk_ = std::min(next_chunk_ * 2, largest_chunk);
			auto starts = std::make_unique<std::uint64_t[]>(
				map_words(wanted));
			auto *base = static_cast<std::byte *>(
				upstream_->allocate(wanted,
					alignof(std::max_align_t)));
			/* Kept in address order, which is what owner() searches. */
			const auto inserted = chunks_.insert(
				std::upper_bound(chunks_.begin(), chunks_.end(),
					base, before_chunk),
				Chunk{base, wanted, std::move(starts)});
			/* The tail of the chunk being left behind is at most one
			 * body, and one partly used chunk is the arena's whole
			 * slack. */
			current_ = base;
			current_starts_ = inserted->starts.get();
			used_ = 0;
			limit_ = wanted;
		}
		auto *block = current_ + used_;
		current_starts_[used_ / granule / 64] |=
			std::uint64_t{1} << (used_ / granule % 64);
		used_ += size;
		return block;
	}

	void deallocate(void *block, std::size_t bytes) noexcept
	{
		const auto size = rounded(bytes);
		auto *freed = static_cast<std::byte *>(block);
		if (size > largest_class) {
			const auto held = std::find_if(direct_.begin(),
				direct_.end(), [freed](const Direct &direct) {
					return direct.base == freed;
				});
			if (held != direct_.end())
				direct_.erase(held);
			upstream_->deallocate(block, size,
				alignof(std::max_align_t));
			return;
		}
		mark(freed, false);
		next_of(freed) = free_[size / granule];
		free_[size / granule] = freed;
	}

	/* The live block holding an address, or nullptr. */
	[[nodiscard]] void *owner(const void *address) const noexcept
	{
		const auto *at = static_cast<const std::byte *>(address);
		if (const auto *chunk = chunk_of(at); chunk != nullptr) {
			const auto index =
				static_cast<std::size_t>(at - chunk->base) / granule;
			/* Nothing in a chunk spans more than the largest class,
			 * so a start any further below is another block's. */
			const auto floor = index >= largest_class / granule ?
				(index - largest_class / granule) / 64 : 0;
			auto word = index / 64;
			auto bits = chunk->starts[word] &
				(~std::uint64_t{0} >> (63 - index % 64));
			for (;;) {
				if (bits != 0)
					return chunk->base +
						(word * 64 + 63 -
							static_cast<std::size_t>(
								std::countl_zero(bits))) *
						granule;
				if (word == floor)
					return nullptr;
				bits = chunk->starts[--word];
			}
		}
		for (const auto &direct : direct_)
			if (at >= direct.base && at < direct.base + direct.bytes)
				return direct.base;
		return nullptr;
	}

	/* What the arena holds, which is what the cache costs. */
	[[nodiscard]] std::size_t bytes() const noexcept
	{
		std::size_t total = 0;
		for (const auto &direct : direct_)
			total += direct.bytes;
		for (const auto &chunk : chunks_)
			total += chunk.bytes;
		return total;
	}

	/* The start maps, which are what owner() costs. */
	[[nodiscard]] std::size_t index_bytes() const noexcept
	{
		std::size_t total = 0;
		for (const auto &chunk : chunks_)
			total += map_words(chunk.bytes) * sizeof(std::uint64_t);
		return total;
	}

#if UML_GADGET_CACHE_CENSUS
	/* Chunks that no longer hold a live body, which the arena keeps until
	 * the cache is destroyed. Their blocks wait on the class free lists. */
	[[nodiscard]] std::size_t empty_bytes() const noexcept
	{
		std::size_t total = 0;
		for (const auto &chunk : chunks_) {
			if (chunk.base == current_)
				continue;
			const auto *starts = chunk.starts.get();
			if (std::all_of(starts, starts + map_words(chunk.bytes),
				    [](std::uint64_t word) { return word == 0; }))
				total += chunk.bytes;
		}
		return total;
	}
#endif

private:
	/* Eight bytes would fit the free-list link, but sixteen keeps every
	 * body's stream tail aligned for a pair access and still rounds a mean
	 * body by eight bytes. */
	static constexpr std::size_t granule = 16;
	/* Up to 16 KiB in classes, which a 64-instruction block reaches only at
	 * 32 stream words per guest instruction. */
	static constexpr std::size_t classes = 1024;
	static constexpr std::size_t largest_class = (classes - 1) * granule;
	static constexpr std::size_t first_chunk = std::size_t{32} << 10;
	static constexpr std::size_t largest_chunk = std::size_t{256} << 10;

	[[nodiscard]] static std::size_t rounded(std::size_t bytes) noexcept
	{
		return (std::max(bytes, granule) + granule - 1) & ~(granule - 1);
	}

	[[nodiscard]] static std::byte *&next_of(std::byte *block) noexcept
	{
		return *reinterpret_cast<std::byte **>(block);
	}

	/* Words in the start map of a chunk of this many bytes. */
	[[nodiscard]] static constexpr std::size_t map_words(
		std::size_t bytes) noexcept
	{
		return (bytes / granule + 63) / 64;
	}

	struct Chunk final {
		std::byte *base;
		std::size_t bytes;
		/* One bit per granule, set at the first granule of a live block. */
		std::unique_ptr<std::uint64_t[]> starts;
	};

	/* A block above the largest class, held outside the chunks. */
	struct Direct final {
		std::byte *base;
		std::size_t bytes;
	};

	[[nodiscard]] static bool before_chunk(
		const std::byte *address, const Chunk &chunk) noexcept
	{
		return address < chunk.base;
	}

	[[nodiscard]] const Chunk *chunk_of(
		const std::byte *address) const noexcept
	{
		const auto after = std::upper_bound(
			chunks_.begin(), chunks_.end(), address, before_chunk);
		if (after == chunks_.begin())
			return nullptr;
		const auto &chunk = *std::prev(after);
		return address < chunk.base + chunk.bytes ? &chunk : nullptr;
	}

	void mark(const std::byte *block, bool live) noexcept
	{
		const auto *chunk = chunk_of(block);
		const auto index =
			static_cast<std::size_t>(block - chunk->base) / granule;
		const auto bit = std::uint64_t{1} << (index % 64);
		if (live)
			chunk->starts[index / 64] |= bit;
		else
			chunk->starts[index / 64] &= ~bit;
	}

	std::pmr::memory_resource *upstream_;
	/* In address order. */
	std::vector<Chunk> chunks_;
	std::vector<Direct> direct_;
	std::byte *current_{};
	std::uint64_t *current_starts_{};
	std::size_t used_{};
	std::size_t limit_{};
	std::size_t next_chunk_{first_chunk};
	std::array<std::byte *, classes> free_{};
};

/* What each executor says about itself for the bodies other executors free.
 *
 * Between begin_execution() and end_execution() an executor may hold a stream
 * pointer it loaded from a link, so a body unpublished in that time may only
 * be freed once the executor has left. The sequence is odd from entry until
 * exit and even outside, and moves at both, so a reclaimer that saw an odd
 * value waits for any other. An executor that never brackets its runs stays
 * odd, which only keeps bodies longer. A guest entry runs at most a dispatch
 * budget, so a running executor leaves within a fraction of a millisecond. */
struct alignas(128) ExecutorAnnouncement final {
	std::atomic<std::uint64_t> sequence{};
	bool used{};
};

namespace {

using Announcement = ExecutorAnnouncement;

/* Publish a pair of words that translated code reads with one `ldp`: a
 * BlockLink's stream and instruction count, or an indirect cell's guest PC
 * and link. With FEAT_LSE2 an aligned sixteen-byte `stp` is a single access,
 * so a vCPU following the pair while another retargets it sees one value or
 * the other, never a stream with another block's count. */
inline void store_pair(void *at, std::uint64_t first,
	std::uint64_t second) noexcept
{
	assert((reinterpret_cast<std::uintptr_t>(at) & 15U) == 0U);
	asm volatile("stp %x1, %x2, [%x0]"
		     :
		     : "r"(at), "r"(first), "r"(second)
		     : "memory");
}

[[nodiscard]] inline std::uint64_t word_of(const void *pointer) noexcept
{
	return reinterpret_cast<std::uintptr_t>(pointer);
}

/* One translation, allocated apart from the slots that index it.
 *
 * A set way exists whether or not it holds a translation, so every byte kept
 * inline is paid for by the whole cache. With the program, its stream and its
 * validation words moved into one separately allocated body, an empty way
 * costs a tag rather than a full BuildResult, and the words a spilling stream
 * used to allocate on their own now share the body's allocation.
 *
 * The body is also paid for by every translation the cache holds, so it keeps
 * only what a stored block cannot recompute. A BuildResult carries a words
 * object able to own and grow its storage, against a mean stream of about 225
 * bytes; a stored block's words are the ones behind this header. result()
 * rebuilds the builder's type from these fields whenever a caller needs one,
 * which is once per entry from C++.
 */
struct Body final {
	/* Where the block starts: its guest PC, or for AArch32 its key. */
	std::uint64_t guest_address{};
	/* The words of the stream, and the facts about it a stored block
	 * cannot recompute, packed into one word. */
	std::uint32_t word_count : 24 {};
	bool complete : 1 {};
	bool accesses_fp_state : 1 {};
	/* A tail after the validation units holds the build's stop (see
	 * Stop). */
	bool has_stop : 1 {};
	/* The translation's aarch32::GuestIsa, from its key: it sizes a unit
	 * and so the validation words. */
	std::uint32_t isa : 2 {};
	/* How many cache slots, in every executor running the address space,
	 * hold this body. Changed only under its link record's shard lock. */
	std::uint16_t holders{};
	std::uint8_t instruction_count{};
	/* The guest units the stream is validated against: words for A64 and
	 * A32, halfwords for T32 (at most maximum_t32_validation_units). */
	std::uint8_t source_word_count{};

	/* Where a build stopped, which a stored block keeps only when
	 * something can ask for it: a block that stopped at a word it could
	 * not translate, or that holds no instruction, reports its stop
	 * address as the illegal exit's PC. A complete block's terminal is in
	 * its stream and its validation words, and no caller of a stored
	 * block reads the stop fields of one. */
	struct Stop final {
		std::uint64_t address;
		std::uint32_t instruction;
		std::uint32_t padding;
	};

	/* The stream follows the body, and the guest words it is validated
	 * against follow the stream. A block that accesses FP/SIMD state is
	 * linked to through one fp_link_entry word in front of its stream, so
	 * its link record needs no prefix of its own. */
	[[nodiscard]] std::size_t entry_words() const noexcept
	{
		return accesses_fp_state ? 1U : 0U;
	}
	[[nodiscard]] const std::uintptr_t *link_entry() const noexcept
	{
		return reinterpret_cast<const std::uintptr_t *>(this + 1);
	}
	[[nodiscard]] std::uintptr_t *words() noexcept
	{
		return reinterpret_cast<std::uintptr_t *>(this + 1) +
			entry_words();
	}
	[[nodiscard]] const std::uintptr_t *words() const noexcept
	{
		return link_entry() + entry_words();
	}
	[[nodiscard]] std::uint32_t *source_words() noexcept
	{
		return reinterpret_cast<std::uint32_t *>(words() + word_count);
	}
	[[nodiscard]] const std::uint32_t *source_words() const noexcept
	{
		return reinterpret_cast<const std::uint32_t *>(
			words() + word_count);
	}
	/* The validation units are bytes in a unit the ISA sizes: a word, or
	 * for T32 a halfword. */
	[[nodiscard]] std::size_t source_unit() const noexcept
	{
		return arm64::aarch32::isa_unit_bytes(
			static_cast<arm64::aarch32::GuestIsa>(isa));
	}
	[[nodiscard]] std::byte *source_bytes() noexcept
	{
		return reinterpret_cast<std::byte *>(source_words());
	}
	[[nodiscard]] const std::byte *source_bytes() const noexcept
	{
		return reinterpret_cast<const std::byte *>(source_words());
	}
	[[nodiscard]] std::span<const std::byte> source() const noexcept
	{
		return {source_bytes(),
			std::size_t{source_word_count} * source_unit()};
	}
	/* Whether a build's stop is kept: see Stop. */
	[[nodiscard]] static bool keeps_stop(
		bool complete, std::size_t instruction_count) noexcept
	{
		return !complete || instruction_count == 0;
	}
	/* The offset of the stop tail from the body, at its own alignment
	 * after the validation units. */
	[[nodiscard]] static std::size_t stop_offset(
		std::size_t words, std::size_t source_bytes) noexcept
	{
		return (sizeof(Body) + words * sizeof(std::uintptr_t) +
			source_bytes + alignof(Stop) - 1U) &
			~(alignof(Stop) - 1U);
	}
	[[nodiscard]] Stop *stop() noexcept
	{
		return reinterpret_cast<Stop *>(
			reinterpret_cast<std::byte *>(this) +
			stop_offset(entry_words() + word_count,
				source_word_count * source_unit()));
	}
	[[nodiscard]] const Stop *stop() const noexcept
	{
		return const_cast<Body *>(this)->stop();
	}
	/* What the arena must be given back follows from the counts, so
	 * neither the body nor the slot that indexes it stores it. `words`
	 * counts an FP block's entry word with its stream. */
	[[nodiscard]] static std::size_t allocation_bytes(
		std::size_t words, std::size_t source_bytes,
		bool with_stop) noexcept
	{
		return with_stop ?
			stop_offset(words, source_bytes) + sizeof(Stop) :
			sizeof(Body) + words * sizeof(std::uintptr_t) +
				source_bytes;
	}
	[[nodiscard]] std::size_t allocation_bytes() const noexcept
	{
		return allocation_bytes(entry_words() + word_count,
			source_word_count * source_unit(), has_stop);
	}
	/* The stored block as the builder produced it. The program refers to
	 * the words behind this header rather than copying them, so it is
	 * valid exactly as long as the body is. */
	[[nodiscard]] BuildResult result() const noexcept
	{
		arm64::gadgets::ProgramWords view{
			arm64::gadgets::ProgramWords::Borrowed{
				const_cast<std::uintptr_t *>(words()),
				word_count}};
		const auto *kept = has_stop ? stop() : nullptr;
		return BuildResult{arm64::gadgets::Program{guest_address,
				instruction_count, std::move(view),
				accesses_fp_state},
			complete, kept != nullptr ? kept->address : 0U,
			kept != nullptr ? kept->instruction : 0U};
	}
};
static_assert(sizeof(Body) == 16 &&
	sizeof(Body) % alignof(std::uintptr_t) == 0,
	"the stream tail follows the body at pointer alignment");
static_assert(maximum_t32_validation_units < 0x100U,
	"a T32 translation's halfword count fits the narrow field");
static_assert(block_instruction_count < 0xffU,
	"a stored block's counts fit the body's narrow fields");

/* A link names a guest PC, which is only meaningful inside one address
 * space; keying by PC alone would let a block in one branch straight into
 * another's translation. Each address space therefore owns its own records,
 * which also keeps the key a single word.
 *
 * Translated code follows `link` with one `ldp` of its stream and count while
 * other vCPUs of the address space may retarget it, so the pair sits on a
 * sixteen-byte boundary and is only ever written whole (store_pair). The
 * fallback word stays right in front of it, where link_exit expects its
 * stream, and the alignment's padding holds the body a miss may adopt.
 */
struct alignas(16) LinkRecord final {
	explicit LinkRecord(std::uint64_t pc) noexcept
		: fallback_stream{reinterpret_cast<std::uintptr_t>(
			uml_arm64_gadget_link_exit)},
		  link{
			.stream = &fallback_stream,
			.instruction_count = 0,
			.guest_pc = pc,
		  }
	{
	}

	void reset() noexcept
	{
		store_pair(&link, word_of(&fallback_stream), 0);
	}

	/* Retarget the record at a stored body. Its words were written before
	 * this, and a vCPU that loads the pair reads them through it. */
	void link_to(const Body &body) noexcept
	{
		std::atomic_thread_fence(std::memory_order_release);
		store_pair(&link, word_of(body.link_entry()),
			body.instruction_count);
	}

	[[nodiscard]] bool names(const Body &body) const noexcept
	{
		return link.stream == body.link_entry();
	}

	/* The newest body stored for this PC that some slot still holds, which
	 * another executor's miss may adopt instead of translating. */
	Body *indexed{};
	/* An unlinked record's stream: link_exit, which takes the guest PC from
	 * the BlockLink right behind it. */
	std::uintptr_t fallback_stream;
	arm64::gadgets::BlockLink link;
	/* Set while this record sits in its shard's live list, so a code change
	 * resets the records that hold an activation instead of every record
	 * the translator ever named. */
	bool listed{};
};

static_assert(offsetof(LinkRecord, link) == 16 &&
	offsetof(LinkRecord, fallback_stream) + sizeof(std::uintptr_t) ==
		offsetof(LinkRecord, link) &&
	sizeof(LinkRecord) == 48,
	"link_exit reads the PC two words past its stream word, and the "
	"stream and count are one aligned pair");

struct LinkKey final {
	[[nodiscard]] std::uint64_t operator()(
		const LinkRecord &record) const noexcept
	{
		return record.link.guest_pc;
	}
};

/* An indirect site is named by the branch that owns it, which its own cells do
 * not record. Translated streams point at the site, the first member; its
 * cells are aligned pairs that translated code trains as it runs. */
struct alignas(16) IndirectRecord final {
	explicit IndirectRecord(std::uint64_t source) noexcept
		: source_pc{source}
	{}

	arm64::gadgets::IndirectLink site{};
	std::uint64_t source_pc;
};

static_assert(offsetof(IndirectRecord, site) == 0 &&
	sizeof(IndirectRecord) == 48,
	"indirect cells are aligned pairs");

struct IndirectKey final {
	[[nodiscard]] std::uint64_t operator()(
		const IndirectRecord &record) const noexcept
	{
		return record.source_pc;
	}
};

constexpr std::size_t announcement_capacity = 256;
Announcement announcements[announcement_capacity];
std::mutex announcements_mutex;

[[nodiscard]] constexpr bool inside(std::uint64_t sequence) noexcept
{
	return (sequence & 1U) != 0U;
}

[[nodiscard]] constexpr std::uint64_t next_odd(std::uint64_t sequence) noexcept
{
	return sequence + (inside(sequence) ? 2U : 1U);
}

[[nodiscard]] Announcement *acquire_announcement() noexcept
{
	const std::lock_guard guard{announcements_mutex};
	for (auto &announcement : announcements) {
		if (announcement.used)
			continue;
		announcement.used = true;
		announcement.sequence.store(next_odd(announcement.sequence.load(
			std::memory_order_relaxed)), std::memory_order_relaxed);
		return &announcement;
	}
	return nullptr;
}

/* The executor may now load stream pointers from links: every body
 * unpublished before this fence is out of its reach. */
void announce_entry(Announcement &announcement) noexcept
{
	announcement.sequence.store(next_odd(announcement.sequence.load(
		std::memory_order_relaxed)), std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_seq_cst);
}

/* The executor holds no stream pointer any more. */
void announce_exit(Announcement &announcement) noexcept
{
	const auto sequence =
		announcement.sequence.load(std::memory_order_relaxed);
	if (inside(sequence))
		announcement.sequence.store(sequence + 1U,
			std::memory_order_release);
}

/* DARWIN_UML_GADGET_SHARED_BODIES=1 lets executors running one address
 * space share its translations; unset or 0 keeps every executor's to itself.
 * Private is the default until a shared space bounds the link records and
 * freed arena it accumulates under process churn (G227). Read as each
 * executor is made. */
[[nodiscard]] bool shared_bodies_enabled() noexcept
{
	const char *value = std::getenv("DARWIN_UML_GADGET_SHARED_BODIES");
	return value != nullptr && *value != '\0' &&
		std::strcmp(value, "0") != 0;
}

void release_announcement(Announcement *announcement) noexcept
{
	if (announcement == nullptr)
		return;
	const auto sequence =
		announcement->sequence.load(std::memory_order_relaxed);
	if (inside(sequence))
		announcement->sequence.store(sequence + 1U,
			std::memory_order_release);
	const std::lock_guard guard{announcements_mutex};
	announcement->used = false;
}

/* The executors of one address space whose leaving a reclaimer waits for, as
 * they were when it unpublished the bodies it holds back. */
struct Snapshot final {
	struct Seen final {
		const Announcement *announcement;
		std::uint64_t sequence;
	};
	std::vector<Seen> seen;

	/* Every executor seen inside has left since, apart from `self`, which
	 * holds nothing it could reach through an unpublished body: releases
	 * happen in C++ before it runs translated code again. */
	[[nodiscard]] bool elapsed(const Announcement *self) const noexcept
	{
		for (const auto &entry : seen) {
			if (entry.announcement == self || !inside(entry.sequence))
				continue;
			if (entry.announcement->sequence.load(
				    std::memory_order_acquire) == entry.sequence)
				return false;
		}
		return true;
	}
};

/* What makes two executors' translations interchangeable: the address space,
 * the SVCR mode they were built for and the build options. A key with an
 * owner is private to that cache. */
struct SpaceKey final {
	unsigned long generation{};
	std::uint8_t mode{};
	const void *owner{};
	BuildOptions options{};
};

/* One address space's translations.
 *
 * Its bodies, the link records and indirect sites their streams name, and the
 * target table the indirect gadgets consult all belong to the address space,
 * so every executor running it shares them when the host can retarget a pair
 * atomically. Each executor keeps its own tagged slots, which decide what it
 * holds; a body lives while any slot holds it, so the address space costs the
 * union of what its executors hold rather than the sum. A miss on a body
 * another executor holds adopts it after exact word validation, with no copy
 * or relocation (G161, G166).
 *
 * The records are split eight ways by key, each part behind its own lock, so
 * vCPUs resolving links for different code rarely meet. Lock order: reset,
 * shard, limbo, arena.
 */
class Space final {
public:
	explicit Space(SpaceKey key, std::uint64_t changes)
		: key_{std::move(key)}, gate_{changes}, reset_done_{changes},
		  bodies_{&counted_}
	{
		auto &empty = shard_of(empty_pc);
		empty_link_ = &empty.links.try_emplace(empty_pc, empty_pc)
			.first->link;
	}

	~Space()
	{
		/* No executor holds this address space any more, so nothing can
		 * run a body or follow a record of it. */
		free_bodies(limbo_.waiting);
		free_bodies(limbo_.incoming);
	}

	Space(const Space &) = delete;
	Space &operator=(const Space &) = delete;

	/* The unreachable guest PC an untrained indirect cell names; guest
	 * targets are admitted only word-aligned, so none can collide. */
	static constexpr std::uint64_t empty_pc = ~std::uint64_t{};

	[[nodiscard]] const SpaceKey &key() const noexcept { return key_; }

	void attach(const Announcement *announcement)
	{
		const std::lock_guard guard{attached_mutex_};
		attached_.push_back(announcement);
	}

	/* Whether this was the last executor. */
	[[nodiscard]] bool detach(const Announcement *announcement)
	{
		const std::lock_guard guard{attached_mutex_};
		const auto found = std::find(attached_.begin(), attached_.end(),
			announcement);
		if (found != attached_.end())
			attached_.erase(found);
		return attached_.empty();
	}

	[[nodiscard]] const arm64::gadgets::BlockLink *link(std::uint64_t pc)
	{
		auto &shard = shard_of(pc);
		const std::lock_guard guard{shard.mutex};
		return &shard.links.try_emplace(pc, pc).first->link;
	}

	[[nodiscard]] arm64::gadgets::IndirectLink *indirect_link(
		std::uint64_t source_pc)
	{
		auto &shard = shard_of(source_pc);
		const std::lock_guard guard{shard.mutex};
		const auto [created, inserted] =
			shard.indirect.try_emplace(source_pc, source_pc);
		if (inserted)
			for (auto &entry : created->site.entries)
				store_pair(&entry, empty_pc, word_of(empty_link_));
		return &created->site;
	}

	/* The target table, or null before the address space's first indirect
	 * miss. It lives as long as the records it names. */
	[[nodiscard]] const void *target_tier() const noexcept
	{
		return targets_.load(std::memory_order_acquire);
	}

	/* Train `site` and the target table with `target`, the record for
	 * `target_pc`. Every vCPU of the address space misses into here, so
	 * only a site's first training takes a lock.
	 *
	 * A cell or table entry only ever names a record, and a record is
	 * linked only to a body validated at the gate, so writing one without
	 * the shard lock is safe even while a code change resets the site:
	 * following it at worst exits through link_exit. Enrolment in the live
	 * list only keeps a reset site's predictions clean. */
	void learn_indirect(arm64::gadgets::IndirectLink &site,
		std::uint64_t target_pc, const arm64::gadgets::BlockLink *target)
	{
		train_target(target_pc, target);
		/* Training is the only thing C++ does to move a site off its
		 * empty cells, and empty_pc is unreachable as a guest target, so
		 * a site still holding two empty cells is untrained since its
		 * last reset -- which makes this the transition that enrols it.
		 * Translated code trains cells too, without enrolling; such a
		 * cell names a record, which a code change resets. */
		if (untrained(site)) {
			const auto &owner =
				*reinterpret_cast<const IndirectRecord *>(&site);
			auto &shard = shard_of(owner.source_pc);
			const std::lock_guard guard{shard.mutex};
			if (untrained(site)) {
				shard.live_indirect.push_back(&site);
				store_pair(&site.entries[0], target_pc,
					word_of(target));
				return;
			}
		}
		for (auto &entry : site.entries) {
			if (cell_pc(entry) != target_pc)
				continue;
			store_pair(&entry, target_pc, word_of(target));
			return;
		}
		std::atomic_ref<std::uint8_t> replacement{site.next_replacement};
		const auto next = replacement.load(std::memory_order_relaxed);
		store_pair(&site.entries[next %
				arm64::gadgets::indirect_link_entries],
			target_pc, word_of(target));
		replacement.store(static_cast<std::uint8_t>((next + 1U) %
			arm64::gadgets::indirect_link_entries),
			std::memory_order_relaxed);
	}

	/* Store a freshly built translation, or adopt the equal one another
	 * executor stored first, and hold it for the caller's slot. */
	[[nodiscard]] Body *publish(const BuildResult &built,
		std::span<const std::byte> validation,
		arm64::aarch32::GuestIsa isa)
	{
		const auto pc = built.program.guest_address();
		auto &shard = shard_of(pc);
		{
			const std::lock_guard guard{shard.mutex};
			auto &record = *shard.links.try_emplace(pc, pc).first;
			if (auto *held = equal_indexed(record, validation, isa)) {
				++held->holders;
				return held;
			}
		}
		auto *body = new_body(built, validation, isa);
		{
			const std::lock_guard guard{shard.mutex};
			auto &record = *shard.links.find(pc);
			if (auto *held = equal_indexed(record, validation, isa)) {
				++held->holders;
				/* Never published, so nothing can reach it. */
				const std::lock_guard arena{arena_mutex_};
				delete_body(body);
				return held;
			}
			body->holders = 1;
			if (!validation.empty())
				record.indexed = body;
		}
		return body;
	}

	/* Hold the body indexed for `pc`, which the caller will validate. */
	[[nodiscard]] Body *hold_indexed(std::uint64_t pc)
	{
		auto &shard = shard_of(pc);
		const std::lock_guard guard{shard.mutex};
		auto *record = shard.links.find(pc);
		if (record == nullptr || record->indexed == nullptr)
			return nullptr;
		++record->indexed->holders;
		return record->indexed;
	}

	/* A slot lets go of `body`. `stale` says validation found guest code
	 * other than the body's, so no miss should adopt it any more. */
	void release(Body *body, bool stale)
	{
		auto &shard = shard_of(body->guest_address);
		const std::lock_guard guard{shard.mutex};
		auto *record = shard.links.find(body->guest_address);
		assert(body->holders != 0);
		const bool last = --body->holders == 0;
		if (record != nullptr && record->indexed == body && (last || stale))
			record->indexed = nullptr;
		if (!last)
			return;
		/* Nothing names it from here on; executors that loaded the
		 * record before this may still be running it, so it waits for
		 * them in limbo. */
		if (record != nullptr && record->names(*body))
			record->reset();
		const std::lock_guard limbo{limbo_.mutex};
		limbo_.incoming.push_back(body);
		limbo_.pending.store(true, std::memory_order_relaxed);
	}

	/* Link the record for `pc` to `body`, which the caller holds and
	 * validated at `changes`, unless a newer code change has reset the
	 * address space's links since. Returns the record for the slot's memo. */
	LinkRecord *activate(std::uint64_t pc, LinkRecord *memo,
		const Body &body, std::uint64_t changes)
	{
		auto &shard = shard_of(pc);
		const std::lock_guard guard{shard.mutex};
		auto *record = memo != nullptr && memo->link.guest_pc == pc ?
			memo : shard.links.try_emplace(pc, pc).first;
		if (gate_.load(std::memory_order_seq_cst) != changes)
			return record;
		if (!record->listed) {
			record->listed = true;
			shard.live_links.push_back(record);
		}
		if (!record->names(body))
			record->link_to(body);
		return record;
	}

	/* Answer a code change: before any executor runs the address space at
	 * `changes`, every link and site that held an activation is reset.
	 * Raising the gate first means an activation racing this either lands
	 * before its shard is reset or sees the gate and stays unlinked. */
	void observe(std::uint64_t changes)
	{
		if (reset_done_.load(std::memory_order_acquire) >= changes)
			return;
		const std::lock_guard reset{reset_mutex_};
		if (reset_done_.load(std::memory_order_relaxed) >= changes)
			return;
		gate_.store(changes, std::memory_order_seq_cst);
		for (auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			for (auto *record : shard.live_links) {
				record->reset();
				record->listed = false;
			}
			shard.live_links.clear();
			/* The reset cells name this address space's own empty
			 * record, so a site never points at another's link
			 * state. */
			for (auto *site : shard.live_indirect) {
				for (auto &entry : site->entries)
					store_pair(&entry, empty_pc,
						word_of(empty_link_));
				std::atomic_ref<std::uint8_t>{site->next_replacement}
					.store(0, std::memory_order_relaxed);
			}
			shard.live_indirect.clear();
		}
		reset_done_.store(changes, std::memory_order_release);
	}

	/* Free what was unpublished long enough ago that no other executor can
	 * still be running it. Never waits. */
	void reclaim(const Announcement *self)
	{
		if (!limbo_.pending.load(std::memory_order_relaxed))
			return;
		std::unique_lock limbo{limbo_.mutex, std::try_to_lock};
		if (!limbo.owns_lock())
			return;
		if (!limbo_.waiting.empty()) {
			if (!limbo_.snapshot.elapsed(self))
				return;
			free_bodies(limbo_.waiting);
		}
		if (!limbo_.incoming.empty()) {
			/* Every body here was unpublished before this fence, so
			 * an executor seen outside now can only enter after it
			 * and never reach one. */
			std::atomic_thread_fence(std::memory_order_seq_cst);
			limbo_.waiting.swap(limbo_.incoming);
			take_snapshot(limbo_.snapshot);
			if (limbo_.snapshot.elapsed(self))
				free_bodies(limbo_.waiting);
		}
		limbo_.pending.store(!limbo_.waiting.empty(),
			std::memory_order_relaxed);
	}

	/* The live body whose allocation holds `address`, or null. A body
	 * waiting in limbo is still live here, so an exit inside a successor
	 * that was unpublished while it ran is still attributed to it. */
	[[nodiscard]] const Body *owner(const void *address) const
	{
		const std::lock_guard arena{arena_mutex_};
		return static_cast<const Body *>(bodies_.owner(address));
	}

	/* owner() for the guest sampler, which must not wait on a vCPU it
	 * has suspended. */
	[[nodiscard]] bool try_owner(const void *address,
		const Body *&body) const
	{
		std::unique_lock arena{arena_mutex_, std::try_to_lock};
		if (!arena.owns_lock())
			return false;
		body = static_cast<const Body *>(bodies_.owner(address));
		return true;
	}

	struct Footprint final {
		std::size_t stream_index, streams, links, indirect;
	};

	[[nodiscard]] Footprint footprint() const
	{
		Footprint result{};
		{
			const std::lock_guard arena{arena_mutex_};
			result.stream_index = bodies_.index_bytes();
			result.streams = bodies_.bytes();
		}
		for (const auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			result.links += shard.links.bytes() +
				shard.live_links.capacity() *
					sizeof(LinkRecord *);
			result.indirect += shard.indirect.bytes() +
				shard.live_indirect.capacity() * sizeof(void *);
		}
		if (targets_.load(std::memory_order_relaxed) != nullptr)
			result.indirect += arm64::gadgets::target_tier_entries *
				sizeof(arm64::gadgets::IndirectLinkEntry);
		return result;
	}

#if UML_GADGET_CACHE_CENSUS
	[[nodiscard]] std::size_t census_empty_bytes() const
	{
		const std::lock_guard arena{arena_mutex_};
		return bodies_.empty_bytes();
	}

	[[nodiscard]] std::size_t link_records() const
	{
		std::size_t count = 0;
		for (const auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			count += shard.links.size();
		}
		return count;
	}

	[[nodiscard]] std::size_t attached() const
	{
		const std::lock_guard guard{attached_mutex_};
		return attached_.size();
	}

	[[nodiscard]] std::size_t limbo_bodies() const
	{
		const std::lock_guard limbo{limbo_.mutex};
		return limbo_.waiting.size() + limbo_.incoming.size();
	}

	template <typename Visit>
	void for_each_link(Visit &&visit) const
	{
		for (const auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			shard.links.for_each(visit);
		}
	}

	template <typename Visit>
	void for_each_indirect(Visit &&visit) const
	{
		for (const auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			shard.indirect.for_each(visit);
		}
	}
#endif

private:
	static constexpr std::size_t shard_count = 8;

	struct Shard final {
		mutable std::mutex mutex;
		/* These grow for the address space's whole residency and are
		 * released only with it. The records live in chunks, so each
		 * stays where translated link cells name it. */
		gadget_cache::StableRecordTable<LinkRecord, LinkKey> links;
		gadget_cache::StableRecordTable<IndirectRecord, IndirectKey>
			indirect;
		/* The records and sites holding an activation. */
		std::vector<LinkRecord *> live_links;
		std::vector<arm64::gadgets::IndirectLink *> live_indirect;
	};

	struct Limbo final {
		mutable std::mutex mutex;
		/* Unpublished, not yet covered by a snapshot. */
		std::vector<Body *> incoming;
		/* Unpublished before `snapshot` was taken. */
		std::vector<Body *> waiting;
		Snapshot snapshot;
		std::atomic<bool> pending{};
	};

	[[nodiscard]] Shard &shard_of(std::uint64_t key) noexcept
	{
		auto mixed = key * 0x9e3779b97f4a7c15ULL;
		mixed ^= mixed >> 29;
		return shards_[(mixed >> 7) & (shard_count - 1U)];
	}

	[[nodiscard]] static std::uint64_t cell_pc(
		arm64::gadgets::IndirectLinkEntry &entry) noexcept
	{
		return std::atomic_ref<std::uint64_t>{entry.guest_pc}.load(
			std::memory_order_relaxed);
	}

	[[nodiscard]] static bool untrained(
		arm64::gadgets::IndirectLink &site) noexcept
	{
		for (auto &entry : site.entries)
			if (cell_pc(entry) != empty_pc)
				return false;
		return true;
	}

	/* The indexed body when it stands for exactly these guest units. */
	[[nodiscard]] static Body *equal_indexed(const LinkRecord &record,
		std::span<const std::byte> validation,
		arm64::aarch32::GuestIsa isa) noexcept
	{
		auto *held = record.indexed;
		if (held == nullptr || validation.empty() ||
		    held->isa != static_cast<std::uint8_t>(isa))
			return nullptr;
		const auto source = held->source();
		if (source.size() != validation.size() ||
		    !std::equal(source.begin(), source.end(),
			    validation.begin()))
			return nullptr;
		return held;
	}

	void train_target(std::uint64_t pc,
		const arm64::gadgets::BlockLink *target)
	{
		using arm64::gadgets::target_tier_entries;
		auto *table = targets_.load(std::memory_order_acquire);
		if (table == nullptr) {
			const std::lock_guard guard{targets_mutex_};
			table = targets_.load(std::memory_order_relaxed);
			if (table == nullptr) {
				targets_storage_ = std::make_unique<
					arm64::gadgets::IndirectLinkEntry[]>(
					target_tier_entries);
				table = targets_storage_.get();
				/* Empty entries name the address space's own
				 * unlinked record for empty_pc, as reset site cells
				 * do. */
				for (std::size_t index = 0;
				     index < target_tier_entries; ++index)
					store_pair(&table[index], empty_pc,
						word_of(empty_link_));
				targets_.store(table, std::memory_order_release);
			}
		}
		store_pair(&table[arm64::gadgets::target_tier_index(pc)], pc,
			word_of(target));
	}

	[[nodiscard]] Body *new_body(const BuildResult &built,
		std::span<const std::byte> validation,
		arm64::aarch32::GuestIsa isa)
	{
		const auto &program = built.program;
		const auto words = program.words();
		assert(program.instruction_count() <= block_instruction_count);
		const auto unit = arm64::aarch32::isa_unit_bytes(isa);
		assert(validation.size() % unit == 0);
		const std::size_t entry = program.accesses_fp_state() ? 1U : 0U;
		const bool with_stop = Body::keeps_stop(
			built.complete, program.instruction_count());
		const auto bytes = Body::allocation_bytes(
			entry + words.size(), validation.size(), with_stop);
		assert(words.size() < (std::size_t{1} << 24));
		void *storage = nullptr;
		{
			const std::lock_guard arena{arena_mutex_};
			storage = bodies_.allocate(bytes);
		}
		auto *body = new (storage) Body{};
		body->guest_address = program.guest_address();
		body->word_count = static_cast<std::uint32_t>(words.size());
		body->instruction_count =
			static_cast<std::uint8_t>(program.instruction_count());
		body->source_word_count =
			static_cast<std::uint8_t>(validation.size() / unit);
		body->complete = built.complete;
		body->accesses_fp_state = program.accesses_fp_state();
		body->has_stop = with_stop;
		body->isa = static_cast<std::uint8_t>(isa) & 3U;
		if (with_stop)
			*body->stop() = {built.stop_address,
				built.stop_instruction, 0U};
		if (entry != 0)
			body->words()[-1] = reinterpret_cast<std::uintptr_t>(
				uml_arm64_gadget_fp_link_entry);
		std::copy(words.begin(), words.end(), body->words());
		std::copy(validation.begin(), validation.end(),
			body->source_bytes());
		return body;
	}

	/* The caller holds arena_mutex_. */
	void delete_body(Body *body) noexcept
	{
		const auto bytes = body->allocation_bytes();
		body->~Body();
		bodies_.deallocate(body, bytes);
	}

	void free_bodies(std::vector<Body *> &bodies) noexcept
	{
		if (bodies.empty())
			return;
		const std::lock_guard arena{arena_mutex_};
		for (auto *body : bodies)
			delete_body(body);
		bodies.clear();
	}

	void take_snapshot(Snapshot &snapshot)
	{
		snapshot.seen.clear();
		const std::lock_guard guard{attached_mutex_};
		for (const auto *announcement : attached_)
			if (announcement != nullptr)
				snapshot.seen.push_back({announcement,
					announcement->sequence.load(
						std::memory_order_acquire)});
	}

	SpaceKey key_;
	std::array<Shard, shard_count> shards_;
	const arm64::gadgets::BlockLink *empty_link_{};
	/* The code-change sequence links may be activated at, and the one
	 * whose reset has finished. */
	std::atomic<std::uint64_t> gate_;
	std::atomic<std::uint64_t> reset_done_;
	std::mutex reset_mutex_;
	std::mutex targets_mutex_;
	std::unique_ptr<arm64::gadgets::IndirectLinkEntry[]> targets_storage_;
	std::atomic<arm64::gadgets::IndirectLinkEntry *> targets_{};
	Limbo limbo_;
	mutable std::mutex arena_mutex_;
	CountingResource counted_;
	BodyArena bodies_;
	mutable std::mutex attached_mutex_;
	std::vector<const Announcement *> attached_;
};

/* The address spaces whose translations executors share, by key. */
class SpaceRegistry final {
public:
	[[nodiscard]] static SpaceRegistry &instance() noexcept
	{
		static SpaceRegistry registry;
		return registry;
	}

	/* The space for `key`, made when absent, with `announcement` among
	 * the executors a reclaimer waits for. A key with an owner is never
	 * shared and never registered. */
	[[nodiscard]] Space *attach(SpaceKey key, std::uint64_t changes,
		const Announcement *announcement)
	{
		if (key.owner != nullptr) {
			auto *space = new Space(std::move(key), changes);
			space->attach(announcement);
			return space;
		}
		const std::lock_guard guard{mutex_};
		auto &candidates = spaces_[{key.generation, key.mode}];
		for (auto *space : candidates)
			if (space->key().options == key.options) {
				space->attach(announcement);
				return space;
			}
		auto space = std::make_unique<Space>(key, changes);
		space->attach(announcement);
		candidates.push_back(space.get());
		return space.release();
	}

	void detach(Space *space, const Announcement *announcement)
	{
		if (space->key().owner != nullptr) {
			(void)space->detach(announcement);
			delete space;
			return;
		}
		std::unique_ptr<Space> destroyed;
		{
			const std::lock_guard guard{mutex_};
			if (!space->detach(announcement))
				return;
			const auto found = spaces_.find(
				{space->key().generation, space->key().mode});
			if (found != spaces_.end()) {
				auto &candidates = found->second;
				candidates.erase(std::find(candidates.begin(),
					candidates.end(), space));
				if (candidates.empty())
					spaces_.erase(found);
			}
			destroyed.reset(space);
		}
	}

private:
	struct Key final {
		unsigned long generation;
		std::uint8_t mode;
		bool operator==(const Key &) const noexcept = default;
	};
	struct KeyHash final {
		[[nodiscard]] std::size_t operator()(const Key &key) const noexcept
		{
			return std::hash<unsigned long>{}(key.generation) ^
				(std::size_t{key.mode} << 56);
		}
	};

	std::mutex mutex_;
	std::unordered_map<Key, std::vector<Space *>, KeyHash> spaces_;
};

} // namespace

class GadgetExecutor::ProgramCache final {
	/* Named by member declarations below and defined with the rest of the
	 * private state. */
	struct Generation;

public:
	/* `mode` is the SVCR mode every translation here is built for, and
	 * `announcement` what this cache's executor says about itself; with
	 * `share` false the cache keeps its address spaces to itself. */
	ProgramCache(const BuildOptions &options, std::uint8_t mode,
		const Announcement *announcement, bool share)
		: options_{options}, mode_{mode}, announcement_{announcement},
		  share_{share},
		  build_scratch_{scratch_.data(), scratch_.size()}
	{}

	~ProgramCache()
	{
		for (auto &slab : slabs_) {
			if (!slab)
				continue;
			for (auto &slot : slab->slots) {
				if (slot.body == nullptr)
					continue;
				space_of(slot).release(slot.body, false);
				slot.vacate();
			}
		}
		for (auto &[generation, state] : generations_)
			SpaceRegistry::instance().detach(state.space,
				announcement_);
	}

	ProgramCache(const ProgramCache &) = delete;
	ProgramCache &operator=(const ProgramCache &) = delete;

#if UML_GADGET_CACHE_CENSUS
	/* Write what this cache holds and how it came to hold it, so
	 * duplication, dormancy and lost work can be counted instead of
	 * estimated. `snapshot` numbers a periodic snapshot; zero is the one
	 * written at teardown.
	 *
	 * A translation is identified by the guest words it consumed, not by the
	 * stream it produced: two executors translating the same code emit the
	 * same words but embed pointers to their own link records, so only the
	 * source identifies the work. The parts of its allocation go out with
	 * it because they decide what a body costs. Every address space the
	 * cache holds is written whole, with its identity, so a reader counts a
	 * space that several executors share once, and each translation names
	 * its body so a body several slots hold is counted once.
	 */
	void census(std::FILE *out, std::uint64_t snapshot) const
	{
		/* The executor record counts only what this cache owns: its slabs
		 * and its per-space slot lists. Each address space's bodies and
		 * records follow on an S line named by the space, which every
		 * cache holding it writes and a reader counts once. */
		std::size_t slabs = 0;
		for (const auto &slab : slabs_)
			if (slab)
				slabs += sizeof(Slab);
		std::size_t lists = 0;
		for (const auto &[generation, state] : generations_)
			lists += state.slots.capacity() * sizeof(std::uint32_t);
		if (link_memo_)
			lists += sizeof(LinkMemo);
		/* How often the stream lookup was asked, against how often a
		 * store would have had to maintain an index for it. */
		const auto slab_total = static_cast<std::size_t>(
			std::count_if(slabs_.begin(), slabs_.end(),
				[](const auto &slab) { return slab != nullptr; }));
		std::fprintf(out,
			"E %p generations=%zu occupancy=%zu slots=%zu slot_bytes=%zu "
			"slab_slots=%zu slab_count=%zu stream_index=0 slabs=%zu "
			"streams=0 arena_empty=0 links=%zu indirect=0 "
			"total=%zu stream_lookups=%llu adoptions=%llu shared=%d",
			static_cast<const void *>(this), resident_generations(),
			occupancy(), slot_count, sizeof(Slot), slab_slots,
			slab_total, slabs, lists, slabs + lists,
			static_cast<unsigned long long>(stream_lookups_),
			static_cast<unsigned long long>(adoptions_),
			share_ ? 1 : 0);
		census::write_when(out, snapshot);
		history_.write(out);
		std::fputc('\n', out);
		const auto now = census::elapsed_seconds();
		for (const auto &[generation, state] : generations_) {
			const auto found = std::find(resident_.rbegin(),
				resident_.rend(), generation);
			const bool running = generation == current_generation_;
			const auto space = state.space->footprint();
			/* The G line's bytes are this cache's own list of its
			 * slots in the space; the space's are on its S line. */
			std::fprintf(out,
				"G %lu current=%d rank=%td idle_switches=%llu "
				"idle_seconds=%.3f link_records=%zu links=%zu "
				"indirect=0 targets=0 space=%p\n",
				generation, running ? 1 : 0,
				found - resident_.rbegin(),
				static_cast<unsigned long long>(running ? 0 :
					switches_ - state.census_switch),
				running ? 0.0 : now - state.census_seconds,
				state.space->link_records(),
				state.slots.capacity() * sizeof(std::uint32_t),
				static_cast<const void *>(state.space));
			std::fprintf(out,
				"S %p generation=%lu mode=%u attached=%zu "
				"stream_index=%zu streams=%zu arena_empty=%zu "
				"links=%zu indirect=%zu link_records=%zu limbo=%zu\n",
				static_cast<const void *>(state.space), generation,
				unsigned{mode_}, state.space->attached(),
				space.stream_index, space.streams,
				state.space->census_empty_bytes(), space.links,
				space.indirect, state.space->link_records(),
				state.space->limbo_bodies());
		}
		if (census::totals_only())
			return;
		const auto sample = census::word_sample();
		/* The same link targets are sampled in every cache, so the
		 * sample says how many of an address space's records each
		 * cache keeps for itself. A shared space writes its records
		 * from every cache that holds it, which a reader removes by
		 * the space's identity. */
		if (sample != 0)
			for (const auto &[generation, state] : generations_)
				state.space->for_each_link(
					[&, generation](const LinkRecord &record) {
						if (census_sampled(
							    record.link.guest_pc,
							    sample))
							std::fprintf(out,
								"L %lu 0x%llx %p\n",
								generation,
								static_cast<
									unsigned long long>(
									record.link
										.guest_pc),
								static_cast<
									const void *>(
									state.space));
					});
		const auto text = census::text_of(reinterpret_cast<const void *>(
			reinterpret_cast<std::uintptr_t>(
				uml_arm64_gadget_link_exit)));
		CensusRecords records;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots) {
				if (slot.body == nullptr)
					continue;
				const auto &body = *slot.body;
				std::fprintf(out,
					"P %lu 0x%llx %zu %zu %zu 0x%llx %zu %zu %zu "
					"%zu %p\n",
					generation_of(slot),
					static_cast<unsigned long long>(slot.key()),
					std::size_t{body.instruction_count},
					std::size_t{body.word_count},
					std::size_t{body.source_word_count},
					static_cast<unsigned long long>(
						body_digest(body)),
					body_bytes(body), sizeof(Body),
					(body.entry_words() + body.word_count) *
						sizeof(std::uintptr_t),
					std::size_t{body.source_word_count} *
						body.source_unit(),
					static_cast<const void *>(&body));
				if (sample != 0 &&
				    census_sampled(slot.key(), sample))
					census_words(out, slot, text, records);
			}
		}
	}
#endif

	/* How many address spaces currently hold translations here. With one
	 * shared pool this is an outcome of the workload, not a build option. */
	[[nodiscard]] std::size_t resident_generations() const noexcept
	{
		return generations_.size();
	}

	[[nodiscard]] std::size_t occupancy() const noexcept
	{
		std::size_t occupied = 0;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots)
				if (slot.body != nullptr)
					++occupied;
		}
		return occupied;
	}

	/* Bind the cache to an address space and answer for whatever changed in
	 * it while it was away. Each address space carries its own mark, so a
	 * switch back to one whose code never changed costs nothing. */
	void prepare(unsigned long generation, std::uint64_t changes)
	{
#if UML_GADGET_CACHE_CENSUS
		/* The address space being left was in use until now. */
		if (generation != current_generation_ && current_ != nullptr) {
			++switches_;
			current_->census_switch = switches_;
			current_->census_seconds = census::elapsed_seconds();
		}
#endif
		current_generation_ = generation;
		current_ = nullptr;
		auto &state = current();
		touch(generation);
		if (state.changes == changes)
			return;
		state.space->observe(changes);
		state.changes = changes;
		advance_epoch(state);
	}

	[[nodiscard]] std::optional<BuildResult> find(
		std::uint64_t pc, std::uint64_t changes,
		std::uint64_t mapping_changes, StaticGuestMemory &memory,
		void *execution_context, GadgetCacheDiagnostics &diagnostics)
	{
#if UML_GADGET_CACHE_CENSUS
		if ((history_.lookups() & 4095U) == 0)
			snapshot_if_due();
#endif
		assert(current_ == nullptr || current_->changes == changes);
		const auto location = locate(current_generation_, pc);
		const auto tag = tag_of(pc);
		auto &slab = slabs_[location.slab];
		if (slab) {
			for (std::size_t way = 0; way < ways; ++way) {
				auto &slot = slab->slots[location.offset + way];
				if (slot.tag != tag)
					continue;
				if (slot.epoch() == current_epoch_) {
#if UML_GADGET_CACHE_CENSUS
					history_.hit();
#endif
					return slot.body->result();
				}
				++diagnostics.validation_attempts;
				if (!validate(slot, changes, mapping_changes,
					    memory, execution_context,
					    diagnostics)) {
#if UML_GADGET_CACHE_CENSUS
					history_.missed();
#endif
					return std::nullopt;
				}
				activate(pc, slot);
#if UML_GADGET_CACHE_CENSUS
				history_.revalidated();
#endif
				return slot.body->result();
			}
		}
		/* No slot here holds the PC, so ask the address space: another
		 * executor may hold its translation. */
		if (share_) {
			if (auto adopted = adopt(pc, changes, mapping_changes,
				    memory, execution_context, diagnostics)) {
#if UML_GADGET_CACHE_CENSUS
				history_.adopted();
#endif
				return adopted;
			}
		}
#if UML_GADGET_CACHE_CENSUS
		history_.missed();
#endif
		return std::nullopt;
	}

	/* The translation whose stream holds an address.
	 *
	 * A terminal exit inside a linked successor reports the successor's
	 * stream, and the retirement correction needs that successor's program
	 * rather than the entered block's. The arena knows which live body
	 * holds any address, and a stream pointer the engine has just produced
	 * always lies in a live body: links only ever name translations a slot
	 * held when they were linked, and an unpublished body is freed only
	 * after this executor has left translated code. The answer is exact,
	 * with no index to fall out of.
	 */
	[[nodiscard]] std::optional<BuildResult> find_stream(
		const void *stream) const
	{
		if (stream == nullptr || current_ == nullptr)
			return std::nullopt;
#if UML_GADGET_CACHE_CENSUS
		++stream_lookups_;
#endif
		const auto *body = current_->space->owner(stream);
		if (body == nullptr)
			return std::nullopt;
		auto result = body->result();
		if (!program_contains_stream(result.program, stream))
			return std::nullopt;
		return result;
	}

	/* find_stream() with the guest units the translation was validated
	 * against, for an AArch32 exit: a T32 block's instructions are found
	 * in them. */
	struct StreamSource final {
		BuildResult result;
		std::span<const std::byte> source;
	};

	[[nodiscard]] std::optional<StreamSource> find_stream_source(
		const void *stream) const
	{
		if (stream == nullptr || current_ == nullptr)
			return std::nullopt;
		const auto *body = current_->space->owner(stream);
		if (body == nullptr)
			return std::nullopt;
		auto result = body->result();
		if (!program_contains_stream(result.program, stream))
			return std::nullopt;
		return StreamSource{std::move(result), body->source()};
	}

	/* The same lookup for the guest sampler, which runs while this
	 * executor's thread is suspended in gadget code: reads only, and never
	 * waits for a lock another vCPU may hold. */
	[[nodiscard]] bool sample_block(const void *stream,
		diagnostics::SampledBlock &block) const noexcept
	{
		for (const auto &[generation, state] : generations_) {
			const Body *body = nullptr;
			try {
				if (!state.space->try_owner(stream, body) ||
				    body == nullptr)
					continue;
			} catch (...) {
				return false;
			}
			const auto *word =
				static_cast<const std::uintptr_t *>(stream);
			if (word < body->link_entry() ||
			    word >= body->words() + body->word_count)
				return false;
			block.guest_address = body->guest_address;
			block.instruction_count = body->instruction_count;
			if (body->isa == 0) {
				block.word_count = std::min<std::uint32_t>(
					body->source_word_count,
					static_cast<std::uint32_t>(
						diagnostics::sampled_block_words));
				std::copy_n(body->source_words(), block.word_count,
					block.words);
				return true;
			}
			/* An AArch32 block's address is its key, and its words
			 * are the guest bytes it was translated from, a T32
			 * block's halfwords in pairs (little-endian words, as
			 * the guest stores them). */
			const auto bytes = std::min<std::size_t>(
				std::size_t{body->source_word_count} *
					body->source_unit(),
				diagnostics::sampled_block_words *
					sizeof(std::uint32_t));
			block.word_count = static_cast<std::uint32_t>(
				(bytes + sizeof(std::uint32_t) - 1) /
				sizeof(std::uint32_t));
			std::fill_n(block.words, block.word_count, 0U);
			std::memcpy(block.words, body->source_bytes(), bytes);
			return true;
		}
		return false;
	}

	[[nodiscard]] const arm64::gadgets::BlockLink *link(std::uint64_t pc)
	{
		return space().link(pc);
	}

	/* The running address space's target table, or null before its first
	 * indirect miss. It lives and dies with the address space, as the link
	 * records it names do, so it can never name another address space's
	 * code; a record reset by a code change just exits through link_exit. */
	[[nodiscard]] const void *target_tier() const noexcept
	{
		return current_ != nullptr ? current_->space->target_tier() :
			nullptr;
	}

	[[nodiscard]] arm64::gadgets::IndirectLink *indirect_link(
		std::uint64_t source_pc)
	{
		return space().indirect_link(source_pc);
	}

	/* The record for `pc` in the current address space, through this
	 * cache's memo of the records its indirect misses named. A record never
	 * moves or dies while its address space is resident here, and retiring
	 * one clears its entries, so a hit is always the live record; it spares
	 * the space's lock, which every vCPU's misses would otherwise meet. */
	[[nodiscard]] const arm64::gadgets::BlockLink *link_record(
		std::uint64_t pc)
	{
		if (!link_memo_)
			link_memo_ = std::make_unique<LinkMemo>();
		auto &entry = (*link_memo_)[((pc >> 2U) ^ (pc >> 14U) ^
			current_generation_) & (link_memo_entries - 1U)];
		if (entry.link == nullptr || entry.pc != pc ||
		    entry.generation != current_generation_)
			entry = {pc, current_generation_,
				current_->space->link(pc)};
		return entry.link;
	}

	void learn_indirect(arm64::gadgets::IndirectLink &site,
			    std::uint64_t target_pc)
	{
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* Distinct targets seen per site, and misses per site, so the
		 * dump can say how many cells a site would have needed. */
		++site_misses_[&site][target_pc];
#endif
		space().learn_indirect(site, target_pc, link_record(target_pc));
	}

	[[nodiscard]] BuildResult store(
		std::uint64_t pc, const BuildResult &result,
		std::span<const std::uint32_t> guest_words,
		[[maybe_unused]] std::uint64_t changes)
	{
		/* The words a validating find() will compare against guest
		 * memory: the program's instructions, plus the word a stopped
		 * build read without retiring. */
		std::span<const std::uint32_t> validation;
		const auto validation_words =
			result.program.instruction_count() +
			static_cast<std::size_t>(!result.complete);
		if (validation_words != 0 &&
		    validation_words <= block_instruction_count &&
		    validation_words <= guest_words.size())
			validation = guest_words.first(validation_words);
		return store_validated(pc, result,
			std::as_bytes(validation), changes);
	}

	/* store() for any ISA: `validation` is the guest bytes the program
	 * stands for, in the units the key's ISA reads (aarch32::GuestIsa), so a
	 * T32 builder supplies halfwords and everything else supplies words. */
	[[nodiscard]] BuildResult store_validated(
		std::uint64_t pc, const BuildResult &result,
		std::span<const std::byte> validation,
		[[maybe_unused]] std::uint64_t changes)
	{
		assert(arm64::aarch32::key_is_canonical(pc));
		/* The running address space is registered before the tag that
		 * names it is made. */
		auto &state = current();
		/* The stored translation is as current as the address space's
		 * code is, which prepare() has been told. */
		assert(state.changes == changes);
		[[maybe_unused]] const auto tag = tag_of(pc);
		/* Nothing is stored under a key with no compact form: such a
		 * key names no fetchable code. */
		assert(tag != unmatchable_tag);
		auto *body = state.space->publish(result, validation,
			arm64::aarch32::key_isa(pc));
#if UML_GADGET_CACHE_CENSUS
		/* A lookup that lost a validation race leaves its translation
		 * resident, and this store makes a second copy beside it. */
		bool restored = false;
		{
			const auto location = locate(current_generation_, pc);
			if (const auto &slab = slabs_[location.slab])
				for (std::size_t way = 0; way < ways; ++way)
					if (slab->slots[location.offset + way]
						    .tag == tag)
						restored = true;
		}
#endif
		auto &slot = install(pc, body, changes);
#if UML_GADGET_CACHE_CENSUS
		history_.arrive(census_id_, current_generation_, pc,
			body_digest(*slot.body), restored);
#endif
		state.space->reclaim(announcement_);
		return slot.body->result();
	}

	/* Where a build assembles its stream.
	 *
	 * store() copies the finished words into the body it allocates, so the
	 * scratch is rewound at the start of every build and a translation
	 * costs no allocation until it is stored. Only one build is ever in
	 * flight on an executor.
	 */
	[[nodiscard]] std::pmr::memory_resource *program_storage() noexcept
	{
		build_scratch_.release();
		return &build_scratch_;
	}

	[[nodiscard]] std::uint64_t adoptions() const noexcept
	{
		return adoptions_;
	}

private:
	static constexpr std::size_t ways = ::uml::cpu::gadget_cache::ways;
	static constexpr std::size_t slot_count = uml_gadget_cache_slots;
	static constexpr std::size_t set_count = slot_count / ways;
	static constexpr std::size_t slab_slots = ::uml::cpu::gadget_cache::slab_slots;
	static constexpr std::size_t slab_count = slot_count / slab_slots;
	/* The ids tags can carry, and the owner bits that match none of them
	 * while no address space is running. */
	static constexpr std::size_t max_owner_id = 0xfffeU;
	static constexpr std::uint64_t no_owner = std::uint64_t{0xffffU}
		<< tag_owner_shift;
	static constexpr std::uint16_t max_epoch = 0xffffU;
	static_assert(uml_gadget_resident_generations + 2U < max_owner_id,
		"every resident address space has an id, and so does the one "
		"being retired");

	/* One way of a set: what a lookup compares and the translation it
	 * names.
	 *
	 * The cache commits a slot per way whether or not it is occupied, so
	 * the tag size multiplies by slot_count: at 262,144 slots every eight
	 * bytes here is two mebibytes per executor. A slot therefore holds
	 * three words, and anything that belongs to one translation belongs
	 * in its Body.
	 */
	struct Slot final {
		/* The translation's key (compact_tag_key) under the id of the
		 * address space it belongs to. A guest virtual address only
		 * means something inside one, so a slot is a match only when
		 * both agree, which is what lets translations for several
		 * address spaces share one cache instead of being wiped at
		 * every switch, and what makes a lookup one comparison. Zero
		 * while the slot is empty: ids start at one, so no occupied
		 * slot's tag is zero and no lookup's is, either.
		 */
		std::uint64_t tag{};
		/* The translation this slot indexes, held in its address space.
		 * A slot is occupied exactly while this is non-null. */
		Body *body{};
		/* Two fields sharing a word, a pointer below the guest's
		 * 48-bit address-space limit and sixteen bits above it:
		 *
		 * The link record reactivation resolves every time, so its
		 * address is memoised here rather than hashed again on each
		 * pass. The address space keeps its records in chunks that
		 * never move, so the address stays valid while the slot holds
		 * this translation: install() and release() reset the memo,
		 * and retiring the address space releases this slot before the
		 * cache lets go of the space.
		 *
		 * The memo is only ever valid for the address space the tag
		 * names, and nothing about the record itself says which one
		 * that is: the guest PC it carries is equal across all of
		 * them. Ordinary slot replacement does not keep the owner, so
		 * install() resets the memo rather than letting activate()
		 * decide from the record alone.
		 *
		 * The epoch of the owner's code (Generation::epoch) when the
		 * translation was last known to match guest memory, zero for
		 * never. It is equal to the owner's current one exactly when no
		 * code change has been seen since.
		 */
		std::uint64_t memo{};

		static constexpr unsigned epoch_shift = 48;
		static constexpr std::uint64_t link_mask =
			(std::uint64_t{1} << epoch_shift) - 1U;

		[[nodiscard]] LinkRecord *link() const noexcept
		{
			return reinterpret_cast<LinkRecord *>(
				static_cast<std::uintptr_t>(memo & link_mask));
		}
		void set_link(LinkRecord *record) noexcept
		{
			const auto address =
				reinterpret_cast<std::uintptr_t>(record);
			assert((address & ~link_mask) == 0U);
			memo = (memo & ~link_mask) | address;
		}
		[[nodiscard]] std::uint16_t epoch() const noexcept
		{
			return static_cast<std::uint16_t>(memo >> epoch_shift);
		}
		void set_epoch(std::uint16_t value) noexcept
		{
			memo = (memo & link_mask) |
				(std::uint64_t{value} << epoch_shift);
		}
		[[nodiscard]] std::uint16_t owner() const noexcept
		{
			return static_cast<std::uint16_t>(tag >> tag_owner_shift);
		}
		[[nodiscard]] std::uint64_t key() const noexcept
		{
			return expand_tag_key(tag);
		}
		/* Empty the slot. The body is the caller's to release. */
		void vacate() noexcept
		{
			tag = 0;
			body = nullptr;
			memo = 0;
		}
	};
	static_assert(sizeof(Slot) == 24,
		"cache slots index translations; they do not hold them");

	struct Slab final {
		std::array<Slot, slab_slots> slots;
	};

	struct Location final {
		std::size_t set;
		std::size_t slab;
		std::size_t offset;
	};

	/* What this cache keeps about one address space it holds: the space,
	 * shared with other executors or not, the code-change mark it last
	 * answered, and the slots it stored into.
	 *
	 * Retiring an address space is then releasing those slots and the
	 * space, rather than filtering it out of structures the others share.
	 * That matters because retirement is not rare on a desktop guest: it
	 * happens on every first sight of a ninth address space. See G167.
	 */
	static constexpr std::size_t link_memo_entries = 4096;
	struct LinkMemoEntry final {
		std::uint64_t pc;
		unsigned long generation;
		const arm64::gadgets::BlockLink *link;
	};
	using LinkMemo = std::array<LinkMemoEntry, link_memo_entries>;

	struct Generation final {
		Generation() = default;
		Generation(const Generation &) = delete;
		Generation &operator=(const Generation &) = delete;
		Generation(Generation &&) = delete;
		Generation &operator=(Generation &&) = delete;

		Space *space{};
		std::uint64_t changes{};
		/* The guest's number for the address space, and the cache's own
		 * for it: the id a slot's tag carries, from one up, recycled
		 * once the address space is retired and its slots released. */
		unsigned long value{};
		std::uint16_t id{};
		/* How many times the address space's code is known to have
		 * changed, as far as a slot's validation can tell: a slot whose
		 * epoch equals this one has matched guest memory since the
		 * last change. It runs from one to its maximum and starts over
		 * after a sweep of the slots that carry it, so a slot's epoch
		 * is never mistaken for the current one. */
		std::uint16_t epoch{1};
		/* Slots this address space has stored into, as flat slot
		 * indices. It may name a slot another address space has since
		 * taken, and may name one twice, so retirement checks every
		 * entry; it can only *miss* a slot by having overflowed, which
		 * `slots_complete` records and retirement answers with the whole
		 * sweep.
		 */
		std::vector<std::uint32_t> slots;
		bool slots_complete{true};
#if UML_GADGET_CACHE_CENSUS
		/* When the cache last left this address space, in switches and
		 * in seconds. */
		std::uint64_t census_switch{};
		double census_seconds{};
#endif
	};

	[[nodiscard]] static Location locate(
		unsigned long generation, std::uint64_t pc) noexcept
	{
		/* Fold the address space into the index so the same guest PC in
		 * two address spaces lands in different sets rather than
		 * competing for the ways of one.
		 */
		const auto block = (pc >> 2) ^
			(static_cast<std::uint64_t>(generation) *
				0x9e3779b97f4a7c15ULL);
		const auto mixed = block ^ (block >> 12) ^ (block >> 24);
		const auto set = static_cast<std::size_t>(mixed) & (set_count - 1);
		const auto first_slot = set * ways;
		return {
			.set = set,
			.slab = first_slot / slab_slots,
			.offset = first_slot % slab_slots,
		};
	}

	/* Least-recently-current first. */
	void touch(unsigned long generation)
	{
		const auto found = std::find(
			resident_.begin(), resident_.end(), generation);
		if (found != resident_.end())
			resident_.erase(found);
		resident_.push_back(generation);
	}

	/* Let go of one address space: its resident translations, and the
	 * cache's hold on the space, which goes with the last executor that
	 * held it.
	 *
	 * The slots come from the list the address space kept while it was
	 * storing, so this is proportional to what it owned rather than to the
	 * size of the cache.
	 */
	void retire(unsigned long generation)
	{
		const auto owner = generations_.find(generation);
		if (owner != generations_.end()) {
			const auto id = owner->second.id;
			if (owner->second.slots_complete) {
				for (const auto index : owner->second.slots)
					release(slot_at(index), id);
			} else {
				for (auto &slab : slabs_) {
					if (!slab)
						continue;
					for (auto &slot : slab->slots)
						release(slot, id);
				}
			}
		} else {
			assert(false && "a resident address space is registered");
		}
		if (link_memo_)
			for (auto &entry : *link_memo_)
				if (entry.generation == generation)
					entry = {};
		if (owner != generations_.end()) {
			auto *space = owner->second.space;
			const auto id = owner->second.id;
			if (current_ == &owner->second)
				unbind();
			owners_[id] = nullptr;
			free_ids_.push_back(id);
			generations_.erase(owner);
			SpaceRegistry::instance().detach(space, announcement_);
		}
		const auto found = std::find(
			resident_.begin(), resident_.end(), generation);
		if (found != resident_.end())
			resident_.erase(found);
	}

	void release(Slot &slot, std::uint16_t owner)
	{
		if (slot.body == nullptr || slot.owner() != owner)
			return;
#if UML_GADGET_CACHE_CENSUS
		history_.depart(generation_of(slot), slot.key(),
			body_digest(*slot.body), body_bytes(*slot.body),
			census::Departure::retired);
#endif
		space_of(slot).release(slot.body, false);
		slot.vacate();
	}

	/* The address space a slot's translation belongs to. */
	[[nodiscard]] Space &space_of(const Slot &slot) const noexcept
	{
		return *owner_of(slot)->space;
	}

	/* The running address space's translations. */
	[[nodiscard]] Space &space() { return *current().space; }

#if UML_GADGET_CACHE_CENSUS
	/* The census names a translation by a digest of its validation
	 * units. */
	[[nodiscard]] static std::uint64_t body_digest(const Body &body) noexcept
	{
		auto digest = census::digest_seed;
		const auto *bytes = body.source_bytes();
		const auto unit = body.source_unit();
		for (std::size_t index = 0; index < body.source_word_count;
		     ++index) {
			std::uint32_t value = 0;
			std::memcpy(&value, bytes + index * unit, unit);
			digest = census::digest_add(digest, value);
		}
		return digest;
	}

	/* What the arena holds for a body: its allocation, in the arena's
	 * sixteen-byte classes. */
	[[nodiscard]] static std::size_t body_bytes(const Body &body) noexcept
	{
		constexpr std::size_t granule = 16;
		return (std::max(body.allocation_bytes(), granule) + granule - 1) &
			~(granule - 1);
	}

	/* What each record a stream can point at names, by the record's
	 * address, for each address space: a link record by the guest PC it
	 * links to, an indirect site by the branch that owns it. */
	using CensusRecords = std::unordered_map<unsigned long,
		std::unordered_map<std::uintptr_t, std::pair<char, std::uint64_t>>>;

	[[nodiscard]] static bool census_sampled(
		std::uint64_t pc, std::uint64_t one_in) noexcept
	{
		auto value = pc * 0x9e3779b97f4a7c15ULL;
		value ^= value >> 29;
		return value % one_in == 0;
	}

	/* One sampled body's stream, entry word included, word by word:
	 * `g` a gadget by its offset in the executable's text, `l` a link
	 * record by the guest PC it links to, `i` an indirect site by the
	 * branch that owns it, and `o` any other word as it is. */
	void census_words(std::FILE *out, const Slot &slot,
		census::TextRange text, CensusRecords &records) const
	{
		auto [named, inserted] = records.try_emplace(generation_of(slot));
		if (inserted) {
			const auto *owner = owner_of(slot);
			if (owner != nullptr) {
				auto &table = named->second;
				owner->space->for_each_link(
					[&table](const LinkRecord &record) {
						table[reinterpret_cast<std::uintptr_t>(
							&record.link)] =
							{'l', record.link.guest_pc};
					});
				owner->space->for_each_indirect(
					[&table](const IndirectRecord &record) {
						table[reinterpret_cast<std::uintptr_t>(
							&record.site)] =
							{'i', record.source_pc};
					});
			}
		}
		const auto &body = *slot.body;
		std::fprintf(out, "W 0x%llx",
			static_cast<unsigned long long>(slot.key()));
		const auto *end = body.words() + body.word_count;
		for (const auto *word = body.link_entry(); word != end; ++word) {
			const auto value = *word;
			if (text.contains(value)) {
				std::fprintf(out, " g%llx",
					static_cast<unsigned long long>(
						value - text.begin));
			} else if (const auto found = named->second.find(value);
				   found != named->second.end()) {
				std::fprintf(out, " %c%llx", found->second.first,
					static_cast<unsigned long long>(
						found->second.second));
			} else {
				std::fprintf(out, " o%llx",
					static_cast<unsigned long long>(value));
			}
		}
		std::fputc('\n', out);
	}

	void snapshot_if_due()
	{
		if (const auto number = schedule_.due(); number != 0)
			census::append([this, number](std::FILE *out) {
				census(out, number);
			});
	}
#endif

	[[nodiscard]] Slot &slot_at(std::uint32_t index) noexcept
	{
		auto &slab = slabs_[index / slab_slots];
		return slab->slots[index % slab_slots];
	}

	/* The address space translation is currently running in, bound to its
	 * space -- made, or shared with the other executors running it -- on
	 * first sight, when the least recently current one beyond the resident
	 * bound is retired.
	 *
	 * prepare() sets this on every switch, but its caller may skip the call
	 * when nothing changed, and on the very first entry there is nothing to
	 * compare against -- so the binding is completed here rather than
	 * assumed. */
	[[nodiscard]] Generation &current()
	{
		if (current_ != nullptr)
			return *current_;
		auto [entry, inserted] =
			generations_.try_emplace(current_generation_);
		current_ = &entry->second;
		if (inserted) {
			auto &state = entry->second;
			enroll(state, current_generation_);
			state.space = SpaceRegistry::instance().attach(
				SpaceKey{
					.generation = current_generation_,
					.mode = mode_,
					.owner = share_ ? nullptr : this,
					.options = options_,
				},
				state.changes, announcement_);
			touch(current_generation_);
			while (resident_.size() > uml_gadget_resident_generations)
				retire(resident_.front());
		}
		bind(*current_);
		return *current_;
	}

	/* Give an address space the id its slots' tags will carry. */
	void enroll(Generation &state, unsigned long generation)
	{
		state.value = generation;
		if (!free_ids_.empty()) {
			state.id = free_ids_.back();
			free_ids_.pop_back();
		} else {
			assert(owners_.size() <= max_owner_id);
			state.id = static_cast<std::uint16_t>(owners_.size());
			owners_.push_back(nullptr);
		}
		owners_[state.id] = &state;
	}

	/* Make an enrolled address space the one lookups are made in. */
	void bind(const Generation &state) noexcept
	{
		current_owner_ = std::uint64_t{state.id} << tag_owner_shift;
		current_epoch_ = state.epoch;
	}

	/* No address space is running: the owner bits name none, so no
	 * lookup matches. */
	void unbind() noexcept
	{
		current_ = nullptr;
		current_owner_ = no_owner;
		current_epoch_ = 0;
	}

	/* The tag a lookup in the running address space compares. */
	[[nodiscard]] std::uint64_t tag_of(std::uint64_t key) const noexcept
	{
		const auto compact = compact_tag_key(key);
		return compact == unmatchable_tag ? unmatchable_tag :
			compact | current_owner_;
	}

	[[nodiscard]] Generation *owner_of(const Slot &slot) const noexcept
	{
		return owners_[slot.owner()];
	}

	[[nodiscard]] unsigned long generation_of(
		const Slot &slot) const noexcept
	{
		return owners_[slot.owner()]->value;
	}

	/* A code change was seen in the running address space: every slot's
	 * validation is out of date. The epoch advances, and when it would
	 * pass what a slot can hold the slots that carry one are cleared, so
	 * that no old validation is ever equal to a new epoch. */
	void advance_epoch(Generation &state) noexcept
	{
		if (state.epoch == max_epoch) {
			const auto id = state.id;
			const auto forget = [id](Slot &slot) noexcept {
				if (slot.body != nullptr && slot.owner() == id)
					slot.set_epoch(0);
			};
			if (state.slots_complete) {
				for (const auto index : state.slots)
					forget(slot_at(index));
			} else {
				for (auto &slab : slabs_)
					if (slab)
						for (auto &slot : slab->slots)
							forget(slot);
			}
			state.epoch = 0;
		}
		++state.epoch;
		if (&state == current_)
			current_epoch_ = state.epoch;
	}

	/* Put a held body in the set for `pc`, letting go of whatever the way
	 * it takes held, and link it. */
	Slot &install(std::uint64_t pc, Body *body,
		[[maybe_unused]] std::uint64_t changes)
	{
		assert(current().changes == changes);
		const auto location = locate(current_generation_, pc);
		auto &slab = slabs_[location.slab];
		if (!slab)
			slab = std::make_unique<Slab>();
		auto &replacement = next_replacement_[location.set];
		std::size_t selected = replacement;
		for (std::size_t way = 0; way < ways; ++way) {
			const auto &slot = slab->slots[location.offset + way];
			if (slot.body == nullptr) {
				selected = way;
				break;
			}
		}
		replacement = static_cast<std::uint8_t>((selected + 1) & (ways - 1));

		auto &slot = slab->slots[location.offset + selected];
		/* Record the slot against the address space taking it, so its
		 * retirement walks what it owns instead of the whole cache. */
		auto &state = current();
		if (state.slots_complete) {
			if (state.slots.size() < slot_count) {
				state.slots.push_back(static_cast<std::uint32_t>(
					location.slab * slab_slots +
					location.offset + selected));
			} else {
				state.slots_complete = false;
				state.slots.clear();
				state.slots.shrink_to_fit();
			}
		}
		/* Whatever occupied the slot lets go of its body, whose address
		 * space may not be the running one. */
		if (slot.body != nullptr) {
#if UML_GADGET_CACHE_CENSUS
			history_.depart(generation_of(slot), slot.key(),
				body_digest(*slot.body), body_bytes(*slot.body),
				census::Departure::evicted);
#endif
			space_of(slot).release(slot.body, false);
			slot.vacate();
		}
		/* The memoised record describes whoever held the slot until now,
		 * and a store may hand the slot to a different address space at
		 * the same guest PC. Left in place it would publish this
		 * translation into the *previous* address space's record, so
		 * taking a slot always resolves the record again; ordinary
		 * reactivation of an unchanged owner keeps the memo.
		 */
		slot.tag = tag_of(pc);
		slot.body = body;
		slot.memo = 0;
		slot.set_epoch(current_epoch_);
		activate(pc, slot);
		return slot;
	}

	/* Take the translation another executor of the address space holds for
	 * `pc` instead of building it: validated against guest memory exactly
	 * as a stale slot is, then held and linked like a fresh store. */
	[[nodiscard]] std::optional<BuildResult> adopt(std::uint64_t pc,
		std::uint64_t changes, std::uint64_t mapping_changes,
		StaticGuestMemory &memory, void *execution_context,
		GadgetCacheDiagnostics &diagnostics)
	{
		auto &space = this->space();
		auto *body = space.hold_indexed(pc);
		if (body == nullptr)
			return std::nullopt;
		const auto verdict = validate_body(*body, pc, changes,
			mapping_changes, memory, execution_context);
		if (verdict != Validation::valid) {
			space.release(body, verdict == Validation::stale);
			return std::nullopt;
		}
		++adoptions_;
		++diagnostics.adoptions;
		auto &slot = install(pc, body, changes);
		space.reclaim(announcement_);
		return slot.body->result();
	}

	void discard(Slot &slot)
	{
		if (slot.body != nullptr) {
#if UML_GADGET_CACHE_CENSUS
			history_.depart(generation_of(slot), slot.key(),
				body_digest(*slot.body), body_bytes(*slot.body),
				census::Departure::discarded);
#endif
			auto &space = space_of(slot);
			space.release(slot.body, true);
			slot.vacate();
			space.reclaim(announcement_);
		}
	}

	enum class Validation : std::uint8_t { valid, stale, race };

	/* Compare a body's guest units with guest memory: words, or for T32
	 * halfwords, read where the key says they lie, and only while the
	 * address space's sequences stay where they were. */
	[[nodiscard]] Validation validate_body(const Body &body,
		std::uint64_t pc, std::uint64_t changes,
		std::uint64_t mapping_changes, StaticGuestMemory &memory,
		void *execution_context, GadgetCacheDiagnostics *diagnostics =
			nullptr)
	{
		const auto units = std::size_t{body.source_word_count};
		const auto limit = body.isa == 0 ? block_instruction_count :
			maximum_t32_validation_units;
		if (units == 0 || units > limit) {
			if (diagnostics != nullptr)
				++diagnostics->validation_mismatches;
			return Validation::stale;
		}

		/* The saved units are the block's instructions and, for a build
		 * that stopped, the one it read without retiring. */
		std::array<std::uint32_t, block_instruction_count + 1> fetched;
		const auto outcome = arm64::aarch32::validate_guest_units(
			pc, units, body.source_bytes(),
			std::as_writable_bytes(std::span{fetched}),
			guest_page_size,
			[&memory](std::uint64_t address,
				  std::span<std::byte> bytes) {
				return memory.fetch(address, bytes);
			},
			false);
		if (outcome == arm64::aarch32::GuestValidation::fault) {
			if (diagnostics != nullptr)
				++diagnostics->validation_faults;
			return Validation::stale;
		}
		if (outcome == arm64::aarch32::GuestValidation::mismatch) {
			if (diagnostics != nullptr)
				++diagnostics->validation_mismatches;
			return Validation::stale;
		}

		const auto current =
			darwin_uml_static_vm_execution_context_sequences(
				execution_context);
		if (current.execution_interrupt != 0 ||
		    current.mapping_changes != mapping_changes ||
		    current.code_changes != changes) {
			if (diagnostics != nullptr)
				++diagnostics->validation_sequence_races;
			return Validation::race;
		}
		return Validation::valid;
	}

	[[nodiscard]] bool validate(
		Slot &slot, std::uint64_t changes, std::uint64_t mapping_changes,
		StaticGuestMemory &memory, void *execution_context,
		GadgetCacheDiagnostics &diagnostics)
	{
		switch (validate_body(*slot.body, slot.key(), changes,
			mapping_changes, memory, execution_context,
			&diagnostics)) {
		case Validation::stale:
			discard(slot);
			return false;
		case Validation::race:
			return false;
		case Validation::valid:
			break;
		}
		slot.set_epoch(current_epoch_);
		++diagnostics.validation_hits;
		return true;
	}

	void activate(std::uint64_t pc, Slot &slot)
	{
		const auto &body = *slot.body;
		if (!body.complete || body.instruction_count == 0)
			return;
		slot.set_link(space().activate(pc, slot.link(), body,
			current().changes));
	}

#if UML_GADGET_EXECUTION_HISTOGRAM
public:
	std::unordered_map<const arm64::gadgets::IndirectLink *,
		std::unordered_map<std::uint64_t, std::uint64_t>> site_misses_;
	[[nodiscard]] std::array<std::uint64_t, 8> miss_distribution() const
	{
		/* misses at sites with 1, 2, 3, 4, 5-8, 9-16, 17-64, >64 targets */
		std::array<std::uint64_t, 8> out{};
		for (const auto &[site, targets] : site_misses_) {
			std::uint64_t misses = 0;
			for (const auto &[pc, count] : targets)
				misses += count;
			const auto n = targets.size();
			const std::size_t bucket = n <= 4 ? n - 1 : n <= 8 ? 4 :
				n <= 16 ? 5 : n <= 64 ? 6 : 7;
			out[bucket] += misses;
		}
		return out;
	}
private:
#endif
	BuildOptions options_;
	std::uint8_t mode_{};
	const Announcement *announcement_{};
	bool share_{};
	std::uint64_t adoptions_{};
	/* The records indirect misses named, made at the first one. */
	std::unique_ptr<LinkMemo> link_memo_;
	/* Large enough that an ordinary build never reaches the upstream: a
	 * stream that doubles its way to 272 words consumes 510 words of
	 * scratch on the way. */
	std::array<std::byte, 32U << 10> scratch_;
	std::pmr::monotonic_buffer_resource build_scratch_;
	/* At most uml_gadget_resident_generations + 1 entries, so finding one is
	 * cheap; the state each holds is what makes retiring it proportional. */
	std::unordered_map<unsigned long, Generation> generations_;
	/* generations_ is node-based, so this stays valid across insertion; only
	 * retiring this very address space could invalidate it, and retirement
	 * only ever takes the least recently current one. */
	Generation *current_{};
	std::vector<unsigned long> resident_;
	unsigned long current_generation_{};
#if UML_GADGET_CACHE_CENSUS
	mutable std::uint64_t stream_lookups_{};
	census::History history_;
	census::Schedule schedule_;
	unsigned census_id_{census::next_cache_id()};
	/* Address-space switches, the clock dormancy is counted in. */
	std::uint64_t switches_{};
#endif
	/* What the running address space adds to a key to make a tag, and
	 * the epoch its slots must carry to be current. Both are read by
	 * every lookup, so they sit here rather than behind current_. */
	std::uint64_t current_owner_{no_owner};
	std::uint16_t current_epoch_{};
	/* Address spaces by id, with id zero never used, and the ids of the
	 * ones retired. */
	std::vector<Generation *> owners_{nullptr};
	std::vector<std::uint16_t> free_ids_;
	std::array<std::unique_ptr<Slab>, slab_count> slabs_;
	std::array<std::uint8_t, set_count> next_replacement_{};
};

GadgetExecutor::GadgetExecutor(arm64::gadgets::BuildOptions build_options)
	: build_options_{build_options},
	  sampling_{diagnostics::guest_sampling_register(*this)}
{
	tlb_.unaligned_atomics =
		build_options_.host_capabilities.has(ArmFeature::lse2);
	/* Executors running one address space share its translations when
	 * sharing is enabled, the host retargets a link pair in one access, and
	 * a reclaimer can see whether this executor is running translated
	 * code. */
	announcement_ = acquire_announcement();
	share_translations_ = announcement_ != nullptr &&
		build_options_.host_capabilities.has(ArmFeature::lse2) &&
		shared_bodies_enabled();
	programs_ = std::make_unique<ProgramCache>(build_options_, 0,
		announcement_, share_translations_);
}

GadgetExecutor::~GadgetExecutor()
{
	if (sampling_ != nullptr)
		diagnostics::guest_sampling_unregister(*sampling_);
#if UML_GADGET_EXECUTION_HISTOGRAM
	diagnostics::execution_histogram_forget_executor(&cache_diagnostics_);
#endif
#if UML_GADGET_CACHE_CENSUS
	/* Every executor of the run reports its own caches, and the union
	 * across them is the measurement. */
	census::append([this](std::FILE *out) {
		programs_->census(out, 0);
		for (const auto &mode : sme_caches_)
			if (mode.programs)
				mode.programs->census(out, 0);
	});
#endif
	if (current_cpu_ != nullptr) {
		current_cpu_->gadget_indirect_miss = nullptr;
		arm64::restore_native_fp_environment(*current_cpu_);
	}
	darwin_uml_static_vm_execution_context_destroy(execution_context_);
	/* The caches let go of their address spaces before the executor stops
	 * being one a reclaimer waits for. */
	programs_.reset();
	for (auto &mode : sme_caches_)
		mode.programs.reset();
	release_announcement(announcement_);
}

bool GadgetExecutor::begin_execution(unsigned long generation,
				     std::uint64_t guest_pc)
{
	preempted_ = false;
	auto *context = execution_context(generation);
	if (context == nullptr ||
	    darwin_uml_static_vm_execution_context_begin(context, guest_pc) != 0)
		return false;
	if (announcement_ != nullptr)
		announce_entry(*announcement_);
	if (sampling_ != nullptr)
		diagnostics::guest_sampling_enter(*sampling_);
	return true;
}

void GadgetExecutor::end_execution() noexcept
{
	if (sampling_ != nullptr)
		diagnostics::guest_sampling_leave(*sampling_);
	if (current_cpu_ != nullptr) {
		/* A miss is a pointer into this executor's cache, not architectural
		 * state. Learning the target can allocate after the guest branch has
		 * retired; discard the marker even when that postamble throws. */
		current_cpu_->gadget_indirect_miss = nullptr;
		arm64::restore_native_fp_environment(*current_cpu_);
	}
	current_cpu_ = nullptr;
	if (execution_context_ != nullptr)
		(void)darwin_uml_static_vm_execution_context_end(execution_context_);
	if (announcement_ != nullptr)
		announce_exit(*announcement_);
}

void GadgetExecutor::select_cpu(CpuState &cpu) noexcept
{
	if (current_cpu_ == &cpu)
		return;
	cpu.gadget_return_stack = {};
	cpu.gadget_return_top = 0;
	if (current_cpu_ != nullptr) {
		current_cpu_->gadget_indirect_miss = nullptr;
		arm64::restore_native_fp_environment(*current_cpu_);
	}
	/* The marker is executor-private scratch. Never inherit it across CPU or
	 * execution reuse; end_execution() resets current_cpu_ at every exit. */
	cpu.gadget_indirect_miss = nullptr;
	current_cpu_ = &cpu;
	if (sampling_ != nullptr)
		diagnostics::guest_sampling_thread(*sampling_, &cpu.tls);
}

void *GadgetExecutor::execution_context(unsigned long generation)
{
	if (execution_context_ != nullptr && execution_generation_ == generation)
		return execution_context_;
	darwin_uml_static_vm_execution_context_destroy(execution_context_);
	execution_context_ =
		darwin_uml_static_vm_execution_context_create(generation);
	execution_generation_ = execution_context_ != nullptr ? generation : 0;
	const auto *sequence = execution_context_ != nullptr ?
		darwin_uml_static_vm_execution_context_code_sequence(
			execution_context_) : nullptr;
	code_sequence_ = sequence != nullptr ? sequence : &arm64::no_code_changes;
	return execution_context_;
}

void GadgetExecutor::prepare_cache(
	unsigned long generation, std::uint64_t changes)
{
	if (cache_generation_ == generation && cache_changes_ == changes)
		return;
	/* Switching address space does not empty the cache: slots, links and
	 * indirect sites all carry the address space they describe, so the
	 * translations of the one being left stay resident for its return. */
	programs_->prepare(generation, changes);
	cache_generation_ = generation;
	cache_changes_ = changes;
}

bool GadgetExecutor::sample_block(const void *stream,
	diagnostics::SampledBlock &block) const noexcept
{
	if (programs_->sample_block(stream, block))
		return true;
	for (const auto &mode : sme_caches_)
		if (mode.programs && mode.programs->sample_block(stream, block))
			return true;
	return false;
}

std::optional<BuildResult> GadgetExecutor::find(
	std::uint64_t pc, std::uint64_t changes, StaticGuestMemory &memory)
{
	return programs_->find(
		pc, changes, mapping_changes_, memory, execution_context_,
		cache_diagnostics_);
}

/* Produce and store a translation the cache did not hold. run_block
 * counts it: with the count in here, run_block's per-dispatch cache lookup
 * compiles differently. */
BuildResult GadgetExecutor::translate(
	std::uint64_t pc, std::span<const std::uint32_t> window)
{
	const auto decoded = arm64::gadgets::Builder{}.build(
		pc, window,
		[this](std::uint64_t target) {
			return programs_->link(target);
		},
		programs_->program_storage(),
		[this](std::uint64_t source_pc) {
			return programs_->indirect_link(source_pc);
		},
		build_options_);
	return programs_->store(pc, decoded, window, cache_changes_);
}

[[gnu::always_inline]] inline ::uml::cpu::EngineEvent
GadgetExecutor::finish_block(ProgramCache &programs, const BuildResult &built,
	::uml::cpu::EngineEvent event, arm64::CpuState &cpu,
	std::size_t &executed)
{
	const bool fpcr_enable_exit =
		event == ::uml::cpu::EngineEvent::continued &&
		(cpu.effective_fpcr() &
		 arm64::detail::fpcr_exception_enable_mask) != 0U;
	const bool count_exit =
		terminal_does_not_retire(event) || fpcr_enable_exit;
	if (count_exit) {
		const auto *current_program = &built.program;
		const auto *exit_stream = cpu.gadget_indirect_miss;
		std::optional<BuildResult> exit_block;
		if (!program_contains_stream(*current_program, exit_stream)) {
			exit_block = programs.find_stream(exit_stream);
			if (exit_block) {
				current_program = &exit_block->program;
				arm64::gadgets::resolve_fault_position(
					*current_program, cpu.pc);
			}
		}
		if (fpcr_enable_exit)
			correct_fpcr_enable_instruction_count(
				*current_program, event, cpu, executed);
		else
			correct_terminal_instruction_count(
				*current_program, event, cpu.pc, executed);
		cpu.gadget_indirect_miss = nullptr;
	}
#if UML_GADGET_EXECUTION_HISTOGRAM
	{
		auto &d = cache_diagnostics_;
		if (event == ::uml::cpu::EngineEvent::system_call)
			++d.exits_syscall;
		else if (event == ::uml::cpu::EngineEvent::memory_fault)
			++d.exits_fault;
		else if (event == ::uml::cpu::EngineEvent::interrupt)
			++d.exits_interrupt;
		else if (event != ::uml::cpu::EngineEvent::continued)
			++d.exits_other;
		else if (cpu.gadget_indirect_miss != nullptr) {
			++d.exits_indirect_miss;
			if (static_cast<const arm64::gadgets::IndirectLink *>(
				    cpu.gadget_indirect_miss)->kind != 0)
				++d.exits_indirect_miss_return;
			if ((d.exits_indirect_miss & 4095) == 0)
				d.miss_by_site_targets = programs.miss_distribution();
		}
		else if (executed >= 4096)
			++d.exits_chain_limit;
		else
			++d.exits_unlinked;
		diagnostics::execution_histogram_note_executor(&d);
	}
#endif
	/* A stream pointer left by a non-retiring or FPCR-enable exit was
	 * consumed and cleared above, so what remains is a cache cell. */
	if (cpu.gadget_indirect_miss != nullptr &&
	    (cpu.pc & (alignof(std::uint32_t) - 1U)) == 0U) {
		programs.learn_indirect(
			*static_cast<arm64::gadgets::IndirectLink *>(
				cpu.gadget_indirect_miss),
			cpu.pc);
	}
	cpu.gadget_indirect_miss = nullptr;
	return event;
}

::uml::cpu::EngineEvent GadgetExecutor::run_block(
	arm64::CpuState &cpu, StaticGuestMemory &memory, unsigned long generation,
	std::size_t &executed)
{
	executed = 0;
	floating_point_syndrome_ = 0;
	tlb_.fault_alignment = 0;
	select_cpu(cpu);
	auto *context = execution_context(generation);
	if (context == nullptr)
		return ::uml::cpu::EngineEvent::fatal;
	std::array<std::uint32_t, block_instruction_count> instructions;
	const auto sequences =
		darwin_uml_static_vm_execution_context_sequences(context);
	preempted_ = sequences.execution_interrupt != 0;
	if (preempted_)
		return ::uml::cpu::EngineEvent::interrupt;
	/* What the translations this run reaches are validated at, below: an
	 * ISB leaves when the address space's code has changed since. */
	cpu.code_sequence = code_sequence_;
	cpu.code_sequence_seen = sequences.code_changes;
	if (mapping_generation_ != generation ||
	    mapping_changes_ != sequences.mapping_changes) {
		memory.prepare(generation, sequences.mapping_changes);
		tlb_.prepare(generation, sequences.mapping_changes);
		mapping_generation_ = generation;
		mapping_changes_ = sequences.mapping_changes;
	}
	tlb_.tagged_addresses = cpu.tbi_enabled;
	if (cpu.svcr != 0U) [[unlikely]]
		return run_sme_block(cpu, memory, generation,
			sequences.code_changes, executed);
	/* CpuState::effective_fpcr(), open-coded: through the member this
	 * block-entry test lowers to a word load and mask instead of a byte
	 * test, and run_block inlines more around it. */
	const auto guest_fpcr = cpu.fp_loaded ? cpu.fpcr : cpu.deferred_fpcr;
	if (single_step_ || (guest_fpcr &
	     arm64::detail::fpcr_exception_enable_mask) != 0U)
		return run_trap_enabled_instruction(cpu, memory, executed);
	if (cache_generation_ != generation) {
		/* Return records name link state of the address space being
		 * left; the ring must not survive into another. */
		cpu.gadget_return_stack = {};
		cpu.gadget_return_top = 0;
	}
	prepare_cache(generation, sequences.code_changes);
	auto selected = find(cpu.pc, sequences.code_changes, memory);
#if UML_GADGET_EXECUTION_HISTOGRAM
	++cache_diagnostics_.block_entries;
	if (!selected)
		++cache_diagnostics_.cache_misses;
#endif
	if (!selected) {
		const auto count = fetch_instruction_window(
			memory, cpu.pc, instructions);
		if (count == 0) {
			const auto fault = memory.last_fault();
			tlb_.fault_address = fault.address;
			tlb_.fault_access = ::uml::memory::translated_access(
				fault.access);
			return ::uml::cpu::EngineEvent::memory_fault;
		}
		const auto window = std::span{instructions}.first(count);
		++cache_diagnostics_.translations;
		selected.emplace(translate(cpu.pc, window));
	}
	const auto &built = *selected;
	if (built.program.instruction_count() == 0) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}
	/* Published on every entry: the CpuState may last have run another
	 * address space, whose table this cache may since have retired. */
	cpu.gadget_target_tier = programs_->target_tier();
	const auto event =
		arm64::gadgets::execute_preserving_native_fp_environment(
			built.program, cpu, tlb_, executed);
	return finish_block(*programs_, built, event, cpu, executed);
}

/* The SME modes' run_block. Its cache, links and target table are the
 * mode's own, so everything the program can reach was translated for the
 * mode it runs in; a guest SVCR write ends its block, and the next run looks
 * its successor up in the new mode. */
[[gnu::noinline]] ::uml::cpu::EngineEvent GadgetExecutor::run_sme_block(
	arm64::CpuState &cpu, StaticGuestMemory &memory,
	unsigned long generation, std::uint64_t code_changes,
	std::size_t &executed)
{
	const auto mode = static_cast<std::uint8_t>(cpu.svcr & arm64::svcr_mask);
	/* SVCR leaves 0 only through an SME instruction or an SME task the
	 * kernel loaded, and either needs the profile and a register file. */
	if (mode != cpu.svcr || cpu.sme_registers == nullptr ||
	    !arm64::implements_sme(build_options_.host_capabilities))
		return ::uml::cpu::EngineEvent::fatal;
	/* Floating-point exceptions never trap in streaming mode. */
	const bool streaming = (mode & arm64::svcr_sm) != 0U;
	if (single_step_ || (!streaming && (cpu.effective_fpcr() &
	     arm64::detail::fpcr_exception_enable_mask) != 0U))
		return run_trap_enabled_instruction(cpu, memory, executed);
	/* A streaming program runs natively in streaming mode; an injected
	 * profile without the hardware cannot run one. */
	if (streaming && !build_options_.host_capabilities.sme_hardware)
		return ::uml::cpu::EngineEvent::fatal;
	auto &state = sme_caches_[mode - 1U];
	if (!state.programs)
		state.programs = std::make_unique<ProgramCache>(build_options_,
			mode, announcement_, share_translations_);
	auto &programs = *state.programs;
	if (state.generation != generation) {
		cpu.gadget_return_stack = {};
		cpu.gadget_return_top = 0;
	}
	if (state.generation != generation || state.changes != code_changes) {
		programs.prepare(generation, code_changes);
		state.generation = generation;
		state.changes = code_changes;
	}
	auto selected = programs.find(cpu.pc, code_changes, mapping_changes_,
		memory, execution_context_, cache_diagnostics_);
#if UML_GADGET_EXECUTION_HISTOGRAM
	++cache_diagnostics_.block_entries;
	if (!selected)
		++cache_diagnostics_.cache_misses;
#endif
	if (!selected) {
		std::array<std::uint32_t, block_instruction_count> instructions;
		const auto count = fetch_instruction_window(
			memory, cpu.pc, instructions);
		if (count == 0) {
			const auto fault = memory.last_fault();
			tlb_.fault_address = fault.address;
			tlb_.fault_access = ::uml::memory::translated_access(
				fault.access);
			return ::uml::cpu::EngineEvent::memory_fault;
		}
		const auto window = std::span{instructions}.first(count);
		++cache_diagnostics_.translations;
		auto options = build_options_;
		options.svcr = mode;
		const auto decoded = arm64::gadgets::Builder{}.build(
			cpu.pc, window,
			[&programs](std::uint64_t target) {
				return programs.link(target);
			},
			programs.program_storage(),
			[&programs](std::uint64_t source_pc) {
				return programs.indirect_link(source_pc);
			},
			options);
		selected.emplace(programs.store(cpu.pc, decoded, window,
			code_changes));
	}
	const auto &built = *selected;
	if (built.program.instruction_count() == 0) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}
	cpu.access_sme_state();
	cpu.gadget_target_tier = programs.target_tier();
	if (!streaming) {
		const auto event =
			arm64::gadgets::execute_preserving_native_fp_environment(
				built.program, cpu, tlb_, executed);
		return finish_block(programs, built, event, cpu, executed);
	}
	/* Streaming programs carry no FP link entry; the FP state they can
	 * reach is loaded here, once, for every program they link to. XNU
	 * resets FPSR on every kernel entry while a thread is in streaming
	 * mode, so the guest's FPSR must not be in the host register: leave
	 * the native FP environment first, as the SVCR write that entered
	 * streaming mode already did. */
	arm64::gadgets::restore_native_fp_environment(cpu);
	cpu.access_fp_state();
	const auto event =
		arm64::gadgets::execute_streaming_preserving_native_fp_environment(
			built.program, cpu, tlb_, executed,
			arm64::sme_profile(build_options_.host_capabilities) ==
				arm64::SmeProfile::sme2);
	return finish_block(programs, built, event, cpu, executed);
}

/* ------------------------------------------------------------ AArch32 */

namespace {

/* Restores what an AArch32 entry changes in the TLB that an A64 entry does
 * not set again: address tagging is set at every A64 entry, but whether an
 * unaligned exclusive faults is configured once. AArch32 has no LSE2 form of
 * LDREX, so every misaligned exclusive faults. */
class A32TlbScope final {
public:
	A32TlbScope(::uml::memory::Tlb &tlb) noexcept
		: tlb_{tlb}, unaligned_atomics_{tlb.unaligned_atomics}
	{
		tlb_.tagged_addresses = false;
		tlb_.unaligned_atomics = false;
	}
	~A32TlbScope() { tlb_.unaligned_atomics = unaligned_atomics_; }
	A32TlbScope(const A32TlbScope &) = delete;
	A32TlbScope &operator=(const A32TlbScope &) = delete;

private:
	::uml::memory::Tlb &tlb_;
	bool unaligned_atomics_;
};

} // namespace

::uml::cpu::EngineEvent GadgetExecutor::run_a32_block(
	arm64::CpuState &cpu, StaticGuestMemory &memory, unsigned long generation,
	std::size_t &executed)
{
	executed = 0;
	floating_point_syndrome_ = 0;
	tlb_.fault_alignment = 0;
	select_cpu(cpu);
	auto *context = execution_context(generation);
	if (context == nullptr)
		return ::uml::cpu::EngineEvent::fatal;
	const auto sequences =
		darwin_uml_static_vm_execution_context_sequences(context);
	preempted_ = sequences.execution_interrupt != 0;
	if (preempted_)
		return ::uml::cpu::EngineEvent::interrupt;
	/* What the translations this run reaches are validated at, below: an
	 * ISB leaves when the address space's code has changed since. */
	cpu.code_sequence = code_sequence_;
	cpu.code_sequence_seen = sequences.code_changes;
	if (mapping_generation_ != generation ||
	    mapping_changes_ != sequences.mapping_changes) {
		memory.prepare(generation, sequences.mapping_changes);
		tlb_.prepare(generation, sequences.mapping_changes);
		mapping_generation_ = generation;
		mapping_changes_ = sequences.mapping_changes;
	}
	/* An AArch32 task never has a streaming mode, and the profile has no
	 * trap-enabled floating point. */
	if (cpu.svcr != 0U)
		return ::uml::cpu::EngineEvent::fatal;
	const A32TlbScope tlb_scope{tlb_};
	/* A single step, and an entry in the middle of an IT block, run one
	 * instruction uncached until ITSTATE is zero again. */
	if (single_step_ || cpu.a32_itstate != 0U)
		return run_a32_instruction(cpu, memory, executed);
	return run_a32_translation(cpu, memory, generation,
		sequences.code_changes, executed);
}

::uml::cpu::EngineEvent GadgetExecutor::run_a32_translation(
	arm64::CpuState &cpu, StaticGuestMemory &memory, unsigned long generation,
	std::uint64_t code_changes, std::size_t &executed)
{
	if (cache_generation_ != generation) {
		cpu.gadget_return_stack = {};
		cpu.gadget_return_top = 0;
	}
	prepare_cache(generation, code_changes);
	const auto key = cpu.pc;
	auto selected = find(key, code_changes, memory);
#if UML_GADGET_EXECUTION_HISTOGRAM
	++cache_diagnostics_.block_entries;
	if (!selected)
		++cache_diagnostics_.cache_misses;
#endif
	if (!selected) {
		const auto isa = arm64::aarch32::key_isa(key);
		const auto unit = arm64::aarch32::isa_unit_bytes(isa);
		std::array<std::uint32_t, block_instruction_count> window;
		const auto bytes = std::as_writable_bytes(std::span{window});
		const auto address = arm64::aarch32::key_pc(key);
		const auto count = fetch_a32_units(memory, address, unit, bytes);
		if (count == 0) {
			const auto fault = memory.last_fault();
			tlb_.fault_address = fault.address;
			tlb_.fault_access = ::uml::memory::translated_access(
				fault.access);
			return ::uml::cpu::EngineEvent::memory_fault;
		}
		++cache_diagnostics_.translations;
		const auto output = arm64::gadgets::a32::build(
			{.key = key, .code = bytes.first(count * unit)},
			[this](std::uint64_t target) {
				return programs_->link(target);
			},
			programs_->program_storage(),
			[this](std::uint64_t source_key) {
				return programs_->indirect_link(source_key);
			},
			build_options_,
			a32::decode_profile(build_options_),
			block_instruction_count);
		if (output.units == 0) {
			/* A T32 instruction whose second halfword lies on a page
			 * that cannot be fetched: the fetch fault is at that
			 * halfword. */
			std::uint16_t halfword = 0;
			(void)memory.fetch(address + count * unit,
				std::as_writable_bytes(std::span{&halfword, 1}));
			const auto fault = memory.last_fault();
			tlb_.fault_address = fault.address;
			tlb_.fault_access = ::uml::memory::translated_access(
				fault.access);
			return ::uml::cpu::EngineEvent::memory_fault;
		}
		selected.emplace(programs_->store_validated(key, output.result,
			bytes.first(output.units * unit), cache_changes_));
	}
	const auto &built = *selected;
	if (built.program.instruction_count() == 0) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}
	cpu.gadget_target_tier = programs_->target_tier();
	const auto event =
		arm64::gadgets::execute_preserving_native_fp_environment(
			built.program, cpu, tlb_, executed);
	return finish_a32_block(*programs_, event, cpu, memory, executed);
}

/* One instruction, uncached, with the ITSTATE it is entered with. */
::uml::cpu::EngineEvent GadgetExecutor::run_a32_instruction(
	arm64::CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed)
{
	cpu.gadget_indirect_miss = nullptr;
	cpu.gadget_target_tier = nullptr;
	const auto key = cpu.pc;
	const auto isa = arm64::aarch32::key_isa(key);
	const auto unit = arm64::aarch32::isa_unit_bytes(isa);
	/* Two halfwords: a T32 instruction is one or two. */
	std::array<std::uint32_t, 1> window;
	const auto bytes = std::as_writable_bytes(std::span{window});
	const auto address = arm64::aarch32::key_pc(key);
	const auto count = fetch_a32_units(memory, address, unit, bytes);
	if (count == 0) {
		const auto fault = memory.last_fault();
		tlb_.fault_address = fault.address;
		tlb_.fault_access = ::uml::memory::translated_access(fault.access);
		return ::uml::cpu::EngineEvent::memory_fault;
	}
	const auto output = arm64::gadgets::a32::build(
		{.key = key, .code = bytes.first(count * unit),
		 .it = arm64::aarch32::ItState{cpu.a32_itstate}, .single = true},
		{}, std::pmr::get_default_resource(), {}, build_options_,
		a32::decode_profile(build_options_),
		block_instruction_count);
	if (output.units == 0) {
		std::uint16_t halfword = 0;
		(void)memory.fetch(address + count * unit,
			std::as_writable_bytes(std::span{&halfword, 1}));
		const auto fault = memory.last_fault();
		tlb_.fault_address = fault.address;
		tlb_.fault_access = ::uml::memory::translated_access(fault.access);
		return ::uml::cpu::EngineEvent::memory_fault;
	}
	const auto &built = output.result;
	if (built.program.instruction_count() == 0) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}
	const auto event = arm64::gadgets::execute(
		built.program, cpu, tlb_, executed);
	/* A memory gadget without a key reports its position, which is 0. */
	if ((cpu.pc & 3U) == 1U)
		cpu.pc = key;
	/* The one instruction is the whole program: a terminal that did not
	 * retire gives it back. */
	if (terminal_does_not_retire(event))
		executed = 0;
	cpu.gadget_indirect_miss = nullptr;
	if (event == ::uml::cpu::EngineEvent::continued ||
	    event == ::uml::cpu::EngineEvent::interrupt ||
	    event == ::uml::cpu::EngineEvent::system_call)
		cpu.a32_itstate = output.next_it.bits();
	if (event == ::uml::cpu::EngineEvent::breakpoint)
		breakpoint_immediate_ = read_breakpoint_immediate(memory, cpu.pc);
	/* A one-instruction fallthrough ends in the synthetic interrupt gadget:
	 * an internal cold boundary, not a guest-visible interrupt. */
	return event == ::uml::cpu::EngineEvent::interrupt ?
		::uml::cpu::EngineEvent::continued : event;
}

::uml::cpu::EngineEvent GadgetExecutor::finish_a32_block(
	ProgramCache &programs, ::uml::cpu::EngineEvent event,
	arm64::CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed)
{
	/* SVC and BKPT leave the stream they ran from, which is not a cache
	 * cell; so do the exits that did not retire. The block holding it is
	 * found from the stream, because the exit may be in a linked successor. */
	const bool leaves_marker =
		event == ::uml::cpu::EngineEvent::system_call ||
		event == ::uml::cpu::EngineEvent::breakpoint;
	const bool does_not_retire = terminal_does_not_retire(event);
	if (does_not_retire || leaves_marker) {
		if (const auto exit = programs.find_stream_source(
			    cpu.gadget_indirect_miss)) {
			const auto &program = exit->result.program;
			const auto guest = program.guest_address();
			/* The memory gadgets other than VLDM, VSTM and the
			 * structure transfers carry no key: they report the
			 * position of their instruction in the block, which has
			 * bit 0 set where a key has none. */
			if ((cpu.pc & 3U) == 1U) {
				const auto at = static_cast<std::size_t>(cpu.pc >> 2);
				std::optional<std::size_t> unit;
				if (arm64::aarch32::key_is_thumb(guest))
					unit = a32::thumb_unit(exit->source, at);
				else if (at < program.instruction_count())
					unit = at;
				if (unit)
					cpu.pc = guest + *unit * 4U;
			}
			const auto position_key = cpu.pc;
			if (position_key >= guest &&
			    ((position_key - guest) & 3U) == 0U) {
				const auto unit = static_cast<std::size_t>(
					(position_key - guest) / 4U);
				std::optional<std::size_t> position;
				if (arm64::aarch32::key_is_thumb(guest)) {
					position = a32::thumb_position(
						exit->source, unit);
					cpu.a32_itstate = a32::thumb_itstate(
						exit->source, unit,
						a32::decode_profile(build_options_));
				} else if (unit < program.instruction_count()) {
					position = unit;
				}
				if (does_not_retire && position &&
				    *position < program.instruction_count()) {
					const auto unretired =
						program.instruction_count() - *position;
					if (executed >= unretired)
						executed -= unretired;
				}
			}
		}
		cpu.gadget_indirect_miss = nullptr;
	}
	if (event == ::uml::cpu::EngineEvent::breakpoint)
		breakpoint_immediate_ = read_breakpoint_immediate(memory, cpu.pc);
#if UML_GADGET_EXECUTION_HISTOGRAM
	{
		auto &d = cache_diagnostics_;
		if (event == ::uml::cpu::EngineEvent::system_call)
			++d.exits_syscall;
		else if (event == ::uml::cpu::EngineEvent::memory_fault)
			++d.exits_fault;
		else if (event == ::uml::cpu::EngineEvent::interrupt)
			++d.exits_interrupt;
		else if (event != ::uml::cpu::EngineEvent::continued)
			++d.exits_other;
		else if (cpu.gadget_indirect_miss != nullptr) {
			++d.exits_indirect_miss;
			if (static_cast<const arm64::gadgets::IndirectLink *>(
				    cpu.gadget_indirect_miss)->kind != 0)
				++d.exits_indirect_miss_return;
			if ((d.exits_indirect_miss & 4095) == 0)
				d.miss_by_site_targets = programs.miss_distribution();
		}
		else if (executed >= 4096)
			++d.exits_chain_limit;
		else
			++d.exits_unlinked;
		diagnostics::execution_histogram_note_executor(&d);
	}
#endif
	if (event == ::uml::cpu::EngineEvent::continued &&
	    cpu.gadget_indirect_miss != nullptr &&
	    (cpu.pc & (alignof(std::uint32_t) - 1U)) == 0U) {
		programs.learn_indirect(
			*static_cast<arm64::gadgets::IndirectLink *>(
				cpu.gadget_indirect_miss),
			cpu.pc);
	}
	cpu.gadget_indirect_miss = nullptr;
	return event;
}

::uml::cpu::EngineEvent GadgetExecutor::run_trap_enabled_instruction(
	arm64::CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed)
{
	/* Guest trap enables and single steps are intentionally a cold
	 * execution mode.  Limiting every translated run to one instruction
	 * guarantees that an integer or memory instruction cannot carry
	 * execution past a later FP/SIMD data-processing instruction in the
	 * same cached block, or past the instruction being stepped.
	 */
	cpu.gadget_indirect_miss = nullptr;
	/* Its one-instruction builds name no link state, but no stale table
	 * may be left published either. */
	cpu.gadget_target_tier = nullptr;
	arm64::gadgets::restore_native_fp_environment(cpu);
	std::uint32_t instruction = 0;
	auto encoded = std::as_writable_bytes(std::span{&instruction, 1});
	if (!memory.fetch(cpu.pc, encoded)) {
		const auto fault = memory.last_fault();
		tlb_.fault_address = fault.address;
		tlb_.fault_access = ::uml::memory::translated_access(fault.access);
		return ::uml::cpu::EngineEvent::memory_fault;
	}

	auto options = build_options_;
	options.svcr = static_cast<std::uint8_t>(cpu.svcr & arm64::svcr_mask);
	auto built = arm64::gadgets::Builder{}.build(
		cpu.pc, std::span{&instruction, 1}, {},
		std::pmr::get_default_resource(), {}, options);
	if (!built.complete || built.program.instruction_count() != 1) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}
	if (options.svcr != 0U) {
		cpu.access_sme_state();
		if ((options.svcr & arm64::svcr_sm) != 0U) {
			/* Floating-point exceptions never trap in streaming
			 * mode, so there is no transaction to run. */
			if (!build_options_.host_capabilities.sme_hardware)
				return ::uml::cpu::EngineEvent::fatal;
			cpu.access_fp_state();
			const auto event = arm64::gadgets::
				execute_streaming_preserving_native_fp_environment(
					built.program, cpu, tlb_, executed,
					arm64::sme_profile(
						build_options_.host_capabilities) ==
						arm64::SmeProfile::sme2);
			arm64::gadgets::restore_native_fp_environment(cpu);
			correct_terminal_instruction_count(
				built.program, event, cpu.pc, executed);
			cpu.gadget_indirect_miss = nullptr;
			return event == ::uml::cpu::EngineEvent::interrupt ?
				::uml::cpu::EngineEvent::continued : event;
		}
	}

	const bool transactional =
		arm64::detail::needs_floating_exception_transaction(
			cpu.effective_fpcr(),
			instruction);
	if (!transactional) {
		const auto event = arm64::gadgets::execute(
			built.program, cpu, tlb_, executed);
		correct_terminal_instruction_count(
			built.program, event, cpu.pc, executed);
		cpu.gadget_indirect_miss = nullptr;
		/* A one-instruction fallthrough ends in Builder's synthetic
		 * interrupt gadget.  It is an internal cold-mode boundary, not a
		 * guest-visible fairness interrupt.
		 */
		return event == ::uml::cpu::EngineEvent::interrupt ?
			::uml::cpu::EngineEvent::continued : event;
	}

	cpu.access_fp_state();
	const auto original_fpsr = cpu.fpsr;
	arm64::CpuState isolated = cpu;
	isolated.fpsr &=
		~arm64::detail::fpsr_exception_cumulative_mask;

	std::size_t isolated_executed = 0;
	const auto event = arm64::gadgets::execute(
		built.program, isolated, tlb_, isolated_executed);
	if (event != ::uml::cpu::EngineEvent::interrupt &&
	    event != ::uml::cpu::EngineEvent::continued) {
		/* Reserved allocations and other non-executing events retain the
		 * live architectural state.
		 */
		correct_terminal_instruction_count(
			built.program, event, isolated.pc, isolated_executed);
		isolated.gadget_indirect_miss = nullptr;
		executed = isolated_executed;
		return event;
	}

	const auto decision = arm64::detail::decide_a64_floating_exception(
		cpu.fpcr, original_fpsr, isolated.fpsr, instruction,
		cpu.vectors[(instruction >> 5U) & 31U].bytes,
		isolated.vectors[instruction & 31U].bytes);
	if (decision.traps()) {
		cpu.fpsr = decision.committed_fpsr;
		floating_point_syndrome_ = decision.syndrome;
		executed = 0;
		return ::uml::cpu::EngineEvent::floating_point_trap;
	}

	/* The scratch execution began with cumulative flags clear.  Preserve
	 * every other FPSR field produced by the gadget while restoring the
	 * pre-existing cumulative state before committing the transaction.
	 */
	isolated.fpsr =
		(isolated.fpsr &
		 ~arm64::detail::fpsr_exception_cumulative_mask) |
		(decision.committed_fpsr &
		 arm64::detail::fpsr_exception_cumulative_mask);
	cpu = isolated;
	executed = isolated_executed;
	return ::uml::cpu::EngineEvent::continued;
}

bool GadgetExecutor::fault_alignment() const noexcept
{
	return tlb_.fault_alignment != 0;
}

std::uint64_t GadgetExecutor::fault_address() const noexcept
{
	return tlb_.fault_address;
}

::uml::memory::Access GadgetExecutor::fault_access() const noexcept
{
	return tlb_.fault_access;
}

std::uint32_t GadgetExecutor::floating_point_syndrome() const noexcept
{
	return floating_point_syndrome_;
}

} // namespace uml::cpu::arm64::gadgets
