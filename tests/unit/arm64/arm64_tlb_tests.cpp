// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory/tlb.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "scratch_backing.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>
#include <sys/mman.h>

namespace {

using uml::memory::Access;
using uml::memory::Tlb;
using uml::memory::tlb_empty_key;

constexpr std::uint64_t page = DARWIN_UML_GUEST_PAGE_SIZE;

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

class Mapping final {
public:
	Mapping(unsigned long generation, std::uint64_t base, std::size_t pages)
		: generation_{generation}
		, backing_{"uml-tlb.XXXXXX", pages * page}
	{
		require(darwin_uml_static_vm_map_generation(generation,
			reinterpret_cast<void *>(base), backing_.descriptor(),
			0, pages * page, PROT_READ | PROT_WRITE, 0, 1) == 0);
	}

	Mapping(const Mapping &) = delete;
	Mapping &operator=(const Mapping &) = delete;

	~Mapping()
	{
		require(darwin_uml_static_vm_destroy(generation_) == 0);
	}

private:
	unsigned long generation_;
	uml::testing::ScratchBacking backing_;
};

[[nodiscard]] std::uint64_t changes(unsigned long generation)
{
	return darwin_uml_static_vm_sequences(generation).mapping_changes;
}

[[nodiscard]] bool empty(const Tlb &tlb)
{
	return std::ranges::all_of(tlb.entries, [](const auto &entry) {
		return entry.read_page == tlb_empty_key &&
			entry.write_page == tlb_empty_key;
	});
}

/* Whether any way holds the page, as presented or with its tag stripped. */
[[nodiscard]] bool cached(const Tlb &tlb, std::uint64_t address)
{
	constexpr std::uint64_t untagged =
		0x00ff'ffff'ffff'ffffULL >> DARWIN_UML_GUEST_PAGE_SHIFT;
	const auto key = (address >> DARWIN_UML_GUEST_PAGE_SHIFT) & untagged;
	return std::ranges::any_of(tlb.entries, [&](const auto &entry) {
		return (entry.read_page != tlb_empty_key &&
				(entry.read_page & untagged) == key) ||
			(entry.write_page != tlb_empty_key &&
				(entry.write_page & untagged) == key);
	});
}

void fill(Tlb &tlb, std::uint64_t base, std::size_t pages)
{
	for (std::size_t index = 0; index < pages; ++index) {
		const auto address = base + index * page;
		require(tlb.translate(address, Access::read) != nullptr);
		if (index % 2 == 0)
			require(tlb.translate(address, Access::write) != nullptr);
	}
}

/* A flush clears only the sets filled since the previous one until more sets
 * were filled than it tracks; both must leave nothing a lookup could hit. */
void generation_switch_clears_every_filled_set()
{
	constexpr unsigned long first = 220;
	constexpr unsigned long second = 221;
	constexpr std::uint64_t base = 0x80'0000;
	constexpr std::size_t pages = 3 * uml::memory::tlb_dirty_capacity;
	Mapping mapping{first, base, pages};
	auto tlb = std::make_unique<Tlb>();
	require(empty(*tlb));

	for (const std::size_t filled : {std::size_t{5},
		     uml::memory::tlb_dirty_capacity, pages}) {
		tlb->prepare(first, changes(first));
		fill(*tlb, base, filled);
		require(cached(*tlb, base) &&
			cached(*tlb, base + (filled - 1) * page));
		tlb->prepare(second, 0);
		require(empty(*tlb));
	}
	/* The table refills correctly after a partial flush. */
	tlb->prepare(first, changes(first));
	fill(*tlb, base, 3);
	tlb->prepare(second, 0);
	tlb->prepare(first, changes(first));
	require(empty(*tlb));
	fill(*tlb, base, pages);
	require(cached(*tlb, base + (pages - 1) * page));
}

/* An unmap wider than the table invalidates by key: every entry in the range,
 * tagged or not, goes, and entries on either side stay. */
void wide_unmap_invalidates_only_its_range()
{
	constexpr unsigned long generation = 222;
	/* The five pages probed below occupy distinct sets in both page
	 * profiles, so no fill evicts another. */
	constexpr std::uint64_t base = 0x1600'0000;
	constexpr std::size_t removed = uml::memory::tlb_set_count + 1;
	constexpr std::size_t pages = removed + 2;
	constexpr std::uint64_t tag = 0x5aULL << 56;
	Mapping mapping{generation, base, pages};
	auto tlb = std::make_unique<Tlb>();
	tlb->tagged_addresses = true;
	tlb->prepare(generation, changes(generation));

	const std::uint64_t below = base;
	const std::uint64_t inside = base + page;
	const std::uint64_t tagged = base + 7 * page;
	const std::uint64_t last_inside = base + removed * page;
	const std::uint64_t above = base + (removed + 1) * page;
	for (const auto address : {below, inside, last_inside, above}) {
		require(tlb->translate(address, Access::read) != nullptr);
		require(tlb->translate(address, Access::write) != nullptr);
	}
	require(tlb->translate_tagged(tagged | tag, Access::read) != nullptr);
	require(tlb->translate(tagged | tag, Access::read) != nullptr);
	for (const auto address : {below, inside, tagged, last_inside, above})
		require(cached(*tlb, address));

	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(inside), removed * page) == 0);
	tlb->prepare(generation, changes(generation));
	require(!cached(*tlb, inside));
	require(!cached(*tlb, tagged));
	require(!cached(*tlb, last_inside));
	require(cached(*tlb, below));
	require(cached(*tlb, above));
	require(tlb->translate(inside, Access::read) == nullptr);
	require(tlb->translate(below, Access::write) != nullptr);
}

/* A narrow unmap still invalidates page by page. */
void narrow_unmap_invalidates_its_pages()
{
	constexpr unsigned long generation = 223;
	constexpr std::uint64_t base = 0x4000'0000;
	Mapping mapping{generation, base, 4};
	auto tlb = std::make_unique<Tlb>();
	tlb->prepare(generation, changes(generation));
	fill(*tlb, base, 4);
	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(base + page), 2 * page) == 0);
	tlb->prepare(generation, changes(generation));
	require(cached(*tlb, base));
	require(!cached(*tlb, base + page));
	require(!cached(*tlb, base + 2 * page));
	require(cached(*tlb, base + 3 * page));
}

} // namespace

int main()
{
	generation_switch_clears_every_filled_set();
	wide_unmap_invalidates_only_its_range();
	narrow_unmap_invalidates_its_pages();
}
