// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_kernel_layout.hpp"

#include "host_environment.hpp"
#include "memory/mmu/address_layout.hpp"

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
	return DeviceGeometry::derive(guest_memory);
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
