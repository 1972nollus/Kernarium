// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_guest.h"
#include "uml_darwin_memory.h"
#include "uml_darwin_static_mmu.h"
#if UML_MEMORY_HAS_NATIVE_VM && DARWIN_UML_GUEST_PAGE_SIZE == 4096
#include "native_address_space.hpp"
#endif

#include <cerrno>
#include <cstdint>
#if UML_MEMORY_HAS_NATIVE_VM
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#endif

namespace {

constexpr std::uintptr_t guest_address_limit = 0x6000'0000'0000ULL;

[[nodiscard]] bool guest_address(const void *address) noexcept
{
	return reinterpret_cast<std::uintptr_t>(address) < guest_address_limit;
}

#if UML_MEMORY_HAS_NATIVE_VM
[[nodiscard]] bool trace_enabled() noexcept
{
	static const bool enabled = [] {
		const auto *value = std::getenv("UML_MMU_TRACE");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return enabled;
}

[[nodiscard]] int write_xor_execute(int protection) noexcept
{
	if ((protection & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC))
		protection &= ~PROT_EXEC;
	return protection;
}
#else
[[nodiscard]] int native_vm_unavailable() noexcept
{
	errno = ENOTSUP;
	return -1;
}
#endif

} // namespace

extern "C" int darwin_uml_vm_map(void *address, int fd, unsigned long long offset,
				 unsigned long length, int protection, int guest_code,
				 int guest_writable)
{
	if (guest_address(address))
		return darwin_uml_static_vm_map(address, fd, offset, length, protection,
						guest_code, guest_writable);
#if UML_MEMORY_HAS_NATIVE_VM
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	const bool shadow =
		!guest_address(address) &&
		(uml::memory::NativeAddressSpace::needs_shadow(
			 reinterpret_cast<std::uintptr_t>(address), offset, length) ||
		 uml::memory::native_address_space().contains(
			 reinterpret_cast<std::uintptr_t>(address), length));
#endif
	if (trace_enabled() && !guest_address(address))
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
		std::fprintf(
			stderr,
			"Darwin native map address=%p fd=%d offset=%#llx length=%#lx "
			"protection=%#x shadow=%d\n",
			address, fd, offset, length, protection, shadow);
#else
		std::fprintf(stderr,
			     "Darwin native direct map address=%p fd=%d offset=%#llx "
			     "length=%#lx protection=%#x\n",
			     address, fd, offset, length, protection);
#endif
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	if (shadow)
		return uml::memory::native_address_space().map(
			reinterpret_cast<std::uintptr_t>(address), fd, offset, length,
			protection);
#endif
	protection = write_xor_execute(protection);
	const auto result = mmap(address, length, protection, MAP_FIXED | MAP_SHARED,
				 fd, offset) == MAP_FAILED
				    ? -1
				    : 0;
	if (trace_enabled())
		std::fprintf(
			stderr,
			"Darwin static native map address=%p offset=%#llx length=%#lx "
			"protection=%#x result=%d\n",
			address, offset, length, protection, result);
	return result;
#else
	return native_vm_unavailable();
#endif
}

extern "C" int darwin_uml_vm_map_generation(unsigned long generation, void *address,
					    int fd, unsigned long long offset,
					    unsigned long length, int protection,
					    int guest_code, int guest_writable)
{
	if (!guest_address(address))
		return darwin_uml_vm_map(address, fd, offset, length, protection,
					 guest_code, guest_writable);
	return darwin_uml_static_vm_map_generation(generation, address, fd, offset,
						   length, protection, guest_code,
						   guest_writable);
}

extern "C" int darwin_uml_vm_protect(void *address, unsigned long length,
				     int protection)
{
	/* Guest execute permission is software-MMU metadata and may coexist with
	 * guest write permission.  Only sanitize the native Darwin mapping that
	 * is actually subject to the host W^X policy.
	 */
	if (guest_address(address))
		return darwin_uml_static_vm_protect(address, length, protection);
#if UML_MEMORY_HAS_NATIVE_VM
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	const bool tracked = uml::memory::native_address_space().contains(
		reinterpret_cast<std::uintptr_t>(address), length);
	if (trace_enabled())
		std::fprintf(
			stderr,
			"Darwin native protect address=%p length=%#lx protection=%#x "
			"tracked=%d\n",
			address, length, protection, tracked);
	if (tracked)
		return uml::memory::native_address_space().protect(
			reinterpret_cast<std::uintptr_t>(address), length, protection);
#endif
	return mprotect(address, length, write_xor_execute(protection));
#else
	return native_vm_unavailable();
#endif
}

extern "C" int darwin_uml_vm_unmap(void *address, unsigned long length)
{
	if (guest_address(address))
		return darwin_uml_static_vm_unmap(address, length);
#if UML_MEMORY_HAS_NATIVE_VM
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	if (trace_enabled() && !guest_address(address))
		std::fprintf(
			stderr,
			"Darwin native unmap address=%p length=%#lx tracked=%d\n",
			address, length,
			uml::memory::native_address_space().contains(
				reinterpret_cast<std::uintptr_t>(address), length));
	return (uml::memory::native_address_space().contains(
			reinterpret_cast<std::uintptr_t>(address), length) ||
		uml::memory::NativeAddressSpace::needs_shadow(
			reinterpret_cast<std::uintptr_t>(address), 0, length))
		       ? uml::memory::native_address_space().unmap(
				 reinterpret_cast<std::uintptr_t>(address), length)
		       : munmap(address, length);
#else
	return munmap(address, length);
#endif
#else
	return native_vm_unavailable();
#endif
}

extern "C" int darwin_uml_vm_unmap_generation(unsigned long generation, void *address,
					      unsigned long length)
{
	if (guest_address(address))
		return darwin_uml_static_vm_unmap_generation(generation, address,
							     length);
#if UML_MEMORY_HAS_NATIVE_VM
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	return (uml::memory::native_address_space().contains(
			reinterpret_cast<std::uintptr_t>(address), length) ||
		uml::memory::NativeAddressSpace::needs_shadow(
			reinterpret_cast<std::uintptr_t>(address), 0, length))
		       ? uml::memory::native_address_space().unmap(
				 reinterpret_cast<std::uintptr_t>(address), length)
		       : munmap(address, length);
#else
	return munmap(address, length);
#endif
#else
	return native_vm_unavailable();
#endif
}

extern "C" int darwin_uml_vm_drop(void *address, unsigned long length)
{
	if (guest_address(address))
		return 0;
#if UML_MEMORY_HAS_NATIVE_VM
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	return uml::memory::native_address_space().flush(
		reinterpret_cast<std::uintptr_t>(address), length);
#else
	(void)address;
	(void)length;
	return 0;
#endif
#else
	(void)length;
	return native_vm_unavailable();
#endif
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

extern "C" int darwin_uml_vm_flush_active(void)
{
	return 0;
}
extern "C" int darwin_uml_vm_flush_generation(unsigned long)
{
	return 0;
}
extern "C" int darwin_uml_vm_sync_to_backing(unsigned long)
{
	return 0;
}
extern "C" int darwin_uml_vm_sync_from_backing(unsigned long address)
{
#if UML_MEMORY_HAS_NATIVE_VM && DARWIN_UML_GUEST_PAGE_SIZE == 4096
	return uml::memory::native_address_space().refresh(address,
							   DARWIN_UML_GUEST_PAGE_SIZE);
#else
	(void)address;
	return 0;
#endif
}

extern "C" int darwin_uml_vm_sync_from_buffer(unsigned long address, const void *buffer,
					      unsigned long length)
{
	return darwin_uml_static_vm_copy_from_buffer(address, buffer, length);
}

extern "C" int darwin_uml_vm_mapping_matches(unsigned long address, int fd,
					     unsigned long long offset)
{
	return darwin_uml_static_vm_mapping_matches(address, fd, offset);
}

extern "C" int darwin_uml_vm_rebind(unsigned long address, int fd,
				    unsigned long long offset)
{
	return darwin_uml_static_vm_rebind(address, fd, offset);
}

extern "C" int darwin_uml_vm_stage_stack(unsigned long)
{
	return 0;
}
extern "C" int darwin_uml_vm_finish_stack(unsigned long)
{
	return 0;
}

extern "C" int darwin_uml_vm_retry_fault(unsigned long address, int execute, int write)
{
	if (address < guest_address_limit)
		return darwin_uml_static_vm_retry_fault(address, execute, write);
#if UML_MEMORY_HAS_NATIVE_VM && DARWIN_UML_GUEST_PAGE_SIZE == 4096
	return uml::memory::native_address_space().retry(address, execute != 0,
							 write != 0);
#else
	return 0;
#endif
}

extern "C" int darwin_uml_begin_write_single_step(unsigned long, unsigned long)
{
	return -1;
}
