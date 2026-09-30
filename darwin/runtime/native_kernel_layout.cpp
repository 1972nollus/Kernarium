// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_kernel_layout.hpp"

#include "host_environment.hpp"
#include "memory/mmu/address_layout.hpp"

#include <cstdio>

#include <sys/mman.h>

namespace uml::runtime {
namespace {

/* Where the kernel goes in a macOS process. The low canonical range is already
 * spoken for there, so the image sits high, above every ordinary allowance. */
using MacosGeometry = NativeKernelLayoutGeometry<
	DARWIN_UML_NATIVE_LINK_ADDRESS,
	DARWIN_UML_GUEST_ADDRESS_LIMIT,
	DARWIN_UML_NATIVE_AUXILIARY_MINIMUM,
	DARWIN_UML_NATIVE_AUXILIARY_MAXIMUM,
	DARWIN_UML_GUEST_PAGE_SIZE,
	DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION,
	DARWIN_UML_NATIVE_STUB_PAGES>;

/* Where it goes on an iOS device, packed against the signed iOS envelope. */
using DeviceGeometry = NativeKernelLayoutGeometry<
	DARWIN_UML_NATIVE_LINK_ADDRESS,
	DARWIN_UML_NATIVE_ADDRESS_CEILING,
	DARWIN_UML_NATIVE_AUXILIARY_MINIMUM,
	DARWIN_UML_NATIVE_AUXILIARY_MAXIMUM,
	DARWIN_UML_GUEST_PAGE_SIZE,
	DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION,
	DARWIN_UML_NATIVE_STUB_PAGES>;

} // namespace

NativeKernelLayout prepare_native_kernel_layout(
	std::size_t guest_memory) noexcept
{
#if DARWIN_UML_NATIVE_HIGH_PLACEMENT
	return MacosGeometry::derive_at(
		guest_memory, DARWIN_UML_MACOS_NATIVE_ADDRESS);
#else
	/* The same iOS build runs on an Apple silicon Mac in Designed for iPad
	 * mode, where the process is a macOS process: the device window below
	 * the signed iOS envelope is already occupied there, and Darwin answers
	 * the loader's address hint with somewhere else entirely, which the
	 * loader must reject. Placement follows the address space the process
	 * actually has rather than the SDK it was built against.
	 */
	if (::uml::platform::running_ios_app_on_macos()) {
		return MacosGeometry::derive_at(
			guest_memory, DARWIN_UML_MACOS_NATIVE_ADDRESS);
	}
	const auto packed = DeviceGeometry::derive(guest_memory);
	if (!packed.valid)
		return packed;
	const auto reserved = reserve_native_kernel_window(packed);
	if (!reserved.valid) {
		std::fprintf(stderr,
			"UML: cannot reserve a %llu-byte native kernel window\n",
			static_cast<unsigned long long>(
				packed.window_end - packed.window_start));
		return {.reservation_failed = true};
	}
	return reserved;
#endif
}

NativeKernelLayout reserve_native_kernel_window(
	const NativeKernelLayout &geometry) noexcept
{
	if (!geometry.valid || geometry.window_end <= geometry.window_start)
		return {};
	const std::size_t length = static_cast<std::size_t>(
		geometry.window_end - geometry.window_start);
	/* Reserve the window before the guest is brought up so nothing else can
	 * take part of it. The address the geometry names is tried first, so a
	 * host that still offers it keeps the established layout. iOS 27 does
	 * not: an app's usable virtual range moved, no hint inside the
	 * compile-time envelope is honoured any more, and the kernel image would
	 * be placed outside the window. Such a host is asked where a window of
	 * this size fits instead of being told.
	 *
	 * Nothing inside the window is released while it lives, not even for
	 * the kernel image: Darwin prefers a freshly freed range for its next
	 * unhinted allocation, and a host allocation that landed there would be
	 * replaced by the next MAP_FIXED guest mapping without either side being
	 * told. The loader and guest RAM map over the reservation in place.
	 */
	void *reserved = mmap(reinterpret_cast<void *>(geometry.window_start),
		length, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (reserved != MAP_FAILED &&
	    reinterpret_cast<std::uintptr_t>(reserved) != geometry.window_start) {
		munmap(reserved, length);
		reserved = mmap(nullptr, length, PROT_NONE,
			MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	}
	if (reserved == MAP_FAILED)
		return {};
	const auto base = reinterpret_cast<std::uintptr_t>(reserved);
	if ((base & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)) != 0U ||
	    base < DARWIN_UML_NATIVE_LINK_ADDRESS) {
		munmap(reserved, length);
		return {};
	}
	return {
		.load_bias = base - DARWIN_UML_NATIVE_LINK_ADDRESS,
		.window_start = base,
		.window_end = base + length,
		.valid = true,
		.reserved = true,
	};
}

NativeKernelLayoutScope::NativeKernelLayoutScope(
	const NativeKernelLayout &layout) noexcept
{
	if (!layout.valid)
		return;
	entered_ = ::uml::memory::configure_native_address_window(
		layout.window_start, layout.window_end);
}

NativeKernelLayoutScope::~NativeKernelLayoutScope()
{
	if (entered_)
		::uml::memory::reset_native_address_window();
}

} // namespace uml::runtime
