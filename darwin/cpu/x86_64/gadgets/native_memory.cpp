// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_entry.hpp"

#include <array>
#include <cstring>
#include <span>

namespace uml::cpu::x86_64::gadgets {
namespace {

constexpr std::uint64_t page_mask = ~std::uint64_t{4095};
constexpr std::size_t tlb_mask = (1U << 13U) - 1U;
constexpr std::uint32_t maximum_bulk_size = 832;

[[nodiscard]] constexpr std::size_t tlb_index(
	std::uint64_t address) noexcept
{
	const auto page = address >> 12U;
	return static_cast<std::size_t>((page ^ (page >> 13U)) & tlb_mask);
}

void record_fault(NativeExecutionContext &context, std::uint64_t address,
	::uml::memory::MemoryAccess access) noexcept
{
	if (context.memory != nullptr && context.memory->has_fault()) {
		const auto fault = context.memory->last_fault();
		context.fault = {fault.address, fault.access};
	} else {
		context.fault = {address, access};
	}
}

} // namespace

extern "C" std::byte *uml_x86_64_gadget_tlb_miss(
	NativeExecutionContext *context, std::uint64_t address,
	std::uint32_t access, std::uint32_t size) noexcept
{
	if (context == nullptr || context->memory == nullptr ||
		context->tlb_entries == nullptr || access > 1U || size == 0U) {
		return nullptr;
	}
	const auto memory_access = access == 0 ?
		::uml::memory::MemoryAccess::read :
		::uml::memory::MemoryAccess::write;
	auto *pointer = context->memory->atomic_pointer(
		address, size, memory_access);
	if (pointer == nullptr) {
		record_fault(*context, address, memory_access);
		return nullptr;
	}
	auto &entry = context->tlb_entries[tlb_index(address)];
	const auto page = address & page_mask;
	const auto delta = reinterpret_cast<std::uintptr_t>(pointer) - address;
	if (access == 0) {
		entry.read_data_minus_address = delta;
		entry.read_page = page;
	} else {
		entry.write_data_minus_address = delta;
		entry.write_page = page;
	}
	return pointer;
}

extern "C" unsigned __int128 uml_x86_64_gadget_cross_page_load(
	NativeExecutionContext *context, std::uint64_t address,
	std::uint32_t size) noexcept
{
	std::array<std::byte, sizeof(std::uint64_t)> bytes{};
	if (context == nullptr || context->memory == nullptr || size == 0U ||
		size > bytes.size() || !context->memory->read(
			address, std::span{bytes}.first(size))) {
		if (context != nullptr)
			record_fault(*context, address,
				::uml::memory::MemoryAccess::read);
		return 0;
	}
	std::uint64_t value{};
	std::memcpy(&value, bytes.data(), size);
	return (static_cast<unsigned __int128>(1) << 64U) | value;
}

extern "C" std::uint32_t uml_x86_64_gadget_cross_page_store(
	NativeExecutionContext *context, std::uint64_t address,
	std::uint64_t value, std::uint32_t size) noexcept
{
	std::array<std::byte, sizeof(std::uint64_t)> bytes{};
	if (context == nullptr || context->memory == nullptr || size == 0U ||
		size > bytes.size())
		return 0;
	std::memcpy(bytes.data(), &value, size);
	if (context->memory->write(address, std::span{bytes}.first(size)))
		return 1;
	record_fault(*context, address, ::uml::memory::MemoryAccess::write);
	return 0;
}

extern "C" void uml_x86_64_gadget_invalidate_code(
	NativeExecutionContext *context, std::uint64_t address) noexcept
{
	if (context != nullptr && context->memory != nullptr)
		context->memory->invalidate_code(address);
}

extern "C" std::uint32_t uml_x86_64_gadget_cross_page_vector_load(
	NativeExecutionContext *context, std::uint64_t address,
	void *destination, std::uint32_t size) noexcept
{
	if (context == nullptr || context->memory == nullptr ||
		destination == nullptr ||
		(size != 8U && size != 16U && size != 32U))
		return 0;
	if (context->memory->read(address, {
		static_cast<std::byte *>(destination), size}))
		return 1;
	record_fault(*context, address, ::uml::memory::MemoryAccess::read);
	return 0;
}

extern "C" std::uint32_t uml_x86_64_gadget_cross_page_vector_store(
	NativeExecutionContext *context, std::uint64_t address,
	const void *source, std::uint32_t size) noexcept
{
	if (context == nullptr || context->memory == nullptr || source == nullptr ||
		(size != 8U && size != 10U && size != 16U && size != 32U))
		return 0;
	if (context->memory->write(address, {
		static_cast<const std::byte *>(source), size}))
		return 1;
	record_fault(*context, address, ::uml::memory::MemoryAccess::write);
	return 0;
}

extern "C" std::uint32_t uml_x86_64_gadget_bulk_load(
	NativeExecutionContext *context, std::uint64_t address,
	void *destination, std::uint32_t size) noexcept
{
	if (context == nullptr || context->memory == nullptr ||
		destination == nullptr || size == 0U || size > maximum_bulk_size)
		return 0;
	try {
		if (context->memory->read(address, {
			static_cast<std::byte *>(destination), size}))
			return 1;
	} catch (...) {
	}
	record_fault(*context, address, ::uml::memory::MemoryAccess::read);
	return 0;
}

extern "C" std::uint32_t uml_x86_64_gadget_bulk_store(
	NativeExecutionContext *context, std::uint64_t address,
	const void *source, std::uint32_t size) noexcept
{
	if (context == nullptr || context->memory == nullptr || source == nullptr ||
		size == 0U || size > maximum_bulk_size)
		return 0;
	try {
		if (context->memory->write(address, {
			static_cast<const std::byte *>(source), size}))
			return 1;
	} catch (...) {
	}
	record_fault(*context, address, ::uml::memory::MemoryAccess::write);
	return 0;
}

} // namespace uml::cpu::x86_64::gadgets
