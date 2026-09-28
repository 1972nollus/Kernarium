// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <limits>
#include <mach/mach.h>
#include <stdexcept>

namespace uml::testing {

[[nodiscard]] inline vm_prot_t current_vm_protection(const void *pointer)
{
	vm_address_t address = reinterpret_cast<vm_address_t>(pointer);
	vm_size_t size = 0;
	vm_region_basic_info_data_64_t info{};
	mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object = MACH_PORT_NULL;
	const auto result = vm_region_64(mach_task_self(), &address, &size,
		VM_REGION_BASIC_INFO_64,
		reinterpret_cast<vm_region_info_t>(&info), &count, &object);
	if (object != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), object);
	if (result != KERN_SUCCESS)
		throw std::runtime_error{"cannot query Darwin VM protection"};
	return info.protection;
}

[[nodiscard]] inline bool writable_and_executable(vm_prot_t protection)
{
	return (protection & (VM_PROT_WRITE | VM_PROT_EXECUTE)) ==
		(VM_PROT_WRITE | VM_PROT_EXECUTE);
}

[[nodiscard]] inline bool process_has_writable_executable_region()
{
	vm_address_t cursor = 0;
	for (;;) {
		vm_address_t address = cursor;
		vm_size_t size = 0;
		vm_region_basic_info_data_64_t info{};
		mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object = MACH_PORT_NULL;
		const auto result = vm_region_64(mach_task_self(), &address, &size,
			VM_REGION_BASIC_INFO_64,
			reinterpret_cast<vm_region_info_t>(&info), &count, &object);
		if (object != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(), object);
		if (result == KERN_INVALID_ADDRESS)
			return false;
		if (result != KERN_SUCCESS || size == 0)
			throw std::runtime_error{"cannot enumerate Darwin VM protections"};
		if (writable_and_executable(info.protection))
			return true;
		if (address > std::numeric_limits<vm_address_t>::max() - size)
			return false;
		cursor = address + size;
	}
}

} // namespace uml::testing
