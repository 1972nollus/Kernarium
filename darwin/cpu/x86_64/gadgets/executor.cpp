// SPDX-License-Identifier: GPL-2.0-or-later
#include "executor.hpp"

#include "cpu/gadget_cache_geometry.hpp"
#include "cpu/gadget_record_table.hpp"

#include "code_page_registry.hpp"
#include "native_entry.hpp"
#include "native_symbols.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#if UML_GADGET_CACHE_CENSUS
#include <cstdio>
#include <cstdlib>
#endif
#include <cstring>
#include <memory>
#if UML_GADGET_CACHE_CENSUS
#include <mutex>
#endif
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uml::cpu::x86_64::gadgets {
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
 * decoder ran, for the pages the program turned out to cover.
 *
 * That read is a lower bound on the program only because the program and the
 * source the slot saves are one observation of guest memory: the decoder's
 * bytes are what the slot keeps. Fetching the block a second time to fill the
 * source instead left the two reads free to disagree, and nothing downstream
 * could tell -- a write between them made the source newer than the program,
 * so a later comparison of that source against memory matched and accepted a
 * program decoded from bytes that were already gone. Re-reading the page stamp
 * after the second read only narrowed that window, because a writer stores its
 * bytes well before it moves the stamp.
 *
 * With one read the ordinary validation decides everything. A publication that
 * touched the block moved the page's stamp -- after storing the bytes, and
 * before moving the sequence that makes this vCPU revalidate -- so the
 * comparison that follows sees the decoder's own bytes against guest memory
 * and says exactly whether the translation is stale. A store that has landed
 * but not yet published is missed here as it always was, and reaches this vCPU
 * at its next code-change checkpoint, which is where the x86
 * cross-modifying-code protocol requires an executing processor to look.
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
		/* Which of `page_links` are in a page's list. */
		std::uint8_t linked_pages{};
		std::uint64_t validated_changes{};
		LinkRecord *link{};
		std::uint64_t link_epoch{};
		std::array<PageLink, 2> page_links{};
		ProgramCodeStamps code_stamps{};
		std::optional<Program> program;
	};
	static_assert(sizeof(Slot) == 184,
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
	 * Its records are named by address from translated streams and released
	 * only with it, so they live in chunks rather than map nodes (G210).
	 */
	struct Generation final {
		std::uint64_t changes{};
		std::array<gadget_cache::StableRecordTable<LinkRecord, LinkKey>, 2>
			links;
		std::array<gadget_cache::StableRecordTable<IndirectRecord,
				   IndirectKey>, 2>
			indirect;
		LinkRecord *live_links{};
		std::array<std::unique_ptr<TargetLinkTable>, 2> target_links;
		std::array<std::vector<IndirectLink *>, 2> live_indirect;
		std::vector<std::uint32_t> slots;
		/* The first link of each page's list. */
		std::unordered_map<std::uint64_t, std::uint32_t> code_pages;
		bool slots_complete{true};
	};

public:
	void invalidate() noexcept
	{
		if (registries_ != nullptr)
			for (auto &[generation, state] : generations_)
				for (const auto &[page, link] : state.code_pages)
					registries_->of(generation).remove(page);
		resident_.clear();
		generations_.clear();
		current_ = nullptr;
		for (auto &slab : slabs_)
			if (slab)
				for (auto &slot : slab->slots) {
					slot.page_links = {};
					slot.linked_pages = 0;
				}
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
					    slot.program && source_overlaps(
						    *slot.program, address, size))
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
		if (!slab)
			return nullptr;
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
		return nullptr;
	}

	void cancel_selection() noexcept { preceding_ = nullptr; }

	[[nodiscard]] const Program &store(Program &&program,
		std::uint64_t changes,
		std::size_t instruction_limit, ProgramCodeStamps code_stamps)
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
		unindex(slot);
		deactivate(slot);
		slot.epoch = 0;
		successors = {};
		slot.program.emplace(std::move(program));
		slot.generation = current_generation_;
		slot.address = slot.program->address();
		slot.validated_changes = changes;
		slot.code_stamps = code_stamps;
		slot.instruction_limit = kept_instruction_limit(instruction_limit);
		slot.mode = slot.program->execution_mode();
		slot.terminal = slot.program->terminal();
		/* Epoch is the validity marker and is written only after the slot is
		 * complete. */
		slot.epoch = epoch_;
		try {
			index(slot);
		} catch (...) {
			slot.epoch = 0;
			slot.program.reset();
			throw;
		}
		activate_link(slot);
		activate(slot, successors);
		return *slot.program;
	}

	[[nodiscard]] std::size_t resident_generations() const noexcept
	{
		return generations_.size();
	}

#if UML_GADGET_CACHE_CENSUS
	/* What this cache costs, in bytes it actually holds rather than in the
	 * capacity it was configured with. The slab array is the only part that
	 * exists whether or not its ways are occupied; everything else is
	 * proportional to what the workload translated. */
	struct Footprint final {
		std::size_t slabs, streams, decoded, links, indirect, targets;
		[[nodiscard]] std::size_t total() const noexcept
		{
			return slabs + streams + decoded + links + indirect +
				targets;
		}
	};

	[[nodiscard]] Footprint footprint() const noexcept
	{
		std::size_t slabs = 0;
		std::size_t streams = 0;
		std::size_t decoded = 0;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			slabs += sizeof(Slab);
			for (const auto &slot : slab->slots) {
				if (!resident(slot))
					continue;
				/* A translation is the native stream and the
				 * saved source; a block some replay has ended
				 * inside also holds what that replay
				 * decoded. */
				const auto &program = *slot.program;
				streams += program.native_stream().size_bytes() +
					program.source().size();
				decoded += program.replay_bytes();
			}
		}
		std::size_t links = 0;
		std::size_t indirect = 0;
		std::size_t targets = 0;
		for (const auto &[generation, state] : generations_) {
			for (const auto &mode : state.links)
				links += mode.bytes();
			links += state.slots.capacity() *
					sizeof(std::uint32_t) +
				map_bytes(state.code_pages);
			for (const auto &mode : state.indirect)
				indirect += mode.bytes();
			for (const auto &live : state.live_indirect)
				indirect += live.capacity() *
					sizeof(IndirectLink *);
			for (const auto &table : state.target_links)
				if (table)
					targets += sizeof(TargetLinkTable);
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

	/* Write what this cache holds, so duplication can be counted instead of
	 * estimated. The record format is the arm64 executor's, so one analyzer
	 * reads either engine's capture.
	 *
	 * A translation is identified by the guest bytes it consumed, not by the
	 * stream it produced: two executors translating the same code emit the
	 * same gadgets but embed pointers to their own link records, so only the
	 * source identifies the work. The stream length goes out with it because
	 * it decides what a body costs.
	 */
	void census(std::FILE *out) const noexcept
	{
		const auto parts = footprint();
		/* sizeof(Slot) is reported because the slab total covers the
		 * successor sets as well as the tags, and only the slot size
		 * separates them. */
		std::fprintf(out,
			"E %p generations=%zu occupancy=%zu slot_bytes=%zu "
			"slabs=%zu streams=%zu decoded=%zu links=%zu "
			"indirect=%zu targets=%zu total=%zu\n",
			static_cast<const void *>(this), resident_generations(),
			occupancy(), sizeof(Slot), parts.slabs, parts.streams,
			parts.decoded, parts.links, parts.indirect,
			parts.targets, parts.total());
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots) {
				if (!resident(slot))
					continue;
				const auto &program = *slot.program;
				const auto source = program.source();
				/* The same bytes decoded in the other
				 * execution mode are a different translation,
				 * so the mode seeds the digest. */
				std::uint64_t digest = 0xcbf29ce484222325ULL ^
					mode_index(slot.mode);
				for (const auto byte : source)
					digest = (digest ^
							 static_cast<
								 std::uint8_t>(
								 byte)) *
						0x100000001b3ULL;
				std::fprintf(out,
					"P %llu 0x%llx %zu %zu %zu 0x%llx\n",
					static_cast<unsigned long long>(
						slot.generation),
					static_cast<unsigned long long>(
						slot.address),
					program.instruction_count(),
					program.native_stream().size(),
					source.size(),
					static_cast<unsigned long long>(
						digest));
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
		return slot.epoch == epoch_ && slot.program.has_value();
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
		if (!slot.program)
			return;
		const auto pages = program_pages(*slot.program);
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
				auto &link = slot.page_links[completed];
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
		return page_link_slot(id).page_links[(id - 1U) % 2U];
	}

	/* Take one of a slot's links out of its page's list. The slot must
	 * still hold the program the link was made for, which names the page. */
	void unlink(std::uint64_t generation, Generation &state, Slot &slot,
		std::size_t entry) noexcept
	{
		const auto bit = static_cast<std::uint8_t>(1U << entry);
		if ((slot.linked_pages & bit) == 0)
			return;
		auto &link = slot.page_links[entry];
		if (link.previous != 0) {
			page_link(link.previous).next = link.next;
		} else {
			const auto page = program_pages(*slot.program).pages[entry];
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
		auto owner = generations_.find(slot.generation);
		if (owner == generations_.end()) {
			slot.page_links = {};
			slot.linked_pages = 0;
			return;
		}
		for (std::size_t entry = 0; entry < slot.page_links.size(); ++entry)
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
		if (!slot.program) {
			discard(slot);
			return nullptr;
		}
		if (!slot.terminal && kept_instruction_limit(maximum_instructions) >
			slot.instruction_limit) {
			discard(slot);
			return nullptr;
		}
		if (slot.validated_changes != changes) {
			++diagnostics.validation_attempts;
			if (!validate(slot, changes, mapping_changes, memory,
				execution_context, sequence_reader, diagnostics))
				return nullptr;
		}
		/* Whatever reset this record -- a replaced slot, or a code change
		 * that cooled the whole address space -- the program in this slot is
		 * valid for the current sequence by the time it is returned, so it is
		 * a legitimate link target again. Re-arming here is what lets a chain
		 * rebuild as blocks run, rather than only when one needs revalidating.
		 */
		activate_link(slot);
		return &*slot.program;
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
		if (!slot.program)
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
		const auto &program = *slot.program;
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
		if (slot.program) {
			if (slot.link != nullptr && slot.link_epoch == epoch_) {
				if (slot.link->link.stream ==
					slot.program->native_stream().data())
					slot.link->reset();
			} else if (slot.link_epoch == epoch_) {
				auto owner = generations_.find(slot.generation);
				if (owner != generations_.end()) {
					auto *found = owner->second.links[
						mode_index(slot.mode)].find(slot.address);
					if (found != nullptr &&
						found->link.stream ==
							slot.program->native_stream().data())
						found->reset();
				}
			}
		}
		slot.link = nullptr;
		slot.link_epoch = 0;
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
		if (slot.epoch != epoch_ || !slot.program ||
			slot.generation != generation)
			return;
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

	void discard(Slot &slot) noexcept
	{
		unindex(slot);
		deactivate(slot);
		slot.epoch = 0;
		slot.program.reset();
		slot.validated_changes = 0;
		slot.code_stamps = {};
		slot.terminal = false;
	}

	[[nodiscard]] bool validate(Slot &slot,
		std::uint64_t changes, std::uint64_t mapping_changes,
		::uml::memory::GuestAddressSpace &memory, void *execution_context,
		ProgramCacheSequenceReader sequence_reader,
		ProgramCacheDiagnostics &diagnostics)
	{
		if (!slot.program || slot.program->source().empty() ||
			slot.program->source().size() !=
				slot.program->byte_count() ||
			slot.program->source().size() >
				maximum_cached_source_bytes) {
			++diagnostics.validation_mismatches;
			discard(slot);
			return false;
		}
		/* Two things say a cached program still matches the guest: the stamp
		 * of every page it covers, which any publication of a code change on
		 * that page moves, and the saved source bytes themselves. Their order
		 * is what makes either of them mean anything. The stamp is taken once,
		 * before a byte of source is read, and that same value is what a
		 * successful validation records; taking it afterwards recorded a stamp
		 * newer than the source it stands for, so the next validation could
		 * accept that source on a stamp which already covered a write to it.
		 *
		 * With that order the code-change sequence has nothing left to guard.
		 * It used to be re-read after the comparison and the whole program
		 * thrown away when it had moved, which is what made one vCPU
		 * publishing at speed -- a JIT heap, or a trampoline sharing a page
		 * with hot data -- cost every other vCPU of the address space a fresh
		 * translation of nearly every block it ran. A publication this reader
		 * has not seen is not a program it must rebuild: a publication that
		 * touched this program moved the stamp of the page carrying it, and
		 * one that did not is no business of this program.
		 *
		 * What accepting here guarantees is what it guaranteed before: the
		 * program matched guest memory at an instant inside this call, and
		 * every change published up to the sequence this cache was prepared
		 * with had landed before that instant, because the writer stores the
		 * bytes, then moves the page stamp, then moves the sequence, and this
		 * vCPU read that sequence before it looked. A store still in flight --
		 * landed but not yet stamped, or not yet issued -- was outside that
		 * instant under the old test exactly as it is under this one, and
		 * reaches this vCPU at its next code-change checkpoint, which is where
		 * the x86 cross-modifying-code protocol requires an executing
		 * processor to look.
		 *
		 * The mapping sequence and an execution interrupt still abandon the
		 * validation. Those say the memory this call reads through is being
		 * replaced, or that this vCPU has been asked to leave, and no page
		 * stamp describes either.
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

		const auto current_stamps = capture_program_code_stamps(
			memory, *slot.program, execution_context);
		if (slot.code_stamps.complete && current_stamps.complete &&
		    current_stamps == slot.code_stamps) {
			if (!memory_settled())
				return false;
			slot.validated_changes = changes;
			++diagnostics.validation_stamp_hits;
			++diagnostics.validation_hits;
			return true;
		}

		switch (compare_program_source(
			memory, *slot.program, slot.program->source())) {
		case ProgramSourceComparison::fault:
			++diagnostics.validation_faults;
			discard(slot);
			return false;
		case ProgramSourceComparison::mismatch:
			++diagnostics.validation_mismatches;
			discard(slot);
			return false;
		case ProgramSourceComparison::match:
			break;
		}

		if (!memory_settled())
			return false;
		slot.validated_changes = changes;
		slot.code_stamps = current_stamps;
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
};

#if UML_GADGET_CACHE_CENSUS
void dump_cache_census(const ProgramCache &programs) noexcept
{
	const char *path = std::getenv("DARWIN_UML_GADGET_CACHE_CENSUS");
	if (path == nullptr || *path == '\0')
		return;
	/* Appended, because every executor of the run reports its own cache and
	 * the union across them is the measurement. */
	static std::mutex census_mutex;
	const std::lock_guard guard{census_mutex};
	if (std::FILE *out = std::fopen(path, "ae"); out != nullptr) {
		programs.census(out);
		std::fclose(out);
	}
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
	bool unaligned_exclusives{};
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

void Executor::permit_unaligned_exclusives(bool value) noexcept
{
	storage_->unaligned_exclusives = value;
}

bool Executor::unaligned_exclusives() const noexcept
{
	return storage_->unaligned_exclusives;
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
	auto built = build(memory, address, mode, maximum_instructions, links);
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
			storage_->code_changes, maximum_instructions, *code_stamps);
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
