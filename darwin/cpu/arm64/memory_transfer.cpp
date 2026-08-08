// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory_transfer.hpp"

#include "uml_darwin_guest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" std::uint32_t uml_arm64_memory_transfer(
	::uml::memory::Tlb *tlb, std::uint64_t address,
	void *data, std::size_t size, std::uint32_t access) noexcept
{
	using ::uml::memory::Access;
	constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
	if (tlb == nullptr || data == nullptr || size == 0 || size > 64)
		return 0;

	const auto direction = access == static_cast<std::uint32_t>(Access::write) ?
		Access::write : Access::read;
	std::array<std::byte *, 2> pages{};
	std::array<std::size_t, 2> lengths{};
	std::size_t count = 0;
	std::size_t completed = 0;
	while (completed < size) {
		if (count == pages.size())
			return 0;
		const auto current = address + completed;
		auto *translated = tlb->translate(current, direction);
		if (translated == nullptr)
			return 0;
		const auto length = std::min(size - completed,
			page_size - static_cast<std::size_t>(current % page_size));
		pages[count] = translated;
		lengths[count] = length;
		++count;
		completed += length;
	}

	completed = 0;
	auto *bytes = static_cast<std::byte *>(data);
	for (std::size_t index = 0; index < count; ++index) {
		if (direction == Access::write)
			std::memcpy(pages[index], bytes + completed, lengths[index]);
		else
			std::memcpy(bytes + completed, pages[index], lengths[index]);
		completed += lengths[index];
	}
	return 1;
}

extern "C" std::uint32_t uml_arm64_simd_structure_memory_transfer(
	uml::cpu::arm64::CpuState *cpu,
	::uml::memory::Tlb *tlb, std::uint64_t tagged_address,
	void *data, std::size_t size, std::uint32_t access,
	std::uint32_t base_sp, std::uint32_t options) noexcept
{
	using ::uml::memory::Access;
	if (cpu == nullptr || tlb == nullptr)
		return 0;
	const auto direction =
		access == static_cast<std::uint32_t>(Access::write) ?
			Access::write : Access::read;
	const bool tagged = base_sp == 0U && cpu->mte_enabled;
	const auto address = tagged ?
		tagged_address & 0x00ff'ffff'ffff'ffffULL : tagged_address;
	const auto element_size = static_cast<std::size_t>(options >> 8);
	const bool tagchecked = (options & 1U) != 0U;
	if (element_size == 0U || element_size > 32U)
		return 0;
	if (tagchecked && cpu->mte_enabled && !cpu->pstate_tco) {
		const auto logical_tag = tagged ?
			static_cast<std::uint8_t>(tagged_address >> 56) & 15U : 0U;
		for (std::size_t completed = 0; completed < size;
		     completed += element_size) {
			const auto current = address + completed;
			const auto access_size =
				std::min(element_size, size - completed);
			for (std::size_t translated = 0;
			     translated < access_size;) {
				const auto translated_address = current + translated;
				if (tlb->translate(translated_address, direction) == nullptr)
					return 0;
				translated += std::min<std::size_t>(
					access_size - translated,
					DARWIN_UML_GUEST_PAGE_SIZE -
						(translated_address &
						 (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));
			}
			for (std::size_t checked = 0; checked < access_size;) {
				const auto tagged_current = current + checked;
				std::uint8_t allocation_tag{};
				if (!tlb->read_allocation_tag(
					    tagged_current, allocation_tag) ||
				    allocation_tag != logical_tag) {
					tlb->fault_address = tagged_current;
					tlb->fault_access = direction;
					return 0;
				}
				checked += std::min<std::size_t>(
					access_size - checked,
					16U - (tagged_current & 15U));
			}
		}
	}
	const auto transferred = uml_arm64_memory_transfer(
		tlb, address, data, size, access);
	if (transferred != 0U && direction == Access::write)
		cpu->exclusive_size = 0;
	return transferred;
}

extern "C" std::byte *uml_arm64_atomic_floating_pointer(
	uml::cpu::arm64::CpuState *cpu,
	::uml::memory::Tlb *tlb, std::uint64_t tagged_address,
	std::size_t size, std::uint32_t base_sp) noexcept
{
	using ::uml::memory::Access;
	if (cpu == nullptr || tlb == nullptr ||
	    (size != 2U && size != 4U && size != 8U))
		return nullptr;
	if (base_sp != 0U && (tagged_address & 15U) != 0U) {
		tlb->fault_address = tagged_address;
		tlb->fault_access = Access::write;
		return nullptr;
	}
	const bool tagged = base_sp == 0U && cpu->mte_enabled;
	const auto address = tagged ?
		tagged_address & 0x00ff'ffff'ffff'ffffULL : tagged_address;
	if ((address & (size - 1U)) != 0U) {
		tlb->fault_address = address;
		tlb->fault_access = Access::write;
		return nullptr;
	}

	auto *pointer = tlb->translate(address, Access::write);
	if (pointer == nullptr)
		return nullptr;
	if (base_sp == 0U && cpu->mte_enabled && !cpu->pstate_tco) {
		std::uint8_t allocation_tag{};
		if (!tlb->read_allocation_tag(address, allocation_tag))
			return nullptr;
		const auto logical_tag =
			static_cast<std::uint8_t>(tagged_address >> 56) & 15U;
		if (allocation_tag != logical_tag) {
			tlb->fault_address = address;
			tlb->fault_access = Access::write;
			return nullptr;
		}
	}
	return pointer;
}

extern "C" void uml_arm64_alignment_fault(
	::uml::memory::Tlb *tlb, std::uint64_t address,
	std::uint32_t access) noexcept
{
	if (tlb == nullptr)
		return;
	tlb->fault_address = address;
	tlb->fault_access = access == static_cast<std::uint32_t>(
		::uml::memory::Access::write) ?
		::uml::memory::Access::write : ::uml::memory::Access::read;
}

extern "C" void *uml_arm64_tlb_miss(
	::uml::memory::Tlb *tlb, std::uint64_t address,
	std::uint32_t access)
{
	if (tlb == nullptr)
		return nullptr;
	return tlb->translate(address,
		access == static_cast<std::uint32_t>(
			::uml::memory::Access::write) ?
			::uml::memory::Access::write : ::uml::memory::Access::read);
}
