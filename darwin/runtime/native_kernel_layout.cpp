// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_kernel_layout.hpp"

#include "host_environment.hpp"
#include "memory/mmu/address_layout.hpp"

#include <sys/mman.h>

namespace uml::runtime {
namespace {

/* Where the kernel goes in a macOS process. The low canonical range is already
 * spoken for there, so the image sits high, above every ordinary allowance. */
using MacosGeometry = NativeKernelLayoutGeometry<
	DARWIN_UML_NATIVE_LINK_ADDRESS,
	DARWIN_UML_GUEST_ADDRESS_LIMIT,
	DARWIN_UML_NATIVE_AUXILIARY_DIVISOR,
	DARWIN_UML_NATIVE_AUXILIARY_MINIMUM,
	DARWIN_UML_NATIVE_AUXILIARY_MAXIMUM,
	DARWIN_UML_GUEST_PAGE_SIZE,
	DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION,
	DARWIN_UML_NATIVE_STUB_PAGES>;

/* Where it goes on an iOS device, packed against the signed iOS envelope. */
using DeviceGeometry = NativeKernelLayoutGeometry<
	DARWIN_UML_NATIVE_LINK_ADDRESS,
	DARWIN_UML_NATIVE_ADDRESS_CEILING,
	DARWIN_UML_NATIVE_AUXILIARY_DIVISOR,
	DARWIN_UML_NATIVE_AUXILIARY_MINIMUM,
	DARWIN_UML_NATIVE_AUXILIARY_MAXIMUM,
	DARWIN_UML_GUEST_PAGE_SIZE,
	DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION,
	DARWIN_UML_NATIVE_STUB_PAGES>;

} // namespace

NativeKernelLayout prepare_native_kernel_layout(
	std::size_t guest_memory) noexcept
{
#if DARWIN_UML_DYNAMIC_NATIVE_LAYOUT
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
	const std::size_t length = static_cast<std::size_t>(
		packed.window_end - packed.window_start);
	/* Reserve the window before the guest is brought up so nothing else can
	 * take part of it; guest RAM is mapped over the reservation with
	 * MAP_FIXED. The address the geometry names is tried first, so a host
	 * that still offers it keeps the established layout. iOS 27 does not:
	 * an app's usable virtual range moved, no hint inside the compile-time
	 * envelope is honoured any more, and the kernel image would be placed
	 * outside the window. Such a host is asked where a window of this size
	 * fits instead of being told.
	 */
	void *reserved = mmap(reinterpret_cast<void *>(packed.window_start),
		length, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (reserved != MAP_FAILED &&
	    reinterpret_cast<std::uintptr_t>(reserved) != packed.window_start) {
		munmap(reserved, length);
		reserved = mmap(nullptr, length, PROT_NONE,
			MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	}
	if (reserved == MAP_FAILED)
		return packed;
	const auto base = reinterpret_cast<std::uintptr_t>(reserved);
	if ((base & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)) != 0U ||
	    base < DARWIN_UML_NATIVE_LINK_ADDRESS) {
		munmap(reserved, length);
		return packed;
	}
	/* The kernel image and the reserved physical prefix behind it are placed
	 * with plain hints their loaders require to be honoured, so the window's
	 * first pages are released back to the host. The allocatable suffix
	 * currently begins 36 MiB in; the released span is sized well above that
	 * and a larger image simply fails to place rather than landing wrong.
	 */
	constexpr std::size_t prefixHole = 64U << 20;
	munmap(reserved, prefixHole < length ? prefixHole : length);
	return {
		.load_bias = base - DARWIN_UML_NATIVE_LINK_ADDRESS,
		.window_start = base,
		.window_end = base + length,
		.valid = true,
	};
#endif
#else
	(void)guest_memory;
	return {
		.load_bias = 0,
		.window_start = DARWIN_UML_NATIVE_ADDRESS_START,
		.window_end = DARWIN_UML_NATIVE_ADDRESS_END,
		.valid = true,
	};
#endif
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
