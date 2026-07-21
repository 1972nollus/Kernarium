// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_runtime.h"
#include "uml_darwin_static_mmu.h"
#include "native_address_space.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>

namespace {

constexpr std::uintptr_t guest_address_limit = 0x6000'0000'0000ULL;

[[nodiscard]] bool guest_address(const void *address) noexcept
{
	return reinterpret_cast<std::uintptr_t>(address) < guest_address_limit;
}

[[nodiscard]] bool trace_enabled() noexcept
{
	static const bool enabled = [] {
		const auto *value = std::getenv("UML_MMU_TRACE");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return enabled;
}

} // namespace

extern "C" int darwin_uml_vm_map(
	void *address, int fd, unsigned long long offset, unsigned long length,
	int protection, int guest_code, int guest_writable)
{
	if (trace_enabled() && !guest_address(address))
		std::fprintf(stderr,
			"Darwin native map address=%p fd=%d offset=%#llx length=%#lx protection=%#x shadow=%d\n",
			address, fd, offset, length, protection,
			uml::runtime::NativeAddressSpace::needs_shadow(
				reinterpret_cast<std::uintptr_t>(address), offset,
				length));
	if (guest_address(address))
		return darwin_uml_static_vm_map(address, fd, offset, length,
			protection, guest_code, guest_writable);
	if (uml::runtime::NativeAddressSpace::needs_shadow(
		reinterpret_cast<std::uintptr_t>(address), offset, length))
		return uml::runtime::native_address_space().map(
			reinterpret_cast<std::uintptr_t>(address), fd, offset,
			length, protection);
	if ((protection & (PROT_WRITE | PROT_EXEC)) ==
	    (PROT_WRITE | PROT_EXEC))
		protection &= ~PROT_EXEC;
	const auto result = mmap(address, length, protection,
		MAP_FIXED | MAP_SHARED, fd, offset) == MAP_FAILED ? -1 : 0;
	if (trace_enabled())
		std::fprintf(stderr,
			"Darwin static native map address=%p offset=%#llx length=%#lx protection=%#x result=%d\n",
			address, offset, length, protection, result);
	return result;
}

extern "C" int darwin_uml_vm_map_generation(
	unsigned long generation, void *address, int fd,
	unsigned long long offset, unsigned long length, int protection,
	int guest_code, int guest_writable)
{
	if (!guest_address(address))
		return darwin_uml_vm_map(address, fd, offset, length, protection,
			guest_code, guest_writable);
	return darwin_uml_static_vm_map_generation(
		generation, address, fd, offset, length, protection,
		guest_code, guest_writable);
}

extern "C" int darwin_uml_vm_protect(
	void *address, unsigned long length, int protection)
{
	if (trace_enabled() && !guest_address(address))
		std::fprintf(stderr,
			"Darwin native protect address=%p length=%#lx protection=%#x tracked=%d\n",
			address, length, protection,
			uml::runtime::native_address_space().contains(
				reinterpret_cast<std::uintptr_t>(address), length));
	return guest_address(address) ?
		darwin_uml_static_vm_protect(address, length, protection) :
		uml::runtime::native_address_space().contains(
			reinterpret_cast<std::uintptr_t>(address), length) ?
			uml::runtime::native_address_space().protect(
				reinterpret_cast<std::uintptr_t>(address), length,
				protection) : mprotect(address, length, protection);
}

extern "C" int darwin_uml_vm_unmap(void *address, unsigned long length)
{
	if (trace_enabled() && !guest_address(address))
		std::fprintf(stderr,
			"Darwin native unmap address=%p length=%#lx tracked=%d\n",
			address, length,
			uml::runtime::native_address_space().contains(
				reinterpret_cast<std::uintptr_t>(address), length));
	return guest_address(address) ?
		darwin_uml_static_vm_unmap(address, length) :
		(uml::runtime::native_address_space().contains(
			reinterpret_cast<std::uintptr_t>(address), length) ||
		 uml::runtime::NativeAddressSpace::needs_shadow(
			reinterpret_cast<std::uintptr_t>(address), 0, length)) ?
			uml::runtime::native_address_space().unmap(
				reinterpret_cast<std::uintptr_t>(address), length) :
			munmap(address, length);
}

extern "C" int darwin_uml_vm_unmap_generation(
	unsigned long generation, void *address, unsigned long length)
{
	return guest_address(address) ?
		darwin_uml_static_vm_unmap_generation(generation, address, length) :
		(uml::runtime::native_address_space().contains(
			reinterpret_cast<std::uintptr_t>(address), length) ||
		 uml::runtime::NativeAddressSpace::needs_shadow(
			reinterpret_cast<std::uintptr_t>(address), 0, length)) ?
			uml::runtime::native_address_space().unmap(
				reinterpret_cast<std::uintptr_t>(address), length) :
			munmap(address, length);
}

extern "C" int darwin_uml_vm_drop(void *address, unsigned long length)
{
	return guest_address(address) ? 0 :
		uml::runtime::native_address_space().flush(
			reinterpret_cast<std::uintptr_t>(address), length);
}

extern "C" int darwin_uml_vm_activate(unsigned long generation)
{
	return darwin_uml_static_vm_activate(generation);
}

extern "C" int darwin_uml_vm_batch_begin(unsigned long generation)
{
	return darwin_uml_static_vm_batch_begin(generation);
}

extern "C" int darwin_uml_vm_batch_end(void)
{
	return darwin_uml_static_vm_batch_end();
}

extern "C" int darwin_uml_vm_walker_lock(unsigned long generation)
{
	return darwin_uml_static_vm_walker_lock(generation);
}

extern "C" int darwin_uml_vm_walker_unlock(unsigned long generation)
{
	return darwin_uml_static_vm_walker_unlock(generation);
}

extern "C" int darwin_uml_vm_destroy(unsigned long generation)
{
	return darwin_uml_static_vm_destroy(generation);
}

extern "C" int darwin_uml_vm_flush_active(void) { return 0; }
extern "C" int darwin_uml_vm_flush_generation(unsigned long) { return 0; }
extern "C" int darwin_uml_vm_refresh_active(void) { return 0; }
extern "C" int darwin_uml_vm_sync_to_backing(unsigned long) { return 0; }
extern "C" int darwin_uml_vm_sync_from_backing(unsigned long address)
{
	return uml::runtime::native_address_space().refresh(address, 4096);
}

extern "C" int darwin_uml_vm_sync_from_buffer(
	unsigned long address, const void *buffer, unsigned long length)
{
	return darwin_uml_static_vm_copy_from_buffer(address, buffer, length);
}

extern "C" int darwin_uml_vm_mapping_matches(
	unsigned long address, int fd, unsigned long long offset)
{
	return darwin_uml_static_vm_mapping_matches(address, fd, offset);
}

extern "C" int darwin_uml_vm_rebind(
	unsigned long address, int fd, unsigned long long offset)
{
	return darwin_uml_static_vm_rebind(address, fd, offset);
}

extern "C" int darwin_uml_vm_stage_stack(unsigned long) { return 0; }
extern "C" int darwin_uml_vm_finish_stack(unsigned long) { return 0; }

extern "C" int darwin_uml_vm_has_mapping(unsigned long address)
{
	return darwin_uml_static_vm_has_mapping(address);
}

extern "C" int darwin_uml_vm_retry_fault(
	unsigned long address, int execute, int write)
{
	return address < guest_address_limit ?
		darwin_uml_static_vm_retry_fault(address, execute, write) :
		uml::runtime::native_address_space().retry(
			address, execute != 0, write != 0);
}

extern "C" int darwin_uml_begin_write_single_step(
	unsigned long, unsigned long)
{
	return -1;
}
