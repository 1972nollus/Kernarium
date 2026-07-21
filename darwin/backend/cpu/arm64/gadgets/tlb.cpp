// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlb.hpp"

#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace uml::backend::arm64::gadgets {
namespace {

constexpr std::uint64_t page_size = 4096;

[[nodiscard]] constexpr std::size_t index_for(std::uint64_t address) noexcept
{
	const auto page = address >> 12;
	return static_cast<std::size_t>((page ^ (page >> 13)) &
		(tlb_entry_count - 1));
}

static_assert(sizeof(TlbEntry) == 32);
static_assert(offsetof(TlbEntry, read_page) == 0);
static_assert(offsetof(TlbEntry, write_page) == 8);
static_assert(offsetof(TlbEntry, read_data_minus_address) == 16);
static_assert(offsetof(TlbEntry, write_data_minus_address) == 24);
static_assert(offsetof(Tlb, generation) == 0);
static_assert(offsetof(Tlb, changes) == 8);
static_assert(offsetof(Tlb, fault_address) == 16);
static_assert(offsetof(Tlb, fault_access) == 24);
static_assert(offsetof(Tlb, entries) == 32);

} // namespace

Tlb::Tlb()
{
	flush();
}

void Tlb::flush()
{
	std::ranges::fill(entries, TlbEntry{});
}

void Tlb::invalidate(std::uint64_t address, std::uint64_t length)
{
	const auto end = address + length;
	for (auto page = address; page < end; page += page_size) {
		auto &entry = entries[index_for(page)];
		if (entry.read_page == page)
			entry.read_page = 1;
		if (entry.write_page == page)
			entry.write_page = 1;
	}
}

void Tlb::prepare(
	std::uint64_t next_generation, std::uint64_t next_changes)
{
	if (generation != next_generation) {
		flush();
		generation = next_generation;
		changes = next_changes;
		return;
	}
	if (changes == next_changes)
		return;

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
	auto &entry = entries[index_for(address)];
	const auto cached_page = access == Access::write ?
		entry.write_page : entry.read_page;
	if (cached_page == page) {
		const auto base = access == Access::write ?
			entry.write_data_minus_address :
			entry.read_data_minus_address;
		return reinterpret_cast<std::byte *>(
			base + address);
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
} // namespace uml::backend::arm64::gadgets

extern "C" void *uml_arm64_gadget_tlb_miss(
	uml::backend::arm64::gadgets::Tlb *tlb, std::uint64_t address,
	std::uint32_t access)
{
	if (tlb == nullptr)
		return nullptr;
	return tlb->translate(address,
		access == static_cast<std::uint32_t>(
			uml::backend::arm64::gadgets::Access::write) ?
			uml::backend::arm64::gadgets::Access::write :
			uml::backend::arm64::gadgets::Access::read);
}

extern "C" std::uint32_t uml_arm64_gadget_condition_holds(
	std::uint32_t nzcv, std::uint32_t condition) noexcept
{
	const bool negative = (nzcv & 0x8000'0000U) != 0;
	const bool zero = (nzcv & 0x4000'0000U) != 0;
	const bool carry = (nzcv & 0x2000'0000U) != 0;
	const bool overflow = (nzcv & 0x1000'0000U) != 0;
	bool result;
	switch (condition >> 1) {
	case 0: result = zero; break;
	case 1: result = carry; break;
	case 2: result = negative; break;
	case 3: result = overflow; break;
	case 4: result = carry && !zero; break;
	case 5: result = negative == overflow; break;
	case 6: result = !zero && negative == overflow; break;
	default: return 1;
	}
	if ((condition & 1U) != 0)
		result = !result;
	return result;
}
