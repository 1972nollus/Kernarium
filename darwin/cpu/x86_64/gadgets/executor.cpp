// SPDX-License-Identifier: GPL-2.0-or-later
#include "executor.hpp"

#include "cpu/gadget_cache_geometry.hpp"
#include "cpu/gadget_record_table.hpp"
#if UML_GADGET_CACHE_CENSUS
#include "cpu/gadget_cache_census.hpp"
#endif
#include "cpu/x86_64/code_page_registry.hpp"

#include "native_entry.hpp"
#include "native_symbols.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#if UML_GADGET_CACHE_CENSUS
#include <cstdio>
#include <malloc/malloc.h>
#endif
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uml::cpu::x86_64::gadgets {
#if UML_GADGET_CACHE_CENSUS
namespace census = ::uml::cpu::gadget_cache::census;
#endif
namespace {

static_assert(DARWIN_UML_GUEST_PAGE_SIZE == 4096,
	"x86-64 gadgets are a 4 KiB guest product");
inline constexpr std::size_t tlb_entry_count = native_tlb_entry_count;
inline constexpr std::size_t program_cache_ways = gadget_cache::ways;
inline constexpr std::size_t program_cache_slots =
	gadget_cache::slots;
inline constexpr std::size_t program_cache_sets =
	program_cache_slots / program_cache_ways;
inline constexpr std::size_t program_cache_slab_slots = gadget_cache::slab_slots;
inline constexpr std::size_t program_cache_slab_count =
	program_cache_slots / program_cache_slab_slots;
inline constexpr std::size_t resident_generation_limit =
	gadget_cache::resident_generations;
inline constexpr std::size_t maximum_cached_source_bytes = 64U * 15U;
/* A prepare that would walk more than this replays nothing and retires the
 * whole address space instead, which is cheaper than the walk. */
inline constexpr std::size_t code_change_replay_limit = 64;
inline constexpr std::uint64_t code_change_replay_pages = 128;
static_assert(maximum_cached_source_bytes <= DARWIN_UML_GUEST_PAGE_SIZE,
	"one cached x86 program may span at most two code pages");
static_assert((program_cache_slots & (program_cache_slots - 1U)) == 0,
	"x86-64 gadget cache slots must be a power of two");
static_assert(program_cache_slots >= program_cache_slab_slots * 4U &&
	program_cache_slots % program_cache_slab_slots == 0,
	"x86-64 gadget cache must contain at least four complete slabs");
static_assert(program_cache_sets * program_cache_ways == program_cache_slots);
static_assert(resident_generation_limit != 0,
	"x86-64 gadget cache must retain at least one address space");
static_assert(std::is_nothrow_move_constructible_v<Program>);

struct ProgramCacheLocation final {
	std::size_t set;
	std::size_t slab;
	std::size_t offset;
};

[[nodiscard]] ProgramCacheLocation locate_program(
	std::uint64_t generation, std::uint64_t address,
	ExecutionMode mode) noexcept
{
	/* Keep the main-cache index as compact as the arm64 gadget path. x86
	 * instruction pointers are byte-granular, so retain their low bits and
	 * fold the execution mode into a high bit before the two xor folds. */
	const auto block = address ^
		(generation * 0x9e37'79b9'7f4a'7c15ULL) ^
		(static_cast<std::uint64_t>(mode) << 32U);
	const auto mixed = block ^ (block >> 12U) ^ (block >> 24U);
	const auto set = static_cast<std::size_t>(mixed) &
		(program_cache_sets - 1U);
	const auto first = set * program_cache_ways;
	return {
		.set = set,
		.slab = first / program_cache_slab_slots,
		.offset = first % program_cache_slab_slots,
	};
}

enum class ProgramSourceComparison {
	match,
	mismatch,
	fault,
};

struct ProgramCodeStamps final {
	std::array<::uml::memory::CodePageStamp, 2> pages{};
	std::uint8_t count{};
	bool complete{};

	friend bool operator==(
		const ProgramCodeStamps &, const ProgramCodeStamps &) = default;
};

/* A page stamp certifies only the bytes read after it was taken: a store that
 * lands later moves the stamp, and one that landed earlier is already visible
 * in the bytes. Every caller therefore takes the stamp before the source it
 * stands for, never after. */
[[nodiscard]] ProgramCodeStamps capture_program_code_stamps(
	::uml::memory::GuestAddressSpace &memory, const Program &program,
	void *execution_context) noexcept
{
	ProgramCodeStamps result;
	auto address = program.address();
	auto remaining = program.byte_count();
	auto previous_page = ~std::uint64_t{};
	while (remaining != 0) {
		const auto page = address &
			~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U};
		if (page != previous_page) {
			if (result.count == result.pages.size() ||
			    !memory.code_page_stamp(address, execution_context,
				    result.pages[result.count]))
				return {};
			++result.count;
			previous_page = page;
		}
		const auto count = std::min<std::size_t>(remaining,
			DARWIN_UML_GUEST_PAGE_SIZE -
				(address & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));
		remaining -= count;
		address = advance_instruction_pointer(
			program.execution_mode(), address, count);
	}
	result.complete = result.count != 0;
	return result;
}

/* Boot profiling shows that more than 90 percent of stale-block validation
 * spans are at most 32 bytes. Keep those comparisons in the caller instead of
 * paying the platform memcmp entry cost for one to four ordinary word loads. */
[[nodiscard]] bool equal_small_source(const std::byte *current,
	const std::byte *expected, std::size_t size) noexcept
{
	while (size >= sizeof(std::uint64_t)) {
		std::uint64_t current_word{};
		std::uint64_t expected_word{};
		std::memcpy(&current_word, current, sizeof(current_word));
		std::memcpy(&expected_word, expected, sizeof(expected_word));
		if (current_word != expected_word)
			return false;
		current += sizeof(current_word);
		expected += sizeof(expected_word);
		size -= sizeof(current_word);
	}
	if (size >= sizeof(std::uint32_t)) {
		std::uint32_t current_word{};
		std::uint32_t expected_word{};
		std::memcpy(&current_word, current, sizeof(current_word));
		std::memcpy(&expected_word, expected, sizeof(expected_word));
		if (current_word != expected_word)
			return false;
		current += sizeof(current_word);
		expected += sizeof(expected_word);
		size -= sizeof(current_word);
	}
	if (size >= sizeof(std::uint16_t)) {
		std::uint16_t current_word{};
		std::uint16_t expected_word{};
		std::memcpy(&current_word, current, sizeof(current_word));
		std::memcpy(&expected_word, expected, sizeof(expected_word));
		if (current_word != expected_word)
			return false;
		current += sizeof(current_word);
		expected += sizeof(expected_word);
		size -= sizeof(current_word);
	}
	return size == 0 || *current == *expected;
}

[[nodiscard]] bool equal_source(const std::byte *current,
	const std::byte *expected, std::size_t size) noexcept
{
	return size <= 32 ? equal_small_source(current, expected, size) :
		std::memcmp(current, expected, size) == 0;
}

[[nodiscard]] ProgramSourceComparison compare_program_source(
	::uml::memory::GuestAddressSpace &memory, const Program &program,
	std::span<const std::byte> expected)
{
	if (expected.size() != program.byte_count())
		return ProgramSourceComparison::mismatch;

	std::array<std::byte, maximum_cached_source_bytes> fetched;
	auto address = program.address();
	std::size_t completed = 0;
	while (completed < expected.size()) {
		auto count = expected.size() - completed;
		const auto last = program.execution_mode() == ExecutionMode::compat32 ?
			std::uint64_t{0xffff'ffffU} : ~std::uint64_t{};
		const auto before_wrap = last - address;
		if (count - 1U > before_wrap)
			count = static_cast<std::size_t>(before_wrap) + 1U;
		count = std::min<std::size_t>(count,
			DARWIN_UML_GUEST_PAGE_SIZE -
				(address & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));

		const auto saved = expected.subspan(completed, count);
		if (const auto *mapped = memory.atomic_pointer(address, count,
				::uml::memory::MemoryAccess::execute);
			mapped != nullptr) {
			if (!equal_source(mapped, saved.data(), count))
				return ProgramSourceComparison::mismatch;
		} else {
			auto copied = std::span{fetched}.first(count);
			if (!memory.fetch(address, copied))
				return ProgramSourceComparison::fault;
			if (!equal_source(copied.data(), saved.data(), count))
				return ProgramSourceComparison::mismatch;
		}

		completed += count;
		address = advance_instruction_pointer(
			program.execution_mode(), address, count);
	}
	return ProgramSourceComparison::match;
}

/* An unlinked BlockLink's stream is the one link_exit word in front of it,
 * which takes the RIP from the BlockLink. */
struct UnlinkedBlockLink final {
	std::uintptr_t fallback_stream;
	BlockLink link;
};
static_assert(offsetof(UnlinkedBlockLink, link) == sizeof(std::uintptr_t),
	"link_exit reads the RIP two words past its stream word");

/* Entries no block has claimed name an address no block begins at and carry
 * that address's own fallback, so the translated tier can load the pointer
 * before it has compared the address, and a guest that really does branch
 * there still leaves with the right RIP.
 */
[[nodiscard]] const TargetLinkTable &vacant_target_links() noexcept
{
	static const UnlinkedBlockLink vacant{
		reinterpret_cast<std::uintptr_t>(
			native::uml_x86_64_gadget_link_exit),
		{&vacant.fallback_stream, 0, ~std::uint64_t{}, ~std::uint64_t{},
			~std::uint64_t{}},
	};
	static const TargetLinkTable table = [] {
		TargetLinkTable initial{};
		for (auto &entry : initial)
			entry = {~std::uint64_t{}, &vacant.link};
		return initial;
	}();
	return table;
}

/* The registries of every address-space generation this executor holds
 * translations for, and its participation in the one it is running. A page
 * is counted once per executor that caches code on it, so a translation
 * retired here is subtracted here.
 */
struct CodePageRegistries final {
	std::unordered_map<std::uint64_t, std::shared_ptr<CodePageRegistry>> held;
	std::shared_ptr<CodePageRegistry> current;
	std::uint64_t current_generation{};
	CodePageRegistry::Participant participant;

	~CodePageRegistries() { unbind(); }

	[[nodiscard]] CodePageRegistry &of(std::uint64_t generation)
	{
		auto [entry, inserted] = held.try_emplace(generation);
		if (inserted)
			entry->second = CodePageRegistry::acquire(generation);
		return *entry->second;
	}

	void bind(std::uint64_t generation)
	{
		if (current && current_generation == generation)
			return;
		unbind();
		auto &registry = of(generation);
		registry.join(participant);
		current = held.find(generation)->second;
		current_generation = generation;
	}

	void unbind() noexcept
	{
		if (current)
			current->leave(participant);
		current.reset();
	}

	void release(std::uint64_t generation) noexcept
	{
		if (current && current_generation == generation)
			return;
		held.erase(generation);
	}

	void drop_all() noexcept
	{
		unbind();
		held.clear();
	}
};

/* Holds a translation's possible pages in the registry while their bytes are
 * read, so any store another executor has already translated for them lands
 * first. A cached program is at most maximum_cached_source_bytes long, so a
 * block beginning at one address can span that page and the page its last
 * possible byte falls in, and no other. The cache's own index keeps whichever
 * pages the program turns out to cover. */
class CodePageHold final {
public:
	CodePageHold(CodePageRegistries *registries, std::uint64_t address,
		ExecutionMode mode) noexcept
	{
		pages_[count_++] = address &
			~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U};
		/* Advancing in the block's own mode keeps the bound right where a
		 * compat32 block reaches its last byte past the 4 GiB wrap. */
		const auto last = advance_instruction_pointer(mode, address,
			maximum_cached_source_bytes - 1U) &
			~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U};
		if (last != pages_[0])
			pages_[count_++] = last;
		if (registries == nullptr || !registries->current)
			return;
		registry_ = registries->current.get();
		for (std::size_t index = 0; index < count_; ++index)
			registry_->add(pages_[index], &registries->participant);
	}
	~CodePageHold()
	{
		if (registry_ == nullptr)
			return;
		for (std::size_t index = 0; index < count_; ++index)
			registry_->remove(pages_[index]);
	}
	CodePageHold(const CodePageHold &) = delete;
	CodePageHold &operator=(const CodePageHold &) = delete;

	[[nodiscard]] std::span<const std::uint64_t> pages() const noexcept
	{
		return {pages_.data(), count_};
	}

private:
	std::array<std::uint64_t, 2> pages_{};
	std::size_t count_{};
	CodePageRegistry *registry_{};
};

/* The stamp of every page a block can span, read before the decoder reads a
 * byte of any of them. A page whose stamp the address space cannot produce is
 * recorded as absent rather than failing the whole capture, because a block
 * near the end of a text page holds the page after it without ever reaching
 * it, and that page need not be code at all. */
struct HeldCodeStamps final {
	std::array<::uml::memory::CodePageStamp, 2> stamps{};
	std::array<bool, 2> present{};
};

[[nodiscard]] HeldCodeStamps capture_held_code_stamps(
	::uml::memory::GuestAddressSpace &memory,
	std::span<const std::uint64_t> pages, void *execution_context) noexcept
{
	HeldCodeStamps result;
	for (std::size_t index = 0; index < pages.size(); ++index)
		result.present[index] = memory.code_page_stamp(
			pages[index], execution_context, result.stamps[index]);
	return result;
}

/* The stamps a fresh translation records, taken from the read made before the
 * decoder ran, for the pages the program turned out to cover. They certify the
 * program because the source the slot saves is the decoder's own bytes, read
 * after them: one observation of guest memory ("Keeping a cached translation"
 * in README.md).
 *
 * `nullopt` means a page this program covers was not held while its bytes were
 * read, which the block's byte bound makes impossible and which no later check
 * could recover; such a translation is used once and not cached. An address
 * space that cannot stamp a page gives no stamps at all, and the program keeps
 * the exact byte validation that address space already relies on.
 */
[[nodiscard]] std::optional<ProgramCodeStamps> program_code_stamps_from_hold(
	const Program &program, std::span<const std::uint64_t> held_pages,
	const HeldCodeStamps &held) noexcept
{
	ProgramCodeStamps result;
	auto address = program.address();
	auto remaining = program.byte_count();
	auto previous_page = ~std::uint64_t{};
	bool stamped = true;
	while (remaining != 0) {
		const auto page = address &
			~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U};
		if (page != previous_page) {
			std::size_t held_index = held_pages.size();
			for (std::size_t index = 0; index < held_pages.size(); ++index)
				if (held_pages[index] == page)
					held_index = index;
			if (held_index == held_pages.size() ||
			    result.count == result.pages.size())
				return std::nullopt;
			if (held.present[held_index])
				result.pages[result.count] =
					held.stamps[held_index];
			else
				stamped = false;
			++result.count;
			previous_page = page;
		}
		const auto count = std::min<std::size_t>(remaining,
			DARWIN_UML_GUEST_PAGE_SIZE -
				(address & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));
		remaining -= count;
		address = advance_instruction_pointer(
			program.execution_mode(), address, count);
	}
	if (!stamped)
		return ProgramCodeStamps{};
	result.complete = result.count != 0;
	return result;
}

class ProgramCache final {
private:
	struct LinkRecord;
	struct Slot;

	/* A slot's place in the list of slots holding code from one guest page.
	 * Every way exists whether or not it holds a translation, so the lists
	 * are threaded through slots by 32-bit link id -- two per slot, `2 *
	 * index + entry + 1`, with 0 for none -- rather than by pointer, and the
	 * page is not kept: entry 0 is the page the slot's first byte is on and
	 * entry 1 the page its last byte is on, which the slot's program still
	 * says whenever a link is taken out. */
	struct PageLink final {
		std::uint32_t previous{};
		std::uint32_t next{};
	};

	/* What a way holds only while it holds a translation: the program, the
	 * page stamps it was last validated against and its places in the
	 * code-page lists. A large cache commonly runs with half its ways empty,
	 * and every way pays for its slot, so these live in an allocation of
	 * their own, which a replaced translation hands to its successor. */
	struct Translation final {
		explicit Translation(Program &&built) noexcept
			: program{std::move(built)}
		{}

		std::array<PageLink, 2> page_links{};
		ProgramCodeStamps code_stamps{};
		Program program;
	};

	struct Slot final {
		/* Keep every field needed to reject a way in the first cache line.
		 * The program and saved bytes are touched only after the key matches. */
		std::uint64_t epoch{};
		std::uint64_t generation{};
		std::uint64_t address{};
		/* The slot's flat index, which names it in successor sets and page
		 * lists; fixed when its slab is made. */
		std::uint32_t index{};
		std::uint8_t instruction_limit{};
		ExecutionMode mode{ExecutionMode::long64};
		bool terminal{};
		/* Which of the translation's `page_links` are in a page's list;
		 * never set without a translation. */
		std::uint8_t linked_pages{};
		std::uint64_t validated_changes{};
		LinkRecord *link{};
		std::uint64_t link_epoch{};
		std::unique_ptr<Translation> translation;
	};
	static_assert(sizeof(Slot) == 64,
		"a way costs its slot whether or not it holds a translation");
	static_assert(program_cache_slots < (std::size_t{1} << 31),
		"a page link id fits 32 bits");

	/* The common monomorphic and bimorphic successors bypass the main
	 * four-way probe. A successor is named by its slot's index plus one, so
	 * zero is empty, and a slot's own set sits at that same index. Slots and
	 * their companion sets live in lazy slabs for the executor's lifetime;
	 * epoch and key checks make stale entries inert. */
	struct SuccessorSet final {
		std::array<std::uint32_t, 2> entries{};
		std::uint8_t next_replacement{};
	};

	struct Slab final {
		std::array<Slot, program_cache_slab_slots> slots;
		std::array<SuccessorSet, program_cache_slab_slots> successors;
	};

	/* Laid out as an UnlinkedBlockLink, so an unlinked record's stream is
	 * its own link_exit word. */
	struct LinkRecord final {
		explicit LinkRecord(std::uint64_t address) noexcept
			: fallback_stream{reinterpret_cast<std::uintptr_t>(
				native::uml_x86_64_gadget_link_exit)},
			  link{&fallback_stream, 0, address, address, address}
		{}

		void reset() noexcept
		{
			link.stream = &fallback_stream;
			link.instruction_count = 0;
			link.program_begin = link.guest_rip;
			link.program_end = link.guest_rip;
		}

		std::uintptr_t fallback_stream;
		BlockLink link;
		bool listed{};
		LinkRecord *next_live{};
	};
	static_assert(offsetof(LinkRecord, link) ==
			offsetof(UnlinkedBlockLink, link) &&
		sizeof(LinkRecord) == 64,
		"link_exit reads the RIP two words past its stream word");

	struct LinkKey final {
		[[nodiscard]] std::uint64_t operator()(
			const LinkRecord &record) const noexcept
		{
			return record.link.guest_rip;
		}
	};

	/* An indirect site is named by the branch that owns it, which its own
	 * cells do not record. Translated streams point at the site, the first
	 * member. */
	struct IndirectRecord final {
		explicit IndirectRecord(std::uint64_t source) noexcept
			: source_rip{source}
		{}

		IndirectLink site{};
		std::uint64_t source_rip;
	};

	struct IndirectKey final {
		[[nodiscard]] std::uint64_t operator()(
			const IndirectRecord &record) const noexcept
		{
			return record.source_rip;
		}
	};

	/* As in the arm64 cache, each address space owns the list of slots it has
	 * populated. Retiring the least-recently-current generation therefore
	 * scales with that generation's footprint rather than the whole cache.
	 * Its records are named by address from translated streams, so they live
	 * in chunks rather than map nodes (G210), and a link sweep releases the
	 * ones nothing names any more (sweep()).
	 */
	struct Generation final {
		std::uint64_t changes{};
		std::array<gadget_cache::StableRecordTable<LinkRecord, LinkKey>, 2>
			links;
		std::array<gadget_cache::StableRecordTable<IndirectRecord,
				   IndirectKey>, 2>
			indirect;
		/* The records and sites the last sweep kept, and the bodies
		 * that have left the cache since (gadget_cache::sweep_due). */
		std::size_t kept{};
		std::size_t departed{};
		LinkRecord *live_links{};
		std::array<std::unique_ptr<TargetLinkTable>, 2> target_links;
		std::array<std::vector<IndirectLink *>, 2> live_indirect;
		std::vector<std::uint32_t> slots;
		/* The first link of each page's list. */
		std::unordered_map<std::uint64_t, std::uint32_t> code_pages;
		bool slots_complete{true};
#if UML_GADGET_CACHE_CENSUS
		/* When the cache last left this address space, in switches and
		 * in seconds. */
		std::uint64_t census_switch{};
		double census_seconds{};
#endif
	};

public:
	void invalidate() noexcept
	{
#if UML_GADGET_CACHE_CENSUS
		for (auto &slab : slabs_)
			if (slab)
				for (auto &slot : slab->slots)
					if (resident(slot))
						census_depart(slot,
							census::Departure::retired);
#endif
		if (registries_ != nullptr)
			for (auto &[generation, state] : generations_)
				for (const auto &[page, link] : state.code_pages)
					registries_->of(generation).remove(page);
		resident_.clear();
		generations_.clear();
		current_ = nullptr;
		for (auto &slab : slabs_)
			if (slab)
				for (auto &slot : slab->slots)
					slot.linked_pages = 0;
		preceding_ = nullptr;
		++epoch_;
		if (epoch_ != 0)
			return;
		for (auto &slab : slabs_)
			if (slab)
				for (auto &slot : slab->slots)
					slot.epoch = 0;
		epoch_ = 1;
	}

	void prepare(std::uint64_t generation, std::uint64_t changes)
	{
		if (current_generation_ != generation)
			preceding_ = nullptr;
#if UML_GADGET_CACHE_CENSUS
		/* The address space being left was in use until now. */
		if (generation != current_generation_ && current_ != nullptr) {
			++switches_;
			current_->census_switch = switches_;
			current_->census_seconds = census::elapsed_seconds();
		}
#endif
		current_generation_ = generation;
		auto [entry, inserted] = generations_.try_emplace(generation);
		current_ = &entry->second;
		touch(generation);
		if (inserted) {
			current_->changes = changes;
			while (resident_.size() > resident_generation_limit)
				retire(resident_.front());
			return;
		}
		if (current_->changes == changes)
			return;
		preceding_ = nullptr;
		if (!retire_changed_code(*current_))
			invalidate_links(*current_);
		current_->changes = changes;
	}

	void set_code_change_reader(
		ProgramCacheCodeChangeReader reader, void *context) noexcept
	{
		code_change_reader_ = reader;
		code_change_context_ = context;
	}

	/* The write translations this cache retires when a page gains code, and
	 * the query the miss handler uses to record whether a page has any.
	 */
	void set_write_translations(NativeTlbEntry *entries) noexcept
	{
		write_translations_ = entries;
	}

	void set_code_page_registries(CodePageRegistries *registries) noexcept
	{
		registries_ = registries;
	}

	[[nodiscard]] bool page_holds_code(std::uint64_t address) const noexcept
	{
		if (current_ == nullptr)
			return false;
		return current_->code_pages.find(address & code_page_mask) !=
			current_->code_pages.end();
	}

	/* A write made by this executor already names its exact guest range.
	 * Reset only translations whose saved instruction bytes overlap it;
	 * unrelated links can remain live when this executor accepts its own
	 * publication at the next branch boundary.
	 */
	void invalidate_code(std::uint64_t address, std::size_t size) noexcept
	{
		if (current_ == nullptr || size == 0)
			return;
		auto current = address;
		auto remaining = size;
		while (remaining != 0) {
			const auto page = current & code_page_mask;
			const auto count = std::min<std::size_t>(remaining,
				DARWIN_UML_GUEST_PAGE_SIZE -
					(current & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));
			const auto found = current_->code_pages.find(page);
			if (found != current_->code_pages.end()) {
				for (auto id = found->second; id != 0;
					     id = page_link(id).next) {
					auto &slot = page_link_slot(id);
					if (slot.epoch == epoch_ &&
					    slot.generation == current_generation_ &&
					    slot.translation && source_overlaps(
						    slot.translation->program,
						    address, size))
						deactivate(slot);
				}
			}
			remaining -= count;
			current += count;
		}
	}

	void accept_code_changes(std::uint64_t changes) noexcept
	{
		if (current_ != nullptr)
			current_->changes = changes;
	}

	[[nodiscard]] const BlockLink *link(
		std::uint64_t address, ExecutionMode mode)
	{
		return &link_record(current(), address, mode).link;
	}

	/* The tier an entering block probes. It belongs to the address space, so
	 * retiring a generation retires its tier with it and no entry can name a
	 * record that has been destroyed. */
	[[nodiscard]] const TargetLinkEntry *target_links(
		ExecutionMode mode) noexcept
	{
		auto *table = current_ == nullptr ? nullptr :
			target_table(*current_, mode);
		return table == nullptr ? vacant_target_links().data() :
			table->data();
	}

	[[nodiscard]] IndirectLink *indirect_link(
		std::uint64_t source, ExecutionMode mode)
	{
		const auto *empty = link(empty_target(mode), mode);
		auto [created, inserted] = current().indirect[mode_index(mode)]
			.try_emplace(source, source);
		if (inserted)
			reset_indirect(created->site, empty_target(mode), empty);
		return &created->site;
	}

	void learn_indirect(IndirectLink &site, std::uint64_t target,
		ExecutionMode mode)
	{
		target = normalize_instruction_pointer(mode, target);
		const auto *target_link = link(target, mode);
		if (!site.listed) {
			current().live_indirect[mode_index(mode)].push_back(&site);
			site.listed = true;
		}
		for (auto &entry : site.entries) {
			if (entry.guest_rip != target)
				continue;
			entry.link = target_link;
			return;
		}
		auto &entry = site.entries[site.next_replacement & 1U];
		entry.guest_rip = target;
		entry.link = target_link;
		site.next_replacement ^= 1U;
	}

	[[nodiscard]] const Program *find(std::uint64_t address,
		ExecutionMode mode, std::size_t maximum_instructions,
		std::uint64_t changes, std::uint64_t mapping_changes,
		::uml::memory::GuestAddressSpace &memory, void *execution_context,
		ProgramCacheSequenceReader sequence_reader,
		ProgramCacheDiagnostics &diagnostics, bool &cache_hit)
	{
		cache_hit = false;
#if UML_GADGET_CACHE_CENSUS
		if ((history_.lookups() & 4095U) == 0)
			snapshot_if_due();
#endif
		if (preceding_ != nullptr) {
			for (const auto successor : preceding_->entries) {
				if (successor == 0)
					continue;
				auto &slot = slot_at(successor - 1U);
				if (!matches(slot, address, mode))
					continue;
				const auto *program = use(slot,
					maximum_instructions, changes, mapping_changes,
					memory, execution_context, sequence_reader,
					diagnostics);
				if (program == nullptr) {
					preceding_ = nullptr;
					return nullptr;
				}
				activate(slot, successors_at(successor - 1U));
				++diagnostics.successor_hits;
				cache_hit = true;
				return program;
			}
		}
		const auto location = locate_program(
			current_generation_, address, mode);
		auto &slab = slabs_[location.slab];
		if (!slab) {
#if UML_GADGET_CACHE_CENSUS
			history_.missed();
#endif
			return nullptr;
		}
		for (std::size_t way = 0; way < program_cache_ways; ++way) {
			auto &slot = slab->slots[location.offset + way];
			if (!matches(slot, address, mode))
				continue;
			const auto *program = use(slot, maximum_instructions,
				changes, mapping_changes, memory, execution_context,
				sequence_reader, diagnostics);
			if (program == nullptr)
				return nullptr;
			activate(slot, slab->successors[location.offset + way]);
			cache_hit = true;
			return program;
		}
#if UML_GADGET_CACHE_CENSUS
		history_.missed();
#endif
		return nullptr;
	}

	void cancel_selection() noexcept { preceding_ = nullptr; }

	/* The resident program at `address` whose stream holds `word`, which a
	 * linked run can be inside of: every block a link enters is resident. */
	[[nodiscard]] const Program *holding(std::uint64_t address,
		ExecutionMode mode, const std::uintptr_t *word) const noexcept
	{
		if (current_ == nullptr)
			return nullptr;
		const auto location = locate_program(
			current_generation_, address, mode);
		if (location.slab >= slabs_.size() || !slabs_[location.slab])
			return nullptr;
		const auto &slab = *slabs_[location.slab];
		for (std::size_t way = 0; way < program_cache_ways; ++way) {
			const auto &slot = slab.slots[location.offset + way];
			if (!matches(slot, address, mode) || !slot.translation)
				continue;
			const auto stream = slot.translation->program.native_stream();
			if (word >= stream.data() &&
				word < stream.data() + stream.size())
				return &slot.translation->program;
		}
		return nullptr;
	}

	[[nodiscard]] const Program &store(Program &&program,
		std::uint64_t changes,
		std::size_t instruction_limit, ProgramCodeStamps code_stamps,
		ProgramCacheDiagnostics &diagnostics)
	{
		(void)link(program.address(), program.execution_mode());

		const auto location = locate_program(current_generation_,
			program.address(), program.execution_mode());
		auto &slab = slabs_[location.slab];
		if (!slab) {
			slab = std::make_unique<Slab>();
			for (std::size_t offset = 0; offset < program_cache_slab_slots;
			     ++offset)
				slab->slots[offset].index = static_cast<std::uint32_t>(
					location.slab * program_cache_slab_slots + offset);
		}

#if UML_GADGET_CACHE_CENSUS
		/* A lookup that could not use its translation can leave it
		 * resident, and this store makes a second copy beside it. */
		bool restored = false;
		for (std::size_t way = 0; way < program_cache_ways; ++way)
			if (matches(slab->slots[location.offset + way],
				    program.address(), program.execution_mode()))
				restored = true;
#endif
		auto &replacement = next_replacement_[location.set];
		std::size_t selected = replacement;
		for (std::size_t way = 0; way < program_cache_ways; ++way) {
			const auto &slot = slab->slots[location.offset + way];
			if (slot.epoch != epoch_) {
				selected = way;
				break;
			}
		}
		replacement = static_cast<std::uint8_t>(
			(selected + 1U) & (program_cache_ways - 1U));

		auto &state = current();
		if (state.slots_complete) {
			if (state.slots.size() < program_cache_slots) {
				state.slots.push_back(static_cast<std::uint32_t>(
					location.slab * program_cache_slab_slots +
					location.offset + selected));
			} else {
				state.slots_complete = false;
				state.slots.clear();
				state.slots.shrink_to_fit();
			}
		}

		auto &slot = slab->slots[location.offset + selected];
		auto &successors = slab->successors[location.offset + selected];
#if UML_GADGET_CACHE_CENSUS
		if (resident(slot))
			census_depart(slot, census::Departure::evicted);
#endif
		depart(slot);
		unindex(slot);
		deactivate(slot);
		slot.epoch = 0;
		successors = {};
		if (slot.translation)
			slot.translation->program = std::move(program);
		else
			slot.translation =
				std::make_unique<Translation>(std::move(program));
		const auto &stored = slot.translation->program;
		slot.generation = current_generation_;
		slot.address = stored.address();
		slot.validated_changes = changes;
		slot.translation->code_stamps = code_stamps;
		slot.instruction_limit = kept_instruction_limit(instruction_limit);
		slot.mode = stored.execution_mode();
		slot.terminal = stored.terminal();
		/* Epoch is the validity marker and is written only after the slot is
		 * complete. */
		slot.epoch = epoch_;
		try {
			index(slot);
		} catch (...) {
			slot.epoch = 0;
			slot.translation.reset();
			throw;
		}
		activate_link(slot);
		activate(slot, successors);
#if UML_GADGET_CACHE_CENSUS
		history_.arrive(census_id_, slot.generation, census_key(slot),
			census_digest(slot), restored);
#endif
		/* The build's words are in the slot's program now, so nothing
		 * outside what a sweep reads names a record it made. */
		if (gadget_cache::sweep_due(records(state), state.kept,
			    state.departed))
			sweep(state, current_generation_, diagnostics);
		return stored;
	}

	[[nodiscard]] std::size_t resident_generations() const noexcept
	{
		return generations_.size();
	}

#if UML_GADGET_CACHE_CENSUS
	/* What this cache costs, in bytes it actually holds rather than in the
	 * capacity it was configured with. The slab array is the only part that
	 * exists whether or not its ways are occupied; everything else is
	 * proportional to what the workload translated. A translation's parts
	 * are counted as the allocator sized them (X79). */
	struct Footprint final {
		std::size_t slabs, streams, decoded, links, indirect, targets;
		[[nodiscard]] std::size_t total() const noexcept
		{
			return slabs + streams + decoded + links + indirect +
				targets;
		}
	};

	/* One address space's link state. */
	struct GenerationBytes final {
		std::size_t links, indirect, targets;
	};

	[[nodiscard]] static GenerationBytes generation_bytes(
		const Generation &state) noexcept
	{
		GenerationBytes bytes{};
		for (const auto &mode : state.links)
			bytes.links += mode.bytes();
		bytes.links += state.slots.capacity() * sizeof(std::uint32_t) +
			map_bytes(state.code_pages);
		for (const auto &mode : state.indirect)
			bytes.indirect += mode.bytes();
		for (const auto &live : state.live_indirect)
			bytes.indirect += live.capacity() * sizeof(IndirectLink *);
		for (const auto &table : state.target_links)
			if (table)
				bytes.targets += sizeof(TargetLinkTable);
		return bytes;
	}

	[[nodiscard]] Footprint footprint() const noexcept
	{
		std::size_t slabs = 0;
		std::size_t streams = 0;
		std::size_t decoded = 0;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			slabs += malloc_size(slab.get());
			for (const auto &slot : slab->slots) {
				if (!resident(slot))
					continue;
				/* A translation is its record, the native
				 * stream and the saved source; a block some
				 * replay has ended inside also holds what that
				 * replay decoded. */
				const auto parts = translation_bytes(slot);
				streams += parts.allocated - parts.decoded;
				decoded += parts.decoded;
			}
		}
		std::size_t links = 0;
		std::size_t indirect = 0;
		std::size_t targets = 0;
		for (const auto &[generation, state] : generations_) {
			const auto bytes = generation_bytes(state);
			links += bytes.links;
			indirect += bytes.indirect;
			targets += bytes.targets;
		}
		return {
			.slabs = slabs,
			.streams = streams,
			.decoded = decoded,
			.links = links,
			.indirect = indirect,
			.targets = targets,
		};
	}

	[[nodiscard]] std::size_t occupancy() const noexcept
	{
		std::size_t occupied = 0;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots)
				if (resident(slot))
					++occupied;
		}
		return occupied;
	}

	/* Write what this cache holds and how it came to hold it, so
	 * duplication, dormancy and lost work can be counted instead of
	 * estimated. `snapshot` numbers a periodic snapshot; zero is the one
	 * written at teardown. The record format is the arm64 executor's, so
	 * one analyzer reads either engine's capture.
	 *
	 * A translation is identified by the guest bytes it consumed, not by the
	 * stream it produced: two executors translating the same code emit the
	 * same gadgets but embed pointers to their own link records, so only the
	 * source identifies the work. The parts of its allocations go out with
	 * it because they decide what a translation costs.
	 */
	void census(std::FILE *out, std::uint64_t snapshot) const noexcept
	{
		const auto parts = footprint();
		/* sizeof(Slot) is reported because the slab total covers the
		 * successor sets as well as the tags, and only the slot size
		 * separates them. */
		const auto slab_count = static_cast<std::size_t>(
			std::count_if(slabs_.begin(), slabs_.end(),
				[](const auto &slab) { return slab != nullptr; }));
		std::fprintf(out,
			"E %p generations=%zu occupancy=%zu slots=%zu "
			"slot_bytes=%zu slab_slots=%zu slab_count=%zu slabs=%zu "
			"streams=%zu decoded=%zu links=%zu indirect=%zu "
			"targets=%zu total=%zu sweeps=%llu swept_links=%llu "
			"swept_sites=%llu sweep_ns=%llu",
			static_cast<const void *>(this), resident_generations(),
			occupancy(), program_cache_slots, sizeof(Slot),
			program_cache_slab_slots, slab_count, parts.slabs,
			parts.streams, parts.decoded, parts.links,
			parts.indirect, parts.targets, parts.total(),
			static_cast<unsigned long long>(sweeps_),
			static_cast<unsigned long long>(swept_links_),
			static_cast<unsigned long long>(swept_sites_),
			static_cast<unsigned long long>(sweep_ns_));
		census::write_when(out, snapshot);
		history_.write(out);
		std::fputc('\n', out);
		const auto now = census::elapsed_seconds();
		for (const auto &[generation, state] : generations_) {
			const auto found = std::find(resident_.rbegin(),
				resident_.rend(), generation);
			const bool running = generation == current_generation_;
			const auto bytes = generation_bytes(state);
			std::fprintf(out,
				"G %llu current=%d rank=%td idle_switches=%llu "
				"idle_seconds=%.3f link_records=%zu links=%zu "
				"indirect=%zu targets=%zu\n",
				static_cast<unsigned long long>(generation),
				running ? 1 : 0, found - resident_.rbegin(),
				static_cast<unsigned long long>(running ? 0 :
					switches_ - state.census_switch),
				running ? 0.0 : now - state.census_seconds,
				state.links[0].size() + state.links[1].size(),
				bytes.links, bytes.indirect, bytes.targets);
		}
		const auto sample = census::word_sample();
		/* The same link targets are sampled in every cache, so the
		 * sample says how many of an address space's records each
		 * cache keeps for itself. */
		if (sample != 0)
			for (const auto &[generation, state] : generations_)
				for (const auto &mode : state.links)
					mode.for_each([&](const LinkRecord &record) {
						if (census_sampled(record.link.guest_rip,
							    sample))
							std::fprintf(out,
								"L %llu 0x%llx\n",
								static_cast<unsigned long long>(
									generation),
								static_cast<unsigned long long>(
									record.link.guest_rip));
					});
		const auto text = census::text_of(reinterpret_cast<const void *>(
			reinterpret_cast<std::uintptr_t>(
				native::uml_x86_64_gadget_link_exit)));
		CensusRecords records;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots) {
				if (!resident(slot))
					continue;
				const auto &program = slot.translation->program;
				const auto bytes = translation_bytes(slot);
				std::fprintf(out,
					"P %llu 0x%llx %zu %zu %zu 0x%llx %zu %zu "
					"%zu %zu\n",
					static_cast<unsigned long long>(
						slot.generation),
					static_cast<unsigned long long>(
						slot.address),
					program.instruction_count(),
					program.native_stream().size(),
					program.source().size(),
					static_cast<unsigned long long>(
						census_digest(slot)),
					bytes.allocated, sizeof(Translation),
					program.native_stream().size_bytes(),
					program.source().size());
				if (sample != 0 &&
				    census_sampled(slot.address, sample))
					census_words(out, slot, text, records);
			}
		}
	}
#endif

private:
	static constexpr std::uint64_t code_page_mask =
		~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U};

#if UML_GADGET_CACHE_CENSUS
	/* A slot holds a translation only while its epoch is the live one; the
	 * epoch is written last, so nothing else has to be cleared to retire
	 * one. */
	[[nodiscard]] bool resident(const Slot &slot) const noexcept
	{
		return slot.epoch == epoch_;
	}

	/* libc++ keeps one heap node per element -- the value, a next pointer
	 * and the cached hash -- plus one pointer per bucket. Exact for the
	 * value, close enough for the node. */
	template <typename Map>
	[[nodiscard]] static std::size_t map_bytes(const Map &map) noexcept
	{
		constexpr std::size_t node = 2 * sizeof(void *);
		return map.size() *
			(sizeof(typename Map::value_type) + node) +
			map.bucket_count() * sizeof(void *);
	}

	/* A resident translation's allocations as the allocator sized them:
	 * its record, stream and source, and what a replay decoded. */
	struct TranslationBytes final {
		std::size_t allocated, decoded;
	};

	[[nodiscard]] static TranslationBytes translation_bytes(
		const Slot &slot) noexcept
	{
		const auto &program = slot.translation->program;
		const auto replay = program.replay_bytes();
		const auto decoded = replay != 0 ? malloc_good_size(replay) : 0;
		return {
			.allocated = malloc_size(slot.translation.get()) +
				malloc_size(program.native_stream().data()) +
				malloc_size(program.source().data()) + decoded,
			.decoded = decoded,
		};
	}

	/* The census keys a translation by address space and guest address;
	 * the execution mode takes the top bit, which no canonical address
	 * of either mode sets. */
	[[nodiscard]] static std::uint64_t census_key(const Slot &slot) noexcept
	{
		return slot.address ^
			(static_cast<std::uint64_t>(mode_index(slot.mode)) << 63);
	}

	/* The same bytes decoded in the other execution mode are a different
	 * translation, so the mode seeds the digest. */
	[[nodiscard]] static std::uint64_t census_digest(const Slot &slot) noexcept
	{
		auto digest = census::digest_seed ^ mode_index(slot.mode);
		for (const auto byte : slot.translation->program.source())
			digest = census::digest_add(digest,
				static_cast<std::uint8_t>(byte));
		return digest;
	}

	void census_depart(const Slot &slot, census::Departure why)
	{
		if (!slot.translation)
			return;
		history_.depart(slot.generation, census_key(slot),
			census_digest(slot), translation_bytes(slot).allocated, why);
	}

	/* What each record a stream can point at names, by the record's
	 * address, for each address space: a link record by the guest
	 * address it links to, an indirect site by the branch that owns
	 * it. */
	using CensusRecords = std::unordered_map<std::uint64_t,
		std::unordered_map<std::uintptr_t, std::pair<char, std::uint64_t>>>;

	[[nodiscard]] static bool census_sampled(
		std::uint64_t address, std::uint64_t one_in) noexcept
	{
		auto value = address * 0x9e3779b97f4a7c15ULL;
		value ^= value >> 29;
		return value % one_in == 0;
	}

	/* One sampled translation's stream, word by word: `g` a gadget by its
	 * offset in the executable's text, `l` a link record by the guest
	 * address it links to, `i` an indirect site by the branch that owns
	 * it, and `o` any other word as it is. */
	void census_words(std::FILE *out, const Slot &slot,
		census::TextRange text, CensusRecords &records) const
	{
		auto [named, inserted] = records.try_emplace(slot.generation);
		if (inserted) {
			const auto owner = generations_.find(slot.generation);
			if (owner != generations_.end()) {
				auto &table = named->second;
				for (const auto &mode : owner->second.links)
					mode.for_each([&table](const LinkRecord &record) {
						table[reinterpret_cast<std::uintptr_t>(
							&record.link)] =
							{'l', record.link.guest_rip};
					});
				for (const auto &mode : owner->second.indirect)
					mode.for_each(
						[&table](const IndirectRecord &record) {
							table[reinterpret_cast<
								std::uintptr_t>(&record.site)] =
								{'i', record.source_rip};
						});
			}
		}
		std::fprintf(out, "W 0x%llx",
			static_cast<unsigned long long>(slot.address));
		for (const auto value :
			slot.translation->program.native_stream()) {
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

	struct ProgramPages final {
		std::array<std::uint64_t, 2> pages{};
		std::size_t count{};
	};

	[[nodiscard]] static ProgramPages program_pages(
		const Program &program) noexcept
	{
		ProgramPages result;
		auto current = program.address();
		auto remaining = program.byte_count();
		while (remaining != 0) {
			const auto page = current & code_page_mask;
			if (result.count == 0 ||
			    result.pages[result.count - 1U] != page)
				result.pages[result.count++] = page;
			const auto count = std::min<std::size_t>(remaining,
				DARWIN_UML_GUEST_PAGE_SIZE -
					(current & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));
			remaining -= count;
			current = advance_instruction_pointer(
				program.execution_mode(), current, count);
		}
		return result;
	}

	[[nodiscard]] static bool source_overlaps(const Program &program,
		std::uint64_t address, std::size_t size) noexcept
	{
		auto source = program.address();
		auto source_remaining = program.byte_count();
		while (source_remaining != 0) {
			const auto source_page = source & code_page_mask;
			const auto source_count = std::min<std::size_t>(source_remaining,
				DARWIN_UML_GUEST_PAGE_SIZE -
					(source & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));
			auto written = address;
			auto written_remaining = size;
			while (written_remaining != 0) {
				const auto written_page = written & code_page_mask;
				const auto written_count = std::min<std::size_t>(
					written_remaining, DARWIN_UML_GUEST_PAGE_SIZE -
						(written & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));
				if (source_page == written_page && source <
					    written + written_count && written <
					    source + source_count)
					return true;
				written_remaining -= written_count;
				written += written_count;
			}
			source_remaining -= source_count;
			source = advance_instruction_pointer(
				program.execution_mode(), source, source_count);
		}
		return false;
	}

	void index(Slot &slot)
	{
		if (!slot.translation)
			return;
		const auto pages = program_pages(slot.translation->program);
		if (write_translations_ != nullptr)
			for (std::size_t entry = 0; entry < pages.count; ++entry) {
				auto &translation = write_translations_[
					native_tlb_index(pages.pages[entry])];
				/* One is the unmatchable empty tag: guest pages are aligned,
				 * so no real page compares equal to it.
				 */
				translation.write_page = 1;
			}
		auto &index = current().code_pages;
		std::size_t completed = 0;
		try {
			for (; completed < pages.count; ++completed) {
				auto [bucket, inserted] =
					index.try_emplace(pages.pages[completed], 0U);
				if (inserted && registries_ != nullptr)
					registries_->of(slot.generation).add(
						pages.pages[completed], &registries_->participant);
				const auto id = page_link_id(slot, completed);
				auto &link = slot.translation->page_links[completed];
				link = {.previous = 0, .next = bucket->second};
				if (link.next != 0)
					page_link(link.next).previous = id;
				bucket->second = id;
				slot.linked_pages |=
					static_cast<std::uint8_t>(1U << completed);
			}
		} catch (...) {
			for (std::size_t entry = 0; entry < completed; ++entry)
				unlink(current_generation_, current(), slot, entry);
			throw;
		}
	}

	[[nodiscard]] static std::uint32_t page_link_id(
		const Slot &slot, std::size_t entry) noexcept
	{
		return slot.index * 2U + static_cast<std::uint32_t>(entry) + 1U;
	}

	[[nodiscard]] Slot &page_link_slot(std::uint32_t id) noexcept
	{
		return slot_at((id - 1U) / 2U);
	}

	[[nodiscard]] PageLink &page_link(std::uint32_t id) noexcept
	{
		return page_link_slot(id).translation->page_links[
			(id - 1U) % 2U];
	}

	/* Take one of a slot's links out of its page's list. The slot must
	 * still hold the program the link was made for, which names the page. */
	void unlink(std::uint64_t generation, Generation &state, Slot &slot,
		std::size_t entry) noexcept
	{
		const auto bit = static_cast<std::uint8_t>(1U << entry);
		if ((slot.linked_pages & bit) == 0)
			return;
		auto &link = slot.translation->page_links[entry];
		if (link.previous != 0) {
			page_link(link.previous).next = link.next;
		} else {
			const auto page = program_pages(
				slot.translation->program).pages[entry];
			auto bucket = state.code_pages.find(page);
			if (bucket != state.code_pages.end() &&
			    bucket->second == page_link_id(slot, entry)) {
				bucket->second = link.next;
				if (bucket->second == 0) {
					state.code_pages.erase(bucket);
					if (registries_ != nullptr)
						registries_->of(generation).remove(page);
				}
			}
		}
		if (link.next != 0)
			page_link(link.next).previous = link.previous;
		link = {};
		slot.linked_pages &= static_cast<std::uint8_t>(~bit);
	}

	void unindex(Slot &slot) noexcept
	{
		if (slot.linked_pages == 0)
			return;
		auto owner = generations_.find(slot.generation);
		if (owner == generations_.end()) {
			slot.linked_pages = 0;
			return;
		}
		auto &links = slot.translation->page_links;
		for (std::size_t entry = 0; entry < links.size(); ++entry)
			unlink(slot.generation, owner->second, slot, entry);
	}

	/* A slot keeps the limit it was built under in one byte. The runtime
	 * asks for at most 64 instructions; a larger limit is kept, and
	 * compared, as 255, so a block built under one serves any other. */
	[[nodiscard]] static std::uint8_t kept_instruction_limit(
		std::size_t limit) noexcept
	{
		return static_cast<std::uint8_t>(std::min<std::size_t>(limit, 0xffU));
	}

	[[nodiscard]] bool matches(const Slot &slot, std::uint64_t address,
		ExecutionMode mode) const noexcept
	{
		return slot.epoch == epoch_ &&
			slot.generation == current_generation_ &&
			slot.address == address && slot.mode == mode;
	}

	[[nodiscard]] const Program *use(Slot &slot,
		std::size_t maximum_instructions, std::uint64_t changes,
		std::uint64_t mapping_changes,
		::uml::memory::GuestAddressSpace &memory, void *execution_context,
		ProgramCacheSequenceReader sequence_reader,
		ProgramCacheDiagnostics &diagnostics)
	{
		if (!slot.translation) {
#if UML_GADGET_CACHE_CENSUS
			history_.missed();
#endif
			discard(slot);
			return nullptr;
		}
		if (!slot.terminal && kept_instruction_limit(maximum_instructions) >
			slot.instruction_limit) {
#if UML_GADGET_CACHE_CENSUS
			history_.missed();
			census_depart(slot, census::Departure::discarded);
#endif
			discard(slot);
			return nullptr;
		}
#if UML_GADGET_CACHE_CENSUS
		if (slot.validated_changes == changes)
			history_.hit();
#endif
		if (slot.validated_changes != changes) {
			++diagnostics.validation_attempts;
			if (!validate(slot, changes, mapping_changes, memory,
				execution_context, sequence_reader, diagnostics)) {
#if UML_GADGET_CACHE_CENSUS
				history_.missed();
#endif
				return nullptr;
			}
#if UML_GADGET_CACHE_CENSUS
			history_.revalidated();
#endif
		}
		/* Whatever reset this record -- a replaced slot, or a code change
		 * that cooled the whole address space -- the program in this slot is
		 * valid for the current sequence by the time it is returned, so it is
		 * a legitimate link target again. Re-arming here is what lets a chain
		 * rebuild as blocks run, rather than only when one needs revalidating.
		 */
		activate_link(slot);
		return &slot.translation->program;
	}

	void activate(Slot &slot, SuccessorSet &successors) noexcept
	{
		if (preceding_ != nullptr) {
			const auto id = slot.index + 1U;
			auto selected = static_cast<std::size_t>(
				preceding_->next_replacement & 1U);
			for (std::size_t way = 0; way < preceding_->entries.size(); ++way) {
				const auto entry = preceding_->entries[way];
				if (entry == 0 || entry == id) {
					selected = way;
					break;
				}
			}
			preceding_->entries[selected] = id;
			preceding_->next_replacement = static_cast<std::uint8_t>(
				(selected + 1U) & 1U);
		}
		preceding_ = &successors;
	}

	[[nodiscard]] static std::size_t mode_index(ExecutionMode mode) noexcept
	{
		return mode == ExecutionMode::compat32 ? 1U : 0U;
	}

	[[nodiscard]] static std::uint64_t empty_target(
		ExecutionMode mode) noexcept
	{
		return normalize_instruction_pointer(mode, ~std::uint64_t{});
	}

	[[nodiscard]] LinkRecord &link_record(Generation &state,
		std::uint64_t address, ExecutionMode mode)
	{
		return *state.links[mode_index(mode)]
			.try_emplace(address, address).first;
	}

	static void reset_indirect(IndirectLink &site, std::uint64_t empty,
		const BlockLink *empty_link) noexcept
	{
		for (auto &entry : site.entries) {
			entry.guest_rip = empty;
			entry.link = empty_link;
		}
		site.next_replacement = 0;
		site.listed = false;
	}

	[[nodiscard]] TargetLinkTable *target_table(
		Generation &state, ExecutionMode mode) noexcept
	{
		auto &owned = state.target_links[mode_index(mode)];
		if (!owned) {
			try {
				owned = std::make_unique<TargetLinkTable>(
					vacant_target_links());
			} catch (...) {
				return nullptr;
			}
		}
		return owned.get();
	}

	/* A code change somewhere in the address space says nothing about the
	 * blocks this cache holds: a fresh executable mapping, or a write to a
	 * page nothing has been translated from, overlaps none of them. Ask what
	 * actually changed and retire only the translations covering it, which is
	 * the same selective retirement this cache already performs for a write
	 * it made itself. Retiring everything stays the answer whenever the
	 * address space cannot describe the whole span -- the history is short,
	 * or the span is wider than one prepare should walk -- because a link
	 * left armed over changed bytes would run them.
	 */
	[[nodiscard]] bool retire_changed_code(Generation &state) noexcept
	{
		if (code_change_reader_ == nullptr || code_change_context_ == nullptr)
			return false;
		std::array<ProgramCodeChange, code_change_replay_limit> changes;
		std::uint64_t current = 0;
		const auto count = code_change_reader_(code_change_context_,
			state.changes, changes.data(), changes.size(), &current);
		if (count < 0)
			return false;
		std::uint64_t pages = 0;
		for (int index = 0; index < count; ++index) {
			pages += changes[index].length / DARWIN_UML_GUEST_PAGE_SIZE + 1U;
			if (pages > code_change_replay_pages)
				return false;
		}
		for (int index = 0; index < count; ++index)
			invalidate_code(changes[index].address,
				static_cast<std::size_t>(changes[index].length));
		return true;
	}

	void invalidate_links(Generation &state) noexcept
	{
		auto *record = state.live_links;
		state.live_links = nullptr;
		while (record != nullptr) {
			auto *next = record->next_live;
			record->reset();
			record->listed = false;
			record->next_live = nullptr;
			record = next;
		}
		for (std::size_t index = 0; index < state.live_indirect.size();
				++index) {
			auto &live = state.live_indirect[index];
			if (live.empty())
				continue;
			const auto mode = index == 0 ? ExecutionMode::long64 :
				ExecutionMode::compat32;
			const auto empty = empty_target(mode);
			const auto *found = state.links[index].find(empty);
			if (found == nullptr)
				continue;
			for (auto *site : live)
				reset_indirect(*site, empty, &found->link);
			live.clear();
		}
	}

	void activate_link(Slot &slot) noexcept
	{
		if (!slot.translation)
			return;
		if (slot.link == nullptr || slot.link_epoch != epoch_) {
			auto owner = generations_.find(slot.generation);
			if (owner == generations_.end())
				return;
			auto *found = owner->second.links[mode_index(slot.mode)]
				.find(slot.address);
			if (found == nullptr)
				return;
			slot.link = found;
			slot.link_epoch = epoch_;
		}
		auto &record = *slot.link;
		if (!record.listed) {
			record.listed = true;
			record.next_live = current().live_links;
			current().live_links = &record;
		}
		const auto &program = slot.translation->program;
		record.link.stream = program.native_stream().data();
		record.link.instruction_count = program.instruction_count();
		record.link.guest_rip = program.address();
		record.link.program_begin = program.address();
		record.link.program_end = program.address() + program.byte_count();
		/* Publishing here, rather than at a branch that asked for this
		 * address, is what lets a RET reach a return site it has never
		 * reached before: the block that runs there announces itself. */
		if (slot.generation != current_generation_)
			return;
		auto *table = target_table(current(), slot.mode);
		if (table != nullptr)
			(*table)[target_link_index(slot.address)] = {
				slot.address, &record.link};
	}

	void deactivate(Slot &slot) noexcept
	{
		if (slot.translation) {
			const auto *stream =
				slot.translation->program.native_stream().data();
			if (slot.link != nullptr && slot.link_epoch == epoch_) {
				if (slot.link->link.stream == stream)
					slot.link->reset();
			} else if (slot.link_epoch == epoch_) {
				auto owner = generations_.find(slot.generation);
				if (owner != generations_.end()) {
					auto *found = owner->second.links[
						mode_index(slot.mode)].find(slot.address);
					if (found != nullptr &&
						found->link.stream ==
							stream)
						found->reset();
				}
			}
		}
		slot.link = nullptr;
		slot.link_epoch = 0;
	}

	[[nodiscard]] static std::size_t records(const Generation &state) noexcept
	{
		return state.links[0].size() + state.links[1].size() +
			state.indirect[0].size() + state.indirect[1].size();
	}

	[[nodiscard]] static const LinkRecord *record_of(
		const BlockLink *link) noexcept
	{
		return reinterpret_cast<const LinkRecord *>(
			reinterpret_cast<std::uintptr_t>(link) -
			offsetof(LinkRecord, link));
	}

	/* Release the link records and indirect sites of one address space that
	 * nothing it holds names any more, as the arm64 cache does (G226).
	 *
	 * A resident translation's stream words name its successors' records and
	 * its indirect sites, and a resident block keeps its own record, which
	 * activation arms; every stream word is tried, operands too, which only
	 * ever keeps a record that nothing needed. Site cells and the target
	 * tables may still name a record no translation names, whose block is
	 * gone: those entries are emptied, as a code change empties them, and
	 * learn the target again if it runs. A transient program is never alive
	 * across a store, the only place a sweep runs.
	 */
	void sweep(Generation &state, std::uint64_t generation,
		ProgramCacheDiagnostics &diagnostics) noexcept
	{
#if UML_GADGET_CACHE_CENSUS
		const auto started = std::chrono::steady_clock::now();
		const auto links = diagnostics.released_links;
		const auto sites = diagnostics.released_sites;
#endif
		try {
			sweep_records(state, generation, diagnostics);
		} catch (const std::bad_alloc &) {
			/* A sweep only returns memory; without the memory to run
			 * one, the records stay. */
		}
		state.kept = records(state);
		state.departed = 0;
		++diagnostics.link_sweeps;
#if UML_GADGET_CACHE_CENSUS
		++sweeps_;
		swept_links_ += diagnostics.released_links - links;
		swept_sites_ += diagnostics.released_sites - sites;
		sweep_ns_ += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - started).count());
#endif
	}

	/* sweep(), with everything it allocates allocated before anything is
	 * released. */
	void sweep_records(Generation &state, std::uint64_t generation,
		ProgramCacheDiagnostics &diagnostics)
	{
		constexpr std::array modes{ExecutionMode::long64,
			ExecutionMode::compat32};
		std::array links{state.links[0].marks(), state.links[1].marks()};
		std::array sites{state.indirect[0].marks(),
			state.indirect[1].marks()};
		std::array<const LinkRecord *, 2> empty{};
		for (std::size_t mode = 0; mode < modes.size(); ++mode) {
			empty[mode] = state.links[mode].find(
				empty_target(modes[mode]));
			if (empty[mode] != nullptr)
				links[mode].mark(empty[mode]);
		}
		std::vector<std::uint64_t> seen((program_cache_slots + 63) / 64);
		std::vector<std::uint32_t> held;
		const auto visit = [&](std::uint32_t index) {
			const auto &slot = slot_at(index);
			if (slot.epoch != epoch_ || slot.generation != generation ||
			    !slot.translation ||
			    (seen[index / 64] >> (index % 64) & 1U) != 0)
				return;
			seen[index / 64] |= std::uint64_t{1} << (index % 64);
			held.push_back(index);
			const auto own = mode_index(slot.mode);
			if (slot.link != nullptr && slot.link_epoch == epoch_)
				links[own].mark(slot.link);
			else if (const auto *record =
					 state.links[own].find(slot.address))
				links[own].mark(record);
			for (const auto word :
			     slot.translation->program.native_stream())
				for (std::size_t mode = 0; mode < modes.size();
				     ++mode) {
					links[mode].mark(
						word - offsetof(LinkRecord, link));
					sites[mode].mark(
						word - offsetof(IndirectRecord, site));
				}
		};
		if (state.slots_complete) {
			for (const auto index : state.slots)
				visit(index);
		} else {
			for (std::size_t slab = 0; slab < program_cache_slab_count;
			     ++slab)
				if (slabs_[slab])
					for (std::size_t offset = 0;
					     offset < program_cache_slab_slots;
					     ++offset)
						visit(static_cast<std::uint32_t>(
							slab * program_cache_slab_slots +
							offset));
		}
		held.shrink_to_fit();
		std::array<std::vector<IndirectLink *>, 2> live_indirect;
		for (std::size_t mode = 0; mode < modes.size(); ++mode) {
			live_indirect[mode].reserve(state.live_indirect[mode].size());
			for (auto *site : state.live_indirect[mode])
				if (sites[mode].marked(
					    reinterpret_cast<const IndirectRecord *>(
						    reinterpret_cast<std::uintptr_t>(site) -
						    offsetof(IndirectRecord, site))))
					live_indirect[mode].push_back(site);
		}

		/* From here each step leaves the address space consistent. */
		for (std::size_t mode = 0; mode < modes.size(); ++mode) {
			diagnostics.released_sites +=
				state.indirect[mode].sweep(sites[mode]);
			state.live_indirect[mode] = std::move(live_indirect[mode]);
		}
		state.slots = std::move(held);
		state.slots_complete = true;
		const auto &vacant = vacant_target_links().front();
		for (std::size_t mode = 0; mode < modes.size(); ++mode) {
			if (empty[mode] != nullptr) {
				const IndirectLinkEntry cleared{
					empty_target(modes[mode]), &empty[mode]->link};
				state.indirect[mode].for_each(
					[&](IndirectRecord &record) {
						for (auto &entry : record.site.entries)
							if (!links[mode].marked(
								    record_of(entry.link)))
								entry = cleared;
					});
			}
			if (const auto &table = state.target_links[mode])
				for (auto &entry : *table)
					if (entry.link != vacant.link &&
					    !links[mode].marked(record_of(entry.link)))
						entry = vacant;
		}
		/* The live list runs through the records, so the ones about to
		 * go leave it first. */
		LinkRecord **next = &state.live_links;
		for (auto *record = state.live_links; record != nullptr;) {
			auto *following = record->next_live;
			if (links[0].marked(record) || links[1].marked(record)) {
				*next = record;
				next = &record->next_live;
			} else {
				record->listed = false;
				record->next_live = nullptr;
			}
			record = following;
		}
		*next = nullptr;
		for (std::size_t mode = 0; mode < modes.size(); ++mode)
			diagnostics.released_links +=
				state.links[mode].sweep(links[mode]);
	}

	void touch(std::uint64_t generation)
	{
		const auto found = std::find(
			resident_.begin(), resident_.end(), generation);
		if (found != resident_.end())
			resident_.erase(found);
		resident_.push_back(generation);
	}

	void retire(std::uint64_t generation) noexcept
	{
		const auto owner = generations_.find(generation);
		if (owner != generations_.end() && owner->second.slots_complete) {
			for (const auto index : owner->second.slots)
				release(slot_at(index), generation);
		} else {
			for (auto &slab : slabs_) {
				if (!slab)
					continue;
				for (auto &slot : slab->slots)
					release(slot, generation);
			}
		}
		if (owner != generations_.end()) {
			if (current_ == &owner->second)
				current_ = nullptr;
			if (registries_ != nullptr)
				for (const auto &[page, link] : owner->second.code_pages)
					registries_->of(generation).remove(page);
			generations_.erase(owner);
		}
		if (registries_ != nullptr)
			registries_->release(generation);
		const auto found = std::find(
			resident_.begin(), resident_.end(), generation);
		if (found != resident_.end())
			resident_.erase(found);
	}

	void release(Slot &slot, std::uint64_t generation) noexcept
	{
		if (slot.epoch != epoch_ || slot.generation != generation)
			return;
#if UML_GADGET_CACHE_CENSUS
		census_depart(slot, census::Departure::retired);
#endif
		discard(slot);
	}

	[[nodiscard]] Slot &slot_at(std::uint32_t index) noexcept
	{
		auto &slab = slabs_[index / program_cache_slab_slots];
		return slab->slots[index % program_cache_slab_slots];
	}

	[[nodiscard]] SuccessorSet &successors_at(std::uint32_t index) noexcept
	{
		auto &slab = slabs_[index / program_cache_slab_slots];
		return slab->successors[index % program_cache_slab_slots];
	}

	[[nodiscard]] Generation &current()
	{
		if (current_ == nullptr)
			current_ = &generations_
				.try_emplace(current_generation_).first->second;
		return *current_;
	}

	/* Count a resident translation leaving against its address space, whose
	 * next sweep waits for enough of them (gadget_cache::sweep_due). */
	void depart(const Slot &slot) noexcept
	{
		if (slot.epoch != epoch_ || !slot.translation)
			return;
		if (const auto owner = generations_.find(slot.generation);
		    owner != generations_.end())
			++owner->second.departed;
	}

	void discard(Slot &slot) noexcept
	{
		depart(slot);
		unindex(slot);
		deactivate(slot);
		slot.epoch = 0;
		slot.translation.reset();
		slot.validated_changes = 0;
		slot.terminal = false;
	}

	[[nodiscard]] bool validate(Slot &slot,
		std::uint64_t changes, std::uint64_t mapping_changes,
		::uml::memory::GuestAddressSpace &memory, void *execution_context,
		ProgramCacheSequenceReader sequence_reader,
		ProgramCacheDiagnostics &diagnostics)
	{
		if (!slot.translation ||
			slot.translation->program.source().empty() ||
			slot.translation->program.source().size() !=
				slot.translation->program.byte_count() ||
			slot.translation->program.source().size() >
				maximum_cached_source_bytes) {
			++diagnostics.validation_mismatches;
#if UML_GADGET_CACHE_CENSUS
			census_depart(slot, census::Departure::discarded);
#endif
			discard(slot);
			return false;
		}
		/* The program still matches the guest when every page stamp it
		 * recorded is unchanged, or else when its saved source still matches
		 * guest memory. The stamps are taken once, before a byte of source is
		 * read, and a successful comparison records those same values. A moved
		 * code-change sequence only brings this vCPU here and discards nothing
		 * by itself. The mapping sequence and an execution interrupt still
		 * abandon the validation, because no page stamp describes either
		 * ("Keeping a cached translation" in README.md).
		 */
		const auto memory_settled = [&]() noexcept {
			if (execution_context == nullptr || sequence_reader == nullptr)
				return true;
			const auto current = sequence_reader(execution_context);
			if (!current.interrupted &&
			    current.mapping_changes == mapping_changes)
				return true;
			++diagnostics.validation_sequence_races;
			return false;
		};

		auto &translation = *slot.translation;
		const auto current_stamps = capture_program_code_stamps(
			memory, translation.program, execution_context);
		if (translation.code_stamps.complete && current_stamps.complete &&
		    current_stamps == translation.code_stamps) {
			if (!memory_settled())
				return false;
			slot.validated_changes = changes;
			++diagnostics.validation_stamp_hits;
			++diagnostics.validation_hits;
			return true;
		}

		switch (compare_program_source(
			memory, translation.program,
			translation.program.source())) {
		case ProgramSourceComparison::fault:
			++diagnostics.validation_faults;
#if UML_GADGET_CACHE_CENSUS
			census_depart(slot, census::Departure::discarded);
#endif
			discard(slot);
			return false;
		case ProgramSourceComparison::mismatch:
			++diagnostics.validation_mismatches;
#if UML_GADGET_CACHE_CENSUS
			census_depart(slot, census::Departure::discarded);
#endif
			discard(slot);
			return false;
		case ProgramSourceComparison::match:
			break;
		}

		if (!memory_settled())
			return false;
		slot.validated_changes = changes;
		translation.code_stamps = current_stamps;
		++diagnostics.validation_hits;
		return true;
	}

	std::unordered_map<std::uint64_t, Generation> generations_;
	Generation *current_{};
	std::vector<std::uint64_t> resident_;
	std::uint64_t current_generation_{};
	std::array<std::unique_ptr<Slab>, program_cache_slab_count> slabs_{};
	std::array<std::uint8_t, program_cache_sets> next_replacement_{};
	SuccessorSet *preceding_{};
	std::uint64_t epoch_{1};
	NativeTlbEntry *write_translations_{};
	CodePageRegistries *registries_{};
	ProgramCacheCodeChangeReader code_change_reader_{};
	void *code_change_context_{};
#if UML_GADGET_CACHE_CENSUS
	census::History history_;
	census::Schedule schedule_;
	unsigned census_id_{census::next_cache_id()};
	/* Address-space switches, the clock dormancy is counted in. */
	std::uint64_t switches_{};
	/* Link sweeps, what they released, and the time they took. */
	std::uint64_t sweeps_{};
	std::uint64_t swept_links_{};
	std::uint64_t swept_sites_{};
	std::uint64_t sweep_ns_{};
#endif
};

#if UML_GADGET_CACHE_CENSUS
void dump_cache_census(const ProgramCache &programs) noexcept
{
	/* Appended, because every executor of the run reports its own cache and
	 * the union across them is the measurement. */
	census::append([&programs](std::FILE *out) {
		programs.census(out, 0);
	});
}
#endif

} // namespace

struct Executor::Storage final {
	Storage() = default;
	/* A move-assigned executor drops its old storage here, so the pages it
	 * counted on the shared list are subtracted here rather than in the
	 * executor's own destructor. */
	~Storage()
	{
#if UML_GADGET_CACHE_CENSUS
		/* Before the cache is emptied, and here rather than in the
		 * executor's own destructor so that a move-assigned executor
		 * reports the storage it drops. */
		dump_cache_census(programs);
#endif
		programs.invalidate();
		registries.drop_all();
	}
	Storage(const Storage &) = delete;
	Storage &operator=(const Storage &) = delete;

	std::array<NativeTlbEntry, tlb_entry_count> entries{};
	CodePageRegistries registries;
	ProgramCache programs;
	std::optional<Program> transient_program;
	::uml::memory::GuestAddressSpace *memory{};
	std::uint64_t generation{};
	std::uint64_t mapping_changes{};
	std::uint64_t code_changes{};
	void *execution_context{};
	ProgramCacheSequenceReader sequence_reader{};
	MappingChangeReader mapping_change_reader{};
	/* The mapping sequence every resident software-TLB entry is coherent
	 * for. A replay advances it to what the address space reported, which
	 * can be newer than the sequence the caller asked about. */
	std::uint64_t tlb_mapping_sequence{};
	ProgramCacheDiagnostics diagnostics{};
	bool prepared{};
	bool cache_ready{};
	unsigned checked{};
};

Executor::Executor() : storage_{std::make_unique<Storage>()}
{
	storage_->programs.set_write_translations(storage_->entries.data());
	storage_->registries.participant.write_translations =
		storage_->entries.data();
	storage_->programs.set_code_page_registries(&storage_->registries);
}
Executor::~Executor() = default;
Executor::Executor(Executor &&) noexcept = default;
Executor &Executor::operator=(Executor &&) noexcept = default;

namespace {

/* Retire the software-TLB entries covering one changed guest range. A write
 * entry carries its publication tags in the low bits of the page it holds, so
 * the compare masks them exactly as the engine's own hit path does. */
void retire_mapping_range(std::span<NativeTlbEntry> entries,
	std::uint64_t address, std::uint64_t length) noexcept
{
	constexpr std::uint64_t page_mask = ~std::uint64_t{0xfff};
	constexpr std::uint64_t write_tags =
		UML_X86_64_TLB_WRITE_CODE | UML_X86_64_TLB_WRITE_CACHED_CODE;
	const auto first = address & page_mask;
	const auto last = (address + length - 1) & page_mask;
	for (auto page = first;; page += 0x1000) {
		auto &entry = entries[native_tlb_index(page)];
		if ((entry.read_page & page_mask) == page)
			entry.read_page = NativeTlbEntry{}.read_page;
		if ((entry.write_page & ~write_tags & page_mask) == page)
			entry.write_page = NativeTlbEntry{}.write_page;
		if (page == last)
			break;
	}
}

/* Replaying a range list costs a lookup per page, so a span wider than a
 * fraction of the table is retired by clearing the table instead. */
constexpr std::uint64_t maximum_replayed_pages = tlb_entry_count / 4;

} // namespace

void Executor::prepare(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t generation, std::uint64_t mapping_changes,
	std::uint64_t code_changes, void *execution_context,
	ProgramCacheSequenceReader sequence_reader,
	ProgramCacheCodeChangeReader code_change_reader,
	MappingChangeReader mapping_change_reader) noexcept
{
	const bool memory_changed = storage_->memory != &memory;
	const bool mapping_changed = memory_changed || !storage_->prepared ||
		storage_->generation != generation ||
		storage_->mapping_changes != mapping_changes;
	const bool cache_binding_changed = memory_changed || !storage_->prepared ||
		storage_->generation != generation ||
		storage_->code_changes != code_changes;
	storage_->mapping_change_reader = mapping_change_reader;
	if (mapping_changed) {
		/* The address space hands a reader the ranges it has not seen, so a
		 * reader only a few changes behind retires exactly those pages
		 * instead of every entry it holds. Anything it cannot describe, and
		 * any move to another address space or generation, still clears the
		 * whole table. */
		bool replayed = false;
		if (!memory_changed && storage_->prepared &&
			storage_->generation == generation &&
			mapping_change_reader != nullptr) {
			std::array<ProgramMappingChange, 128> ranges{};
			std::uint64_t current = mapping_changes;
			const int count = mapping_change_reader(execution_context,
				generation, storage_->tlb_mapping_sequence, ranges.data(),
				ranges.size(), &current);
			if (count >= 0) {
				std::uint64_t pages = 0;
				for (int index = 0; index < count; ++index)
					pages += ranges[index].length / 0x1000 + 2;
				if (pages <= maximum_replayed_pages) {
					for (int index = 0; index < count; ++index)
						retire_mapping_range(storage_->entries,
							ranges[index].address, ranges[index].length);
					storage_->tlb_mapping_sequence = current;
					replayed = true;
				}
			}
		}
		if (!replayed) {
			std::ranges::fill(storage_->entries, NativeTlbEntry{});
			storage_->tlb_mapping_sequence = mapping_changes;
		}
	}
	if (memory_changed)
		storage_->programs.invalidate();
	if (memory_changed || !storage_->prepared ||
	    storage_->generation != generation) {
		try {
			storage_->registries.bind(generation);
		} catch (...) {
			/* Without a registry every executable store publishes, which
			 * is the conservative contract this replaces. */
			storage_->registries.unbind();
		}
	}
	storage_->memory = &memory;
	storage_->generation = generation;
	storage_->mapping_changes = mapping_changes;
	storage_->code_changes = code_changes;
	storage_->execution_context = execution_context;
	storage_->sequence_reader = sequence_reader;
	storage_->programs.set_code_change_reader(
		code_change_reader, execution_context);
	storage_->prepared = true;
	if (!cache_binding_changed && storage_->cache_ready)
		return;
	try {
		storage_->programs.prepare(generation, code_changes);
		storage_->cache_ready = true;
	} catch (...) {
		storage_->programs.invalidate();
		storage_->cache_ready = false;
	}
}

NativeTlbEntry *Executor::tlb_entries() noexcept
{
	return storage_->entries.data();
}

bool Executor::page_holds_cached_code(std::uint64_t address) noexcept
{
	return storage_->prepared && storage_->cache_ready &&
		storage_->programs.page_holds_code(address);
}

CodePageRegistry *Executor::code_page_registry() const noexcept
{
	return storage_->registries.current.get();
}

void Executor::enter_engine() noexcept
{
	storage_->registries.participant.engine_epoch.fetch_add(
		1, std::memory_order_seq_cst);
}

void Executor::leave_engine() noexcept
{
	storage_->registries.participant.engine_epoch.fetch_add(
		1, std::memory_order_release);
}

void Executor::check_exceptions(unsigned checked) noexcept
{
	storage_->checked = checked;
}

unsigned Executor::checked_exceptions() const noexcept
{
	return storage_->checked;
}

void *Executor::execution_context() const noexcept
{
	return storage_->execution_context;
}

std::uint64_t Executor::prepared_code_changes() const noexcept
{
	return storage_->code_changes;
}

std::uint64_t Executor::current_code_changes() const noexcept
{
	if (storage_->execution_context == nullptr ||
	    storage_->sequence_reader == nullptr)
		return storage_->code_changes;
	return storage_->sequence_reader(
		storage_->execution_context).code_changes;
}

void Executor::invalidate_cached_code(
	std::uint64_t address, std::size_t size) noexcept
{
	if (storage_->prepared && storage_->cache_ready)
		storage_->programs.invalidate_code(address, size);
}

bool Executor::accept_code_changes(std::uint64_t code_changes) noexcept
{
	if (!storage_->prepared || !storage_->cache_ready)
		return false;
	storage_->programs.accept_code_changes(code_changes);
	storage_->code_changes = code_changes;
	return true;
}

const TargetLinkEntry *Executor::target_links(ExecutionMode mode) noexcept
{
	return storage_->programs.target_links(mode);
}

ProgramSelection Executor::select_program(
	::uml::memory::GuestAddressSpace &memory, std::uint64_t address,
	ExecutionMode mode, std::size_t maximum_instructions) noexcept
{
	if (!storage_->prepared || storage_->memory != &memory)
		prepare(memory);
	storage_->transient_program.reset();
	address = normalize_instruction_pointer(mode, address);
	if (storage_->cache_ready) {
		bool cache_hit = false;
		if (const auto *program = storage_->programs.find(address, mode,
			maximum_instructions, storage_->code_changes,
			storage_->mapping_changes, memory, storage_->execution_context,
			storage_->sequence_reader, storage_->diagnostics, cache_hit);
			program != nullptr) {
			return {
				.status = BuildStatus::built,
				.program = program,
				.fault_address = 0,
				.cache_hit = cache_hit,
			};
		}
	}

	++storage_->diagnostics.translations;
	ProgramLinkResolver links{};
	if (storage_->cache_ready) {
		links = {
			.context = &storage_->programs,
			.resolve = [](void *context, std::uint64_t target,
				ExecutionMode target_mode) {
				return static_cast<ProgramCache *>(context)->link(
					target, target_mode);
			},
			.resolve_indirect = [](void *context, std::uint64_t source,
				ExecutionMode source_mode) {
				return static_cast<ProgramCache *>(context)->indirect_link(
					source, source_mode);
			},
		};
	}
	const CodePageHold hold{
		storage_->cache_ready ? &storage_->registries : nullptr, address,
		mode};
	/* Before the decoder reads a byte, and after the hold, so a store another
	 * executor has already translated for one of these pages either lands
	 * ahead of the read or moves the stamp this reads. */
	const auto held_stamps = capture_held_code_stamps(
		memory, hold.pages(), storage_->execution_context);
	auto built = build(memory, address, mode, maximum_instructions, links,
		storage_->checked);
	if (built.status != BuildStatus::built) {
		storage_->programs.cancel_selection();
		return {
			.status = built.status,
			.program = nullptr,
			.fault_address = built.fault_address,
			.cache_hit = false,
		};
	}

	const auto use_transient = [&]() noexcept {
		storage_->programs.cancel_selection();
		storage_->transient_program.emplace(std::move(built.program));
		return ProgramSelection{
			.status = BuildStatus::built,
			.program = &*storage_->transient_program,
			.fault_address = 0,
			.cache_hit = false,
		};
	};
	if (!storage_->cache_ready || built.program.byte_count() == 0 ||
		built.program.byte_count() > maximum_cached_source_bytes)
		return use_transient();
	const auto code_stamps = program_code_stamps_from_hold(
		built.program, hold.pages(), held_stamps);
	if (!code_stamps)
		return use_transient();

	try {
		const auto &stored = storage_->programs.store(
			std::move(built.program),
			storage_->code_changes, maximum_instructions, *code_stamps,
			storage_->diagnostics);
		return {
			.status = BuildStatus::built,
			.program = &stored,
			.fault_address = 0,
			.cache_hit = false,
		};
	} catch (...) {
		return use_transient();
	}
}

const Program *Executor::program_holding(std::uint64_t address,
	ExecutionMode mode, const std::uintptr_t *word) const noexcept
{
	if (const auto &transient = storage_->transient_program; transient) {
		const auto stream = transient->native_stream();
		if (word >= stream.data() && word < stream.data() + stream.size())
			return &*transient;
	}
	return storage_->programs.holding(
		normalize_instruction_pointer(mode, address), mode, word);
}

void Executor::train_indirect(IndirectLink *site, std::uint64_t target,
	ExecutionMode mode) noexcept
{
	if (site == nullptr || !storage_->cache_ready)
		return;
	try {
		storage_->programs.learn_indirect(*site, target, mode);
	} catch (...) {
		/* Training is optional. Allocation pressure must preserve the cold
		 * indirect exit rather than changing architectural execution. */
	}
}

const ProgramCacheDiagnostics &Executor::cache_diagnostics() const noexcept
{
	return storage_->diagnostics;
}

std::size_t Executor::resident_generations() const noexcept
{
	return storage_->programs.resident_generations();
}

} // namespace uml::cpu::x86_64::gadgets
