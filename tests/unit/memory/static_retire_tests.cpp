// SPDX-License-Identifier: GPL-2.0-or-later
/* A memory domain keeps the bookkeeping of the pages it maps (the generation's
 * fill index and its mapping history) in a heap it maps itself. The default
 * malloc zone then does not grow with the pages a run maps, and the heap's
 * pages leave the process when the domain retires: the zone returns freed
 * memory when it chooses, which a run that restarts cannot wait for. */
#include "domain_contract.hpp"
#include "domain_heap.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <malloc/malloc.h>
#include <sys/mman.h>
#include <thread>

namespace {

using uml::testing::require;
using uml::testing::ScratchBacking;

/* A sanitizer replaces the allocator, and the default zone then says nothing
 * about the program's own allocations. */
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool zone_is_measurable = false;
#else
constexpr bool zone_is_measurable = true;
#endif

std::atomic_uint64_t active_domain{};

bool provide_test_domain(std::uint64_t &domain) noexcept
{
	domain = active_domain.load(std::memory_order_acquire);
	return domain != 0;
}

[[nodiscard]] std::size_t zone_in_use() noexcept
{
	malloc_statistics_t statistics{};
	malloc_zone_statistics(malloc_default_zone(), &statistics);
	return statistics.size_in_use;
}

/* Single-page maps in batches, as a guest's page faults and mmaps make them:
 * the fill index and the history grow with the pages. */
void map_pages(unsigned long generation, unsigned long base, int descriptor,
	std::size_t pages, std::size_t per_batch, std::size_t backing_pages)
{
	constexpr std::size_t page = DARWIN_UML_GUEST_PAGE_SIZE;
	for (std::size_t first = 0; first < pages; first += per_batch) {
		require(darwin_uml_static_vm_batch_begin(generation) == 0,
			"a batch did not begin");
		for (std::size_t index = first;
		     index < first + per_batch && index < pages; ++index)
			require(darwin_uml_static_vm_map_generation(generation,
					reinterpret_cast<void *>(base + index * page),
					descriptor, (index % backing_pages) * page, page,
					PROT_READ | PROT_WRITE, 0, 1) == 0,
				"a page was not mapped");
		require(darwin_uml_static_vm_batch_end() == 0,
			"a batch did not end");
	}
}

/* A run's vCPU threads end before its domain retires, and with them every
 * cached resolution of the domain: each step runs on a thread of its own. */
template <typename Step>
void on_own_thread(Step step)
{
	std::thread thread{step};
	thread.join();
}

void test_domain_indexes_live_in_its_heap()
{
	constexpr std::uint64_t domain = 40'001;
	constexpr unsigned long generation = 950;
	constexpr unsigned long base = 0x1000'0000;
	constexpr std::size_t backing_pages = 64;
	constexpr std::size_t pages = 16384;
	/* What the domain may still put in the zone, however many pages it maps:
	 * the space, its generation and their registry entries. */
	constexpr std::size_t allowance = 64U << 10;
	ScratchBacking backing{"uml-static-retire-XXXXXX",
		backing_pages * DARWIN_UML_GUEST_PAGE_SIZE};
	auto &region = uml::memory::HeapRegion::instance();

	/* Warm the registries and the zone with a domain like it. */
	on_own_thread([&] {
		active_domain.store(domain - 1, std::memory_order_release);
		require(darwin_uml_static_vm_activate(generation - 1) == 0,
			"the warm-up generation was not activated");
		map_pages(generation - 1, base, backing.descriptor(), 256, 64,
			backing_pages);
		active_domain.store(0, std::memory_order_release);
	});
	uml::memory::retire_domain(domain - 1);

	const auto heap_before = region.committed();
	const auto heaps_before = uml::memory::DomainHeap::live_heaps();
	const auto zone_before = zone_in_use();
	std::size_t heap_mapped = 0;
	std::size_t zone_mapped = 0;
	on_own_thread([&] {
		active_domain.store(domain, std::memory_order_release);
		require(darwin_uml_static_vm_activate(generation) == 0,
			"the generation was not activated");
		map_pages(generation, base, backing.descriptor(), pages, 64,
			backing_pages);
		heap_mapped = region.committed() - heap_before;
		zone_mapped = zone_in_use();
		active_domain.store(0, std::memory_order_release);
	});
	std::printf("static domain: %zu pages mapped, heap +%zu bytes, zone "
		    "%+lld bytes\n",
		pages, heap_mapped,
		static_cast<long long>(zone_mapped) -
			static_cast<long long>(zone_before));
	/* One word per page is the least the fill index holds. */
	require(heap_mapped >= pages * sizeof(std::uintptr_t),
		"the domain's indexes are not in its heap");
	require(uml::memory::DomainHeap::live_heaps() == heaps_before + 1,
		"the domain does not own one heap");
	require(!zone_is_measurable || zone_mapped <= zone_before + allowance,
		"the domain's indexes grew the default zone");

	uml::memory::retire_domain(domain);
	require(region.committed() == heap_before,
		"retirement left heap pages mapped");
	require(uml::memory::DomainHeap::live_heaps() == heaps_before,
		"retirement left the domain's heap alive");
	require(!zone_is_measurable || zone_in_use() <= zone_before + allowance,
		"the default zone did not return to where it was");
}

} // namespace

int main()
{
	uml::memory::set_domain_provider(&provide_test_domain);
	test_domain_indexes_live_in_its_heap();
	uml::memory::set_domain_provider(nullptr);
	std::puts("static retire tests passed");
	return EXIT_SUCCESS;
}
