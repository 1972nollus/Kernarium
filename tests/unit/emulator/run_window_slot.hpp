// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "native_window_test_support.hpp"
#include "require.hpp"
#include "testing.hpp"
#include "uml_darwin_abi.h"

#include <cstddef>
#include <cstdint>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <unistd.h>

namespace uml::testing {

/* A run maps its kernel window where an embedded kernel's run does; default
 * options give the guest 256 MiB. */
inline constexpr std::size_t run_guest_memory = 256U * 1024U * 1024U;

/* A host page reserved at the first free address of the native window that a
 * run takes. A test releases it before the run, so that the run's kernel maps
 * its guest page at page(), and asks retired() afterwards; a slot never
 * released is returned when it is destroyed. */
class RunWindowSlot final {
public:
	RunWindowSlot() : size_{static_cast<std::size_t>(getpagesize())}
	{
		const auto window = ::uml::emulator::testing::native_kernel_window(
			run_guest_memory);
		require(window.start != 0, "could not derive the run's native window");
		for (auto candidate = window.start;
		     address_ == 0 && candidate <= window.end - size_;
		     candidate += size_) {
			mach_vm_address_t reservation = candidate;
			if (mach_vm_allocate(mach_task_self(), &reservation, size_,
				    VM_FLAGS_FIXED) == KERN_SUCCESS)
				address_ = static_cast<std::uintptr_t>(reservation);
		}
		require(address_ != 0,
			"could not reserve a host slot in the run's native window");
	}

	~RunWindowSlot()
	{
		if (!released_)
			(void)mach_vm_deallocate(mach_task_self(), address_, size_);
	}

	RunWindowSlot(const RunWindowSlot &) = delete;
	RunWindowSlot &operator=(const RunWindowSlot &) = delete;

	void release()
	{
		require(!released_ &&
			mach_vm_deallocate(mach_task_self(), address_, size_) ==
				KERN_SUCCESS,
			"could not release the native window slot");
		released_ = true;
	}

	/* The guest page one page into the slot. */
	[[nodiscard]] std::uintptr_t page() const noexcept
	{
		return address_ + DARWIN_UML_GUEST_PAGE_SIZE;
	}

	/* Retirement hands the slot back to the run's window reservation, so it
	 * carries no mapping and is not free. */
	[[nodiscard]] bool retired() const noexcept
	{
		return released_into_native_window(address_, size_);
	}

private:
	std::uintptr_t address_{};
	std::size_t size_{};
	bool released_{};
};

} // namespace uml::testing
