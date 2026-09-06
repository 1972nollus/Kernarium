// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory/tlb.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace uml::memory {
namespace {

constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;

[[nodiscard]] constexpr std::size_t index_for(
	std::uint64_t address) noexcept
{
	const auto page = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
	return static_cast<std::size_t>(
		(page & (tlb_set_count - 1)) * tlb_way_count);
#else
	return static_cast<std::size_t>((page ^ (page >> 13)) &
		(tlb_set_count - 1));
#endif
}

static_assert(sizeof(TlbEntry) == 32);
static_assert(offsetof(TlbEntry, read_page) == 0);
static_assert(offsetof(TlbEntry, read_data_minus_address) == 8);
static_assert(offsetof(TlbEntry, write_page) == 16);
static_assert(offsetof(TlbEntry, write_data_minus_address) == 24);
static_assert(offsetof(Tlb, entries) == 0);
static_assert(offsetof(Tlb, generation) ==
	tlb_entry_count * sizeof(TlbEntry));
static_assert(offsetof(Tlb, changes) ==
	offsetof(Tlb, generation) + sizeof(std::uint64_t));
static_assert(offsetof(Tlb, fault_address) ==
	offsetof(Tlb, changes) + sizeof(std::uint64_t));
static_assert(offsetof(Tlb, fault_access) ==
	offsetof(Tlb, fault_address) + sizeof(std::uint64_t));

} // namespace

Tlb::Tlb()
{
	flush();
}

void Tlb::flush()
{
	std::ranges::fill(entries, TlbEntry{});
	std::ranges::fill(replacement_way, 0);
}

void Tlb::invalidate(std::uint64_t address, std::uint64_t length)
{
	/* Keys may carry a logical tag in the top byte (translate_tagged), and
	 * the set index never depends on that byte, so a masked compare finds
	 * every key of the page in its own set. */
	constexpr std::uint64_t untagged = 0x00ff'ffff'ffff'ffffULL;
	const auto end = address + length;
	for (auto page = address; page < end; page += page_size) {
		const auto first = index_for(page);
		for (std::size_t way = 0; way < tlb_way_count; ++way) {
			auto &entry = entries[first + way];
			if ((entry.read_page & untagged) == page)
				entry.read_page = 1;
			if ((entry.write_page & untagged) == page)
				entry.write_page = 1;
		}
	}
}

std::byte *Tlb::translate_tagged(std::uint64_t address, Access access)
{
	constexpr std::uint64_t untagged_mask = 0x00ff'ffff'ffff'ffffULL;
	if (!tagged_addresses || (address & ~untagged_mask) == 0 ||
	    access == Access::execute)
		return translate(address, access);
	const auto untagged = address & untagged_mask;
	auto *host = translate(untagged, access);
	if (host == nullptr)
		return nullptr;
	/* Re-key the entry translate() just filled or hit so the tagged form
	 * the guest actually uses hits next time. */
	const auto page = untagged & ~(page_size - 1);
	const auto tagged_page = address & ~(page_size - 1);
	const auto first = index_for(untagged);
	for (std::size_t way = 0; way < tlb_way_count; ++way) {
		auto &entry = entries[first + way];
		if (access == Access::write && entry.write_page == page) {
			entry.write_page = tagged_page;
			entry.write_data_minus_address -= (tagged_page - page);
			break;
		}
		if (access != Access::write && entry.read_page == page) {
			entry.read_page = tagged_page;
			entry.read_data_minus_address -= (tagged_page - page);
			break;
		}
	}
	return host;
}

void Tlb::prepare(std::uint64_t next_generation,
	std::uint64_t next_changes)
{
	if (generation != next_generation) {
		flush();
		generation = next_generation;
		changes = next_changes;
		return;
	}
	if (changes != next_changes)
		reconcile_changes(next_generation, next_changes);
}

void Tlb::discard(std::uint64_t address)
{
	invalidate(address, 1);
}

__attribute__((noinline)) void Tlb::reconcile_changes(
	std::uint64_t next_generation, std::uint64_t next_changes)
{
	std::array<darwin_uml_static_mapping_change, 256> changed_ranges{};
	std::uint64_t current_changes = next_changes;
	const int count = darwin_uml_static_vm_mapping_changes(
		next_generation, changes, changed_ranges.data(),
		changed_ranges.size(), &current_changes);
	if (count < 0) {
		flush();
	} else {
		for (int index = 0; index < count; ++index)
			invalidate(changed_ranges[index].address,
				changed_ranges[index].length);
	}
	generation = next_generation;
	changes = current_changes;
}

std::byte *Tlb::translate(std::uint64_t address, Access access)
{
	const auto page = address & ~(page_size - 1);
	if (access == Access::execute) {
		/* Translated engines normally fetch through StaticGuestMemory before
		 * entering generated code. Keep execute translation precise here as
		 * well without changing the assembly-visible two-way data TLB layout. */
		void *translated = nullptr;
		const auto result = darwin_uml_static_vm_translate(
			static_cast<unsigned long>(generation),
			static_cast<unsigned long>(page), DARWIN_UML_STATIC_EXECUTE,
			&translated);
		if (result != DARWIN_UML_STATIC_TRANSLATED || translated == nullptr) {
			fault_address = address;
			fault_access = Access::execute;
			return nullptr;
		}
		return static_cast<std::byte *>(translated) +
			(address & (page_size - 1));
	}
	const auto first = index_for(address);
	for (std::size_t way = 0; way < tlb_way_count; ++way) {
		auto &entry = entries[first + way];
		const auto cached_page = access == Access::write ?
			entry.write_page : entry.read_page;
		if (cached_page == page) {
			const auto base = access == Access::write ?
				entry.write_data_minus_address :
				entry.read_data_minus_address;
			return reinterpret_cast<std::byte *>(base + address);
		}
	}

	void *translated = nullptr;
	const auto host_access = access == Access::write ?
		DARWIN_UML_STATIC_WRITE : DARWIN_UML_STATIC_READ;
	const auto result = darwin_uml_static_vm_translate(
		static_cast<unsigned long>(generation),
		static_cast<unsigned long>(page), host_access, &translated);
	if (result != DARWIN_UML_STATIC_TRANSLATED || translated == nullptr) {
		fault_address = address;
		fault_access = access;
		return nullptr;
	}
	std::size_t selected = tlb_way_count;
	for (std::size_t way = 0; way < tlb_way_count; ++way) {
		const auto &candidate = entries[first + way];
		const auto other_page = access == Access::write ?
			candidate.read_page : candidate.write_page;
		if (other_page == page) {
			selected = way;
			break;
		}
	}
	if (selected == tlb_way_count) {
		for (std::size_t way = 0; way < tlb_way_count; ++way) {
			const auto &candidate = entries[first + way];
			const auto cached_page = access == Access::write ?
				candidate.write_page : candidate.read_page;
			if (cached_page == 1) {
				selected = way;
				break;
			}
		}
	}
	const auto set = first / tlb_way_count;
	if (selected == tlb_way_count) {
		selected = replacement_way[set];
		replacement_way[set] = static_cast<std::uint8_t>(
			(selected + 1) % tlb_way_count);
	}
	auto &entry = entries[first + selected];
	const auto host_page = reinterpret_cast<std::uintptr_t>(translated);
	if (access == Access::write) {
		entry.write_data_minus_address = host_page - page;
		entry.write_page = page;
	} else {
		entry.read_data_minus_address = host_page - page;
		entry.read_page = page;
	}
	return static_cast<std::byte *>(translated) +
		(address & (page_size - 1));
}

bool Tlb::read_allocation_tag(std::uint64_t address, std::uint8_t &tag)
{
	if (darwin_uml_static_vm_load_allocation_tag(
		generation, address, &tag) == DARWIN_UML_STATIC_TRANSLATED)
		return true;
	fault_address = address & 0x00ff'ffff'ffff'fff0ULL;
	fault_access = Access::read;
	return false;
}

bool Tlb::write_allocation_tag(std::uint64_t address, std::uint8_t tag)
{
	if (darwin_uml_static_vm_store_allocation_tag(
		generation, address, tag) == DARWIN_UML_STATIC_TRANSLATED)
		return true;
	fault_address = address & 0x00ff'ffff'ffff'fff0ULL;
	fault_access = Access::write;
	return false;
}

} // namespace uml::memory
