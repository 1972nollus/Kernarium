// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_address_space.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>

namespace uml::memory {
namespace {

constexpr std::uintptr_t guest_page_size = 4096;

[[nodiscard]] std::uintptr_t host_page_size() noexcept
{
	static const auto size = static_cast<std::uintptr_t>(getpagesize());
	return size;
}

[[nodiscard]] std::uintptr_t slot_for(std::uintptr_t address) noexcept
{
	return address & ~(host_page_size() - 1);
}

[[nodiscard]] int shadow_data_protection(int protection) noexcept
{
	/* kern_map() currently supplies execute for every native kernel alias,
	 * even though these private 4 KiB shadows only access physical backing.
	 * Keep the entire shared MMU path data-only.  Genuine native kernel text
	 * is protected directly by the host-page-granular ELF loader, while JIT
	 * code uses its separate publication allocator.
	 */
	return protection & ~PROT_EXEC;
}

int read_exact(int fd, std::uint64_t offset, std::byte *destination,
	       std::size_t length)
{
	std::size_t completed = 0;
	while (completed < length) {
		const auto result = pread(fd, destination + completed,
			length - completed, static_cast<off_t>(offset + completed));
		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0)
			return -1;
		completed += static_cast<std::size_t>(result);
	}
	return 0;
}

int write_exact(int fd, std::uint64_t offset, const std::byte *source,
		std::size_t length)
{
	std::size_t completed = 0;
	while (completed < length) {
		const auto result = pwrite(fd, source + completed,
			length - completed, static_cast<off_t>(offset + completed));
		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0)
			return -1;
		completed += static_cast<std::size_t>(result);
	}
	return 0;
}

} // namespace

bool NativeAddressSpace::needs_shadow(
	std::uintptr_t address, std::uint64_t offset,
	std::uint64_t length) noexcept
{
	const auto mask = host_page_size() - 1;
	return ((address | offset | length) & mask) != 0;
}

bool NativeAddressSpace::contains(
	std::uintptr_t address, std::uint64_t length) const
{
	std::lock_guard lock{mutex_};
	return contains_locked(address, length);
}

bool NativeAddressSpace::contains_locked(
	std::uintptr_t address, std::uint64_t length) const
{
	if (length == 0 || address + length < address)
		return false;
	address &= ~(guest_page_size - 1);
	for (std::uint64_t position = 0; position < length;
	     position += guest_page_size)
		/*
		 * An operation on an absent 4 KiB sibling still targets the same
		 * 16 KiB Darwin VM slot.  It must be handled here so a raw munmap
		 * cannot discard the slot's other live Linux quarters.
		 */
		if (materialized_slots_.contains(slot_for(address + position)))
			return true;
	return false;
}

int NativeAddressSpace::flush_page_locked(std::uintptr_t page)
{
	auto found = mappings_.find(page);
	if (found == mappings_.end() || !found->second.loaded)
		return 0;
	std::array<std::byte, guest_page_size> backing{};
	const auto *current = reinterpret_cast<const std::byte *>(page);
	if (std::memcmp(current, found->second.original.data(),
		guest_page_size) == 0)
		return 0;
	if (read_exact(found->second.fd, found->second.offset,
		backing.data(), backing.size()) < 0)
		return -1;
	if (backing != found->second.original) {
		/*
		 * A shared physical alias changed this page after the shadow was
		 * loaded.  Its contents are authoritative; never overwrite a reused
		 * physical page with a stale shadow.
		 */
		if (std::getenv("UML_MMU_TRACE") != nullptr)
			std::fprintf(stderr,
				"Darwin native shadow conflict address=%#lx fd=%d offset=%#llx\n",
				static_cast<unsigned long>(page), found->second.fd,
				found->second.offset);
		found->second.loaded = false;
		return 0;
	}
	if (write_exact(found->second.fd, found->second.offset,
		current, guest_page_size) < 0)
		return -1;
	std::copy_n(current, guest_page_size,
		found->second.original.begin());
	return 0;
}

int NativeAddressSpace::flush_slot_locked(std::uintptr_t slot)
{
	if (!materialized_slots_.contains(slot))
		return 0;
	for (std::uintptr_t position = 0; position < host_page_size();
	     position += guest_page_size)
		if (flush_page_locked(slot + position) < 0)
			return -1;
	return 0;
}

int NativeAddressSpace::load_page_locked(std::uintptr_t page)
{
	auto found = mappings_.find(page);

	if (found == mappings_.end())
		return 0;
	if (read_exact(found->second.fd, found->second.offset,
		reinterpret_cast<std::byte *>(page), guest_page_size) < 0)
		return -1;
	std::copy_n(reinterpret_cast<const std::byte *>(page), guest_page_size,
		found->second.original.begin());
	found->second.loaded = true;
	return 0;
}

int NativeAddressSpace::materialize_slot_locked(std::uintptr_t slot)
{
	if (materialized_slots_.contains(slot))
		return set_slot_protection_locked(
			slot, PROT_READ | PROT_WRITE);
	void *result = mmap(reinterpret_cast<void *>(slot), host_page_size(),
		PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
	if (result == MAP_FAILED)
		return -1;
	materialized_slots_.insert(slot);
	slot_protections_.insert_or_assign(slot, PROT_READ | PROT_WRITE);
	return 0;
}

int NativeAddressSpace::set_slot_protection_locked(
	std::uintptr_t slot, int protection)
{
	/* This is the sole protection transition for every tracked host slot.
	 * Refuse W+X even if a future caller bypasses the normal RX/RW selection.
	 */
	if ((protection & (PROT_WRITE | PROT_EXEC)) ==
	    (PROT_WRITE | PROT_EXEC)) {
		errno = EACCES;
		return -1;
	}
	if (const auto current = slot_protections_.find(slot);
	    current != slot_protections_.end() && current->second == protection)
		return 0;
	if (mprotect(reinterpret_cast<void *>(slot), host_page_size(),
		     protection) != 0)
		return -1;
	slot_protections_.insert_or_assign(slot, protection);
	return 0;
}

int NativeAddressSpace::update_slot_protection_locked(std::uintptr_t slot)
{
	int protection = PROT_NONE;
	bool populated = false;

	for (std::uintptr_t position = 0; position < host_page_size();
	     position += guest_page_size) {
		const auto page = slot + position;
		if (const auto found = mappings_.find(page);
		    found != mappings_.end()) {
			populated = true;
			protection |= shadow_data_protection(
				found->second.protection);
		}
	}
	if (!populated) {
		slot_protections_.erase(slot);
		if (materialized_slots_.erase(slot) != 0)
			return munmap(reinterpret_cast<void *>(slot), host_page_size());
		return 0;
	}
	if (!materialized_slots_.contains(slot) &&
	    materialize_slot_locked(slot) < 0)
		return -1;
	/* Darwin applies VM permissions to the complete host slot.  Every tracked
	 * mapping was made data-only above, so this transition can never add
	 * execute permission or become an iOS-incompatible RW-to-RX publication.
	 */
	return set_slot_protection_locked(slot, protection);
}

int NativeAddressSpace::map(
	std::uintptr_t address, int fd, std::uint64_t offset,
	std::uint64_t length, int protection)
{
	if (((address | offset | length) & (guest_page_size - 1)) != 0 ||
	    length == 0 || address + length < address)
		return -1;
	std::lock_guard lock{mutex_};
	std::vector<std::uintptr_t> slots;
	for (std::uint64_t position = 0; position < length;
	     position += guest_page_size) {
		const auto page = address + position;
		const auto page_offset = offset + position;
		auto existing = mappings_.find(page);
		if (existing != mappings_.end() && existing->second.loaded &&
		    existing->second.fd == fd &&
		    existing->second.offset == page_offset &&
		    existing->second.protection == protection)
			continue;
		const auto slot = slot_for(page);
		if (std::ranges::find(slots, slot) == slots.end())
			slots.push_back(slot);
		if (existing != mappings_.end() && existing->second.fd == fd &&
		    existing->second.offset == page_offset) {
			existing->second.protection = protection;
			if (existing->second.loaded)
				continue;
		} else {
			if (existing != mappings_.end() &&
			    flush_page_locked(page) < 0)
				return -1;
			mappings_.insert_or_assign(page,
				Mapping{fd, page_offset, protection});
		}
		/* Never replace an already materialized 16 KiB slot.  A sibling
		 * virtual CPU may be reading or writing one of its other live 4 KiB
		 * quarters.  Temporarily make the stable slot writable and populate
		 * only the quarter whose Linux mapping changed.
		 */
		if (materialize_slot_locked(slot) < 0 || load_page_locked(page) < 0)
			return -1;
	}
	for (const auto slot : slots)
		if (update_slot_protection_locked(slot) < 0)
			return -1;
	return 0;
}

int NativeAddressSpace::protect(
	std::uintptr_t address, std::uint64_t length, int protection)
{
	if (length == 0 || address + length < address)
		return -1;
	std::lock_guard lock{mutex_};
	std::vector<std::uintptr_t> slots;
	address &= ~(guest_page_size - 1);
	for (std::uint64_t position = 0; position < length;
	     position += guest_page_size) {
		const auto page = address + position;
		auto mapping = mappings_.find(page);
		if (mapping == mappings_.end())
			continue;
		const auto slot = slot_for(page);
		if (std::ranges::find(slots, slot) == slots.end())
			slots.push_back(slot);
		mapping->second.protection = protection;
	}
	for (const auto slot : slots)
		if (update_slot_protection_locked(slot) < 0)
			return -1;
	return 0;
}

int NativeAddressSpace::unmap(
	std::uintptr_t address, std::uint64_t length)
{
	if (length == 0 || address + length < address)
		return length == 0 ? 0 : -1;
	std::lock_guard lock{mutex_};
	std::vector<std::uintptr_t> slots;
	address &= ~(guest_page_size - 1);
	for (std::uint64_t position = 0; position < length;
	     position += guest_page_size) {
		const auto page = address + position;
		const auto slot = slot_for(page);
		if (std::ranges::find(slots, slot) == slots.end())
			slots.push_back(slot);
		if (flush_page_locked(page) < 0)
			return -1;
		mappings_.erase(page);
	}
	for (const auto slot : slots)
		if (update_slot_protection_locked(slot) < 0)
			return -1;
	return 0;
}

int NativeAddressSpace::flush(
	std::uintptr_t address, std::uint64_t length)
{
	std::lock_guard lock{mutex_};
	const auto first = slot_for(address);
	const auto end = address + length;
	for (auto slot = first; slot < end; slot += host_page_size())
		if (flush_slot_locked(slot) < 0)
			return -1;
	return 0;
}

int NativeAddressSpace::refresh(
	std::uintptr_t address, std::uint64_t length)
{
	if (length == 0 || address + length < address)
		return length == 0 ? 0 : -1;
	std::lock_guard lock{mutex_};
	std::vector<std::uintptr_t> slots;
	const auto first = address & ~(guest_page_size - 1);
	const auto end = address + length;
	for (auto page = first; page < end; page += guest_page_size) {
		if (!mappings_.contains(page))
			continue;
		const auto slot = slot_for(page);
		if (std::ranges::find(slots, slot) == slots.end())
			slots.push_back(slot);
		/* Synchronize only the requested Linux page.  Rebuilding the host
		 * slot would overwrite unrelated live pages beneath other CPUs.
		 */
		if (flush_page_locked(page) < 0 ||
		    materialize_slot_locked(slot) < 0 || load_page_locked(page) < 0)
			return -1;
	}
	for (const auto slot : slots)
		if (update_slot_protection_locked(slot) < 0)
			return -1;
	return 0;
}

int NativeAddressSpace::retry(
	std::uintptr_t address, bool execute, bool write)
{
	std::lock_guard lock{mutex_};
	const auto page = address & ~(guest_page_size - 1);
	const auto mapping = mappings_.find(page);
	if (mapping == mappings_.end() ||
	    !materialized_slots_.contains(slot_for(address)))
		return 0;
	const auto required = execute ? PROT_EXEC : write ? PROT_WRITE : PROT_READ;
	const auto intended = shadow_data_protection(mapping->second.protection);
	if ((intended & required) == 0)
		return 0;
	/* The slot union already contains every intended data permission.  Retry
	 * only reports whether Linux may retry the access; it never changes host
	 * VM protections.  In particular, execute can never be authorized here.
	 */
	return 1;
}

NativeAddressSpace &native_address_space()
{
	static NativeAddressSpace instance;
	return instance;
}

} // namespace uml::memory
