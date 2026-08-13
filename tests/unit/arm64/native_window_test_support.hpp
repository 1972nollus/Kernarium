// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace uml::testing {

[[nodiscard]] inline std::uintptr_t reserve_native_test_window(
	std::size_t length) noexcept
{
	constexpr std::uint64_t start = DARWIN_UML_NATIVE_ADDRESS_START;
	constexpr std::uint64_t end = DARWIN_UML_NATIVE_ADDRESS_END;
	if (length == 0 || length > end - start)
		return 0;

	const auto reserve_gap = [length](mach_vm_address_t gap_start,
		mach_vm_address_t gap_end) noexcept -> std::uintptr_t {
		if (gap_end < gap_start || length > gap_end - gap_start)
			return 0;
		const auto candidate = gap_end - length;
		mach_vm_address_t reservation = candidate;
		if (mach_vm_allocate(mach_task_self(), &reservation, length,
			    VM_FLAGS_FIXED) == KERN_SUCCESS) {
			if (reservation == candidate)
				return static_cast<std::uintptr_t>(reservation);
			(void)mach_vm_deallocate(
				mach_task_self(), reservation, length);
		}
		return 0;
	};

	mach_vm_address_t cursor = start;
	while (cursor < end) {
		mach_vm_address_t region = cursor;
		mach_vm_size_t region_size = 0;
		vm_region_basic_info_data_64_t information{};
		mach_msg_type_number_t information_count =
			VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object = MACH_PORT_NULL;
		const auto status = mach_vm_region(mach_task_self(), &region,
			&region_size, VM_REGION_BASIC_INFO_64,
			reinterpret_cast<vm_region_info_t>(&information),
			&information_count, &object);
		if (object != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(), object);
		if (status == KERN_INVALID_ADDRESS)
			return reserve_gap(cursor, end);
		if (status != KERN_SUCCESS)
			return 0;

		const auto gap_end = region < end ? region : end;
		if (const auto reserved = reserve_gap(cursor, gap_end);
		    reserved != 0)
			return reserved;
		if (region >= end || region_size > end - region)
			return 0;
		const auto next = region + region_size;
		if (next <= cursor)
			return 0;
		cursor = next;
	}
	return 0;
}

[[nodiscard]] inline bool release_native_test_window(
	std::uintptr_t address, std::size_t length) noexcept
{
	return mach_vm_deallocate(mach_task_self(), address, length) ==
		KERN_SUCCESS;
}

} // namespace uml::testing
