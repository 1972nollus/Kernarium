// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "native_symbols.hpp"
#include "program.hpp"
#include "translation_body.hpp"

#include "cpu/gadget_body_arena.hpp"
#include "cpu/gadget_cache_budget.hpp"
#include "cpu/gadget_cache_geometry.hpp"
#include "cpu/gadget_record_table.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <utility>
#include <vector>

namespace uml::cpu::x86_64::gadgets {

[[nodiscard]] constexpr std::size_t mode_index(ExecutionMode mode) noexcept
{
	return mode == ExecutionMode::compat32 ? 1U : 0U;
}

/* The unreachable guest address an untrained indirect cell and an empty
 * target-table entry name. */
[[nodiscard]] constexpr std::uint64_t empty_target(ExecutionMode mode) noexcept
{
	return normalize_instruction_pointer(mode, ~std::uint64_t{});
}

/* A target table nothing has been written to: every entry names an address no
 * block begins at and carries that address's own fallback, so the translated
 * tier can load the pointer before it has compared the address, and a guest
 * that really does branch there still leaves with the right RIP. */
[[nodiscard]] const TargetLinkTable &vacant_target_links() noexcept;

/* Laid out as an UnlinkedBlockLink, so an unlinked record's stream is its own
 * link_exit word. */
struct alignas(16) LinkRecord final {
	explicit LinkRecord(std::uint64_t address) noexcept
		: unlinked_begin{address}, unlinked_end{address},
		  fallback_stream{reinterpret_cast<std::uintptr_t>(
			native::uml_x86_64_gadget_link_exit)},
		  link{&fallback_stream, 0, address}
	{}

	void reset() noexcept
	{
		link.stream = &fallback_stream;
		link.instruction_count = 0;
	}

	LinkRecord *next_live{};
	std::uint64_t unlinked_begin;
	std::uint64_t unlinked_end;
	std::uintptr_t fallback_stream;
	BlockLink link;
	bool listed{};
};
static_assert(offsetof(LinkRecord, unlinked_begin) ==
		offsetof(UnlinkedBlockLink, unlinked_begin) &&
	offsetof(LinkRecord, fallback_stream) ==
		offsetof(UnlinkedBlockLink, fallback_stream) &&
	offsetof(LinkRecord, link) == offsetof(UnlinkedBlockLink, link) &&
	sizeof(LinkRecord) == 64,
	"a link record is laid out as the unlinked state link_exit and "
	"follow_block_link read");

struct LinkKey final {
	[[nodiscard]] std::uint64_t operator()(
		const LinkRecord &record) const noexcept
	{
		return record.link.guest_rip;
	}
};

/* An indirect site is named by the branch that owns it, which its own cells do
 * not record. Translated streams point at the site, the first member. */
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

/* One address space's translations: their bodies, in two arenas, and the link
 * records, indirect sites and target tables their streams name.
 *
 * It is what an executor keeps for each address space it runs, and what it
 * charges to its own account in the process's gadget-cache budget
 * (gadget_cache_budget.hpp), which may ask the executor to let go of it. The
 * records live in chunks, so a record stays where translated code names it
 * until a sweep releases the ones nothing names any more; a sweep reads the
 * executor's slots, so it is the executor's to ask for.
 *
 * Bodies are allocated and freed here and nothing else: which of them are
 * indexed by page and which a slot holds are the executor's.
 */
class Space final {
public:
	using LinkTable = gadget_cache::StableRecordTable<LinkRecord, LinkKey>;
	using SiteTable =
		gadget_cache::StableRecordTable<IndirectRecord, IndirectKey>;
	using LinkMarks = LinkTable::Marks;
	using SiteMarks = SiteTable::Marks;

	/* `notify` is called, under the budget's lock, when the budget asks this
	 * address space to shed. */
	Space(gadget_cache::Account::Notify notify, void *owner) noexcept
		: account_{notify, owner}
	{
		streams_.meter(&account_);
		sources_.meter(&account_);
		for (auto &mode : links_)
			mode.meter(&account_);
		for (auto &mode : indirect_)
			mode.meter(&account_);
	}

	Space(const Space &) = delete;
	Space &operator=(const Space &) = delete;

	[[nodiscard]] gadget_cache::Account &account() noexcept
	{
		return account_;
	}
	[[nodiscard]] const gadget_cache::Account &account() const noexcept
	{
		return account_;
	}

	/* A body for `program`: its stream behind the body, in the stream arena,
	 * and its saved source apart. Nothing else is touched, so a failure
	 * leaves the space as it was. */
	[[nodiscard]] Body *make_body(const Program &program,
		std::uint8_t instruction_limit, const ProgramCodeStamps &stamps);
	/* Give a body that is in no list and that nothing holds back. What a
	 * replay decoded is the only thing it frees itself. */
	void free_body(Body *body) noexcept;

	/* The record for `address`, made on first use. */
	[[nodiscard]] LinkRecord &link_record(
		std::uint64_t address, ExecutionMode mode)
	{
		return *links_[mode_index(mode)]
			.try_emplace(address, address).first;
	}
	[[nodiscard]] LinkRecord *find_link(
		std::uint64_t address, ExecutionMode mode) noexcept
	{
		return links_[mode_index(mode)].find(address);
	}
	[[nodiscard]] const BlockLink *link(
		std::uint64_t address, ExecutionMode mode)
	{
		return &link_record(address, mode).link;
	}

	/* The site of the branch at `source`, made on first use, with its cells
	 * empty. */
	[[nodiscard]] IndirectLink *indirect_link(
		std::uint64_t source, ExecutionMode mode);
	/* Teach `site` that its branch reached `target`. */
	void learn_indirect(
		IndirectLink &site, std::uint64_t target, ExecutionMode mode);

	/* The table an entering block probes, made on first use, or the vacant
	 * one when it cannot be made. It belongs to the space, so no entry can
	 * name a record that has been destroyed. */
	[[nodiscard]] const TargetLinkEntry *target_links(
		ExecutionMode mode) noexcept
	{
		const auto *table = target_table(mode);
		return table == nullptr ? vacant_target_links().data() :
			table->data();
	}
	/* The table, made on first use; null when it cannot be made. */
	[[nodiscard]] TargetLinkTable *target_table(ExecutionMode mode) noexcept;

	/* Point `record` at `body`'s block, and list it so a code change can
	 * reset it. */
	void arm(LinkRecord &record, const Body &body) noexcept;
	/* Announce `record`'s block in the target table of its mode. */
	void announce(const LinkRecord &record, ExecutionMode mode) noexcept;
	/* Reset the record of `body`'s block if it names the body's stream. */
	void reset_link_of(const Body &body) noexcept;
	/* Reset every record and site that holds an activation, as a code
	 * change that cannot be described does. */
	void invalidate_links() noexcept;

	/* Records and sites, and what the last sweep kept and the bodies that
	 * have left since: a sweep waits for enough of them
	 * (gadget_cache::sweep_due). */
	[[nodiscard]] std::size_t records() const noexcept
	{
		return links_[0].size() + links_[1].size() + indirect_[0].size() +
			indirect_[1].size();
	}
	[[nodiscard]] bool sweep_due() const noexcept;
	void note_departed() noexcept { ++departed_; }

	/* The marks a sweep starts from: every record and site, the empty
	 * records already kept. */
	[[nodiscard]] std::array<LinkMarks, 2> link_marks() const;
	[[nodiscard]] std::array<SiteMarks, 2> site_marks() const;

	struct Released final {
		std::size_t links;
		std::size_t sites;
	};

	/* Release every record and site the marks leave. A site cell or target
	 * table entry may still name such a record, whose block is gone; it is
	 * emptied as a code change empties it, to learn its target again if
	 * that runs. Everything this allocates comes before anything is
	 * released, and each step after leaves the space consistent, so a
	 * sweep that runs out of memory throws before it changes anything. */
	[[nodiscard]] Released sweep(const std::array<LinkMarks, 2> &links,
		const std::array<SiteMarks, 2> &sites);

	/* A sweep, whole or not, has run: the next waits for growth and
	 * departures from here. */
	void swept() noexcept;

	/* The record a link belongs to. */
	[[nodiscard]] static const LinkRecord *record_of(
		const BlockLink *link) noexcept
	{
		return reinterpret_cast<const LinkRecord *>(
			reinterpret_cast<std::uintptr_t>(link) -
			offsetof(LinkRecord, link));
	}

#if UML_GADGET_CACHE_CENSUS
	/* What the space keeps besides its bodies, in bytes. */
	struct LinkBytes final {
		std::size_t links, indirect, targets;
	};
	[[nodiscard]] LinkBytes link_bytes() const noexcept;
	[[nodiscard]] std::size_t link_records() const noexcept
	{
		return links_[0].size() + links_[1].size();
	}
	template <typename Visit>
	void for_each_link(Visit &&visit) const
	{
		for (const auto &mode : links_)
			mode.for_each(visit);
	}
	template <typename Visit>
	void for_each_indirect(Visit &&visit) const
	{
		for (const auto &mode : indirect_)
			mode.for_each(visit);
	}
#endif

private:
	static void reset_indirect(IndirectLink &site, std::uint64_t empty,
		const BlockLink *empty_link) noexcept;
	std::array<LinkTable, 2> links_;
	std::array<SiteTable, 2> indirect_;
	std::array<std::unique_ptr<TargetLinkTable>, 2> targets_;
	/* The records and sites holding an activation. */
	LinkRecord *live_links_{};
	std::array<std::vector<IndirectLink *>, 2> live_indirect_;
	std::size_t kept_{};
	std::size_t departed_{};
	/* The translations of the address space: each body with its stream, and
	 * apart from them the source each was read from, which is cold. A body
	 * gives no heap allocation back but what a replay decoded, so dropping
	 * the arenas drops the bodies. Both are charged to the account as they
	 * grow and shrink. */
	gadget_cache::BodyArena streams_{std::pmr::new_delete_resource()};
	gadget_cache::BodyArena sources_{std::pmr::new_delete_resource()};
	/* Last, so it is destroyed first: a call of shed() in progress holds up
	 * its destruction, and everything shed() touches is still there. */
	gadget_cache::Account account_;
};

} // namespace uml::cpu::x86_64::gadgets
