// SPDX-License-Identifier: GPL-2.0-or-later
#include "translation_space.hpp"

#include <algorithm>
#include <cstring>
#include <new>

#include <malloc/malloc.h>

namespace uml::cpu::x86_64::gadgets {

const TargetLinkTable &vacant_target_links() noexcept
{
	static const UnlinkedBlockLink vacant{
		0, ~std::uint64_t{}, ~std::uint64_t{},
		reinterpret_cast<std::uintptr_t>(
			native::uml_x86_64_gadget_link_exit),
		{&vacant.fallback_stream, 0, ~std::uint64_t{}},
	};
	static const TargetLinkTable table = [] {
		TargetLinkTable initial{};
		for (auto &entry : initial)
			entry = {~std::uint64_t{}, &vacant.link};
		return initial;
	}();
	return table;
}

Body *Space::make_body(const Program &program,
	std::uint8_t instruction_limit, const ProgramCodeStamps &stamps)
{
	const auto bytes =
		Body::allocation_bytes(program.native_stream().size());
	auto *storage = static_cast<std::byte *>(streams_.allocate(bytes));
	std::byte *source = nullptr;
	try {
		if (program.byte_count() != 0)
			source = static_cast<std::byte *>(
				sources_.allocate(program.byte_count()));
	} catch (...) {
		streams_.deallocate(storage, bytes);
		throw;
	}
	auto *body = new (storage) Body{program.view_in(
		reinterpret_cast<std::uintptr_t *>(storage + sizeof(Body)),
		source)};
	body->instruction_limit = instruction_limit;
	body->code_pages = stamps.pages;
	body->stamp_state = static_cast<std::uint8_t>(stamps.count |
		(stamps.complete ? Body::stamps_complete_bit : 0U));
	return body;
}

void Space::free_body(Body *body) noexcept
{
	const auto bytes = body->allocation_bytes();
	auto *source = const_cast<std::byte *>(body->program.source().data());
	const auto source_bytes = body->program.byte_count();
	std::destroy_at(body);
	if (source_bytes != 0)
		sources_.deallocate(source, source_bytes);
	streams_.deallocate(body, bytes);
}

void Space::reset_indirect(IndirectLink &site, std::uint64_t empty,
	const BlockLink *empty_link) noexcept
{
	for (auto &entry : site.entries) {
		entry.guest_rip = empty;
		entry.link = empty_link;
	}
	site.next_replacement = 0;
	site.listed = false;
}

IndirectLink *Space::indirect_link(std::uint64_t source, ExecutionMode mode)
{
	const auto *empty = link(empty_target(mode), mode);
	auto [created, inserted] =
		indirect_[mode_index(mode)].try_emplace(source, source);
	if (inserted)
		reset_indirect(created->site, empty_target(mode), empty);
	return &created->site;
}

void Space::learn_indirect(
	IndirectLink &site, std::uint64_t target, ExecutionMode mode)
{
	target = normalize_instruction_pointer(mode, target);
	const auto *target_link = link(target, mode);
	if (!site.listed) {
		live_indirect_[mode_index(mode)].push_back(&site);
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

TargetLinkTable *Space::target_table(ExecutionMode mode) noexcept
{
	auto &owned = targets_[mode_index(mode)];
	if (!owned) {
		try {
			owned = std::make_unique<TargetLinkTable>(
				vacant_target_links());
		} catch (...) {
			return nullptr;
		}
		account_.charge(static_cast<std::ptrdiff_t>(
			malloc_good_size(sizeof(TargetLinkTable))));
	}
	return owned.get();
}

void Space::arm(LinkRecord &record, const Body &body) noexcept
{
	if (!record.listed) {
		record.listed = true;
		record.next_live = live_links_;
		live_links_ = &record;
	}
	const auto &program = body.program;
	record.link.stream = program.native_stream().data();
	record.link.instruction_count = program.instruction_count();
	record.link.guest_rip = program.address();
}

void Space::announce(const LinkRecord &record, ExecutionMode mode) noexcept
{
	if (auto *table = target_table(mode); table != nullptr)
		(*table)[target_link_index(record.link.guest_rip)] = {
			record.link.guest_rip, &record.link};
}

void Space::reset_link_of(const Body &body) noexcept
{
	auto *record = find_link(
		body.program.address(), body.program.execution_mode());
	if (record != nullptr &&
	    record->link.stream == body.program.native_stream().data())
		record->reset();
}

void Space::invalidate_links() noexcept
{
	auto *record = live_links_;
	live_links_ = nullptr;
	while (record != nullptr) {
		auto *next = record->next_live;
		record->reset();
		record->listed = false;
		record->next_live = nullptr;
		record = next;
	}
	for (std::size_t index = 0; index < live_indirect_.size(); ++index) {
		auto &live = live_indirect_[index];
		if (live.empty())
			continue;
		const auto mode = index == 0 ? ExecutionMode::long64 :
			ExecutionMode::compat32;
		const auto empty = empty_target(mode);
		const auto *found = links_[index].find(empty);
		if (found == nullptr)
			continue;
		for (auto *site : live)
			reset_indirect(*site, empty, &found->link);
		live.clear();
	}
}

bool Space::sweep_due() const noexcept
{
	return gadget_cache::sweep_due(records(), kept_, departed_);
}

std::array<Space::LinkMarks, 2> Space::link_marks() const
{
	std::array<LinkMarks, 2> marks{links_[0].marks(), links_[1].marks()};
	constexpr std::array modes{ExecutionMode::long64, ExecutionMode::compat32};
	for (std::size_t mode = 0; mode < modes.size(); ++mode)
		if (const auto *empty = links_[mode].find(empty_target(modes[mode]));
		    empty != nullptr)
			marks[mode].mark(empty);
	return marks;
}

std::array<Space::SiteMarks, 2> Space::site_marks() const
{
	return {indirect_[0].marks(), indirect_[1].marks()};
}

void Space::swept() noexcept
{
	kept_ = records();
	departed_ = 0;
}

Space::Released Space::sweep(const std::array<LinkMarks, 2> &links,
	const std::array<SiteMarks, 2> &sites)
{
	constexpr std::array modes{ExecutionMode::long64, ExecutionMode::compat32};
	std::array<std::vector<IndirectLink *>, 2> live_indirect;
	for (std::size_t mode = 0; mode < modes.size(); ++mode) {
		live_indirect[mode].reserve(live_indirect_[mode].size());
		for (auto *site : live_indirect_[mode])
			if (sites[mode].marked(
				    reinterpret_cast<const IndirectRecord *>(
					    reinterpret_cast<std::uintptr_t>(site) -
					    offsetof(IndirectRecord, site))))
				live_indirect[mode].push_back(site);
	}

	/* From here each step leaves the address space consistent. */
	Released released{};
	for (std::size_t mode = 0; mode < modes.size(); ++mode) {
		released.sites += indirect_[mode].sweep(sites[mode]);
		live_indirect_[mode] = std::move(live_indirect[mode]);
	}
	const auto &vacant = vacant_target_links().front();
	for (std::size_t mode = 0; mode < modes.size(); ++mode) {
		if (const auto *empty = links_[mode].find(empty_target(modes[mode]));
		    empty != nullptr) {
			const IndirectLinkEntry cleared{
				empty_target(modes[mode]), &empty->link};
			indirect_[mode].for_each([&](IndirectRecord &record) {
				for (auto &entry : record.site.entries)
					if (!links[mode].marked(record_of(entry.link)))
						entry = cleared;
			});
		}
		if (const auto &table = targets_[mode])
			for (auto &entry : *table)
				if (entry.link != vacant.link &&
				    !links[mode].marked(record_of(entry.link)))
					entry = vacant;
	}
	/* The live list runs through the records, so the ones about to go leave
	 * it first. */
	LinkRecord **next = &live_links_;
	for (auto *record = live_links_; record != nullptr;) {
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
		released.links += links_[mode].sweep(links[mode]);
	return released;
}

#if UML_GADGET_CACHE_CENSUS
Space::LinkBytes Space::link_bytes() const noexcept
{
	LinkBytes bytes{};
	for (const auto &mode : links_)
		bytes.links += mode.bytes();
	for (const auto &mode : indirect_)
		bytes.indirect += mode.bytes();
	for (const auto &live : live_indirect_)
		bytes.indirect += live.capacity() * sizeof(IndirectLink *);
	for (const auto &table : targets_)
		if (table)
			bytes.targets += sizeof(TargetLinkTable);
	return bytes;
}
#endif

} // namespace uml::cpu::x86_64::gadgets
