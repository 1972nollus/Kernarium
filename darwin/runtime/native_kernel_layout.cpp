// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_kernel_layout.hpp"

#include "memory/mmu/address_layout.hpp"

namespace uml::runtime {

NativeKernelLayout prepare_native_kernel_layout(
	std::size_t guest_memory) noexcept
{
#if DARWIN_UML_DYNAMIC_NATIVE_LAYOUT
#if DARWIN_UML_NATIVE_HIGH_PLACEMENT
	using Geometry = NativeKernelLayoutGeometry<
		DARWIN_UML_NATIVE_LINK_ADDRESS,
		DARWIN_UML_GUEST_ADDRESS_LIMIT,
		DARWIN_UML_NATIVE_AUXILIARY_DIVISOR,
		DARWIN_UML_NATIVE_AUXILIARY_MINIMUM,
		DARWIN_UML_NATIVE_AUXILIARY_MAXIMUM,
		DARWIN_UML_GUEST_PAGE_SIZE>;
	return Geometry::derive_at(
		guest_memory, DARWIN_UML_MACOS_NATIVE_ADDRESS);
#else
	using Geometry = NativeKernelLayoutGeometry<
		DARWIN_UML_NATIVE_LINK_ADDRESS,
		DARWIN_UML_NATIVE_ADDRESS_CEILING,
		DARWIN_UML_NATIVE_AUXILIARY_DIVISOR,
		DARWIN_UML_NATIVE_AUXILIARY_MINIMUM,
		DARWIN_UML_NATIVE_AUXILIARY_MAXIMUM,
		DARWIN_UML_GUEST_PAGE_SIZE>;
	return Geometry::derive(guest_memory);
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
