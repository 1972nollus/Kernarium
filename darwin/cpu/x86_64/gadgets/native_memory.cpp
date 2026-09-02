// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_entry.hpp"
#include "executor.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>

namespace uml::cpu::x86_64::gadgets {
namespace {

constexpr std::uint64_t page_mask = ~std::uint64_t{4095};
constexpr std::uint32_t maximum_bulk_size = 832;
constexpr std::uint64_t write_code_tag = UML_X86_64_TLB_WRITE_CODE;
constexpr std::uint64_t write_cached_code_tag =
	UML_X86_64_TLB_WRITE_CACHED_CODE;
constexpr std::uint8_t write_code_published = UML_X86_64_WRITE_CODE_PUBLISHED;


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

void invalidate_cached_code(NativeExecutionContext &context,
	std::uint64_t address, std::size_t size) noexcept
{
	if (context.sequence_context != nullptr)
		static_cast<Executor *>(context.sequence_context)
			->invalidate_cached_code(address, size);
}

void publish_code_write(NativeExecutionContext &context,
	std::uint64_t address, std::size_t size) noexcept
{
	if (context.memory == nullptr || size == 0 ||
	    (context.write_code == write_code_published &&
		    context.sequence_context == nullptr))
		return;
	std::size_t completed = 0;
	while (completed < size) {
		const auto current = address + completed;
		const auto in_page = static_cast<std::size_t>(current & ~page_mask);
		const auto count = std::min(size - completed,
			static_cast<std::size_t>(4096) - in_page);
		if (context.memory->is_executable(current)) {
			invalidate_cached_code(context, address, size);
			if (context.write_code != write_code_published) {
				context.memory->invalidate_code(
					current, context.execution_context);
				++context.code_publications;
			}
			context.write_code = write_code_published;
			return;
		}
		completed += count;
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
	auto &entry = context->tlb_entries[native_tlb_index(address)];
	const bool writes_code = access != 0 &&
		context->memory->is_executable(address);
	if (writes_code &&
		context->write_code != UML_X86_64_WRITE_CODE_PUBLISHED)
		context->write_code = UML_X86_64_WRITE_CODE_PENDING;
	/* Publication only has work to do when this executor holds a translation
	 * covering the page. Recording that here lets an ordinary store skip the
	 * call entirely; the executor retires this entry when the page later gains
	 * cached code.
	 */
	const bool caches_code = writes_code &&
		context->sequence_context != nullptr &&
		static_cast<Executor *>(context->sequence_context)
			->page_holds_cached_code(address);
	const auto page = address & page_mask;
	const auto delta = reinterpret_cast<std::uintptr_t>(pointer) - address;
	if (access == 0) {
		entry.read_data_minus_address = delta;
		entry.read_page = page;
	} else {
		entry.write_data_minus_address = delta;
		entry.write_page = page | (writes_code ? write_code_tag : 0) |
			(caches_code ? write_cached_code_tag : 0);
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
	if (context->memory->write(address, std::span{bytes}.first(size))) {
		publish_code_write(*context, address, size);
		return 1;
	}
	record_fault(*context, address, ::uml::memory::MemoryAccess::write);
	return 0;
}

extern "C" void uml_x86_64_gadget_publish_code_write(
	NativeExecutionContext *context, std::uint64_t address,
	std::uint32_t size) noexcept
{
	/* The write TLB tagged this page executable while resolving it. Mapping
	 * mutation is excluded by the execution gate until this block returns.
	 * Every executable store selectively cools overlapping cached blocks;
	 * only the first store in one native program publishes the global
	 * sequence change.
	 */
	if (context == nullptr || context->memory == nullptr || size == 0)
		return;
	invalidate_cached_code(*context, address, size);
	if (context->write_code != write_code_published) {
		context->memory->invalidate_code(address, context->execution_context);
		++context->code_publications;
		context->write_code = write_code_published;
	}
}

extern "C" void uml_x86_64_gadget_invalidate_code(
	NativeExecutionContext *context, std::uint64_t address) noexcept
{
	if (context != nullptr && context->memory != nullptr &&
		context->memory->is_executable(address))
		context->memory->invalidate_code(
			address, context->execution_context);
}

extern "C" std::uint32_t uml_x86_64_gadget_code_link_safe(
	NativeExecutionContext *context) noexcept
{
	if (context == nullptr || context->execution_context == nullptr ||
	    context->sequence_context == nullptr ||
	    context->code_change_reader == nullptr ||
	    context->code_publications == 0 ||
	    context->code_changes >
		std::numeric_limits<std::uint64_t>::max() -
			context->code_publications)
		return 0;
	const auto expected =
		context->code_changes + context->code_publications;
	if (context->code_change_reader(context->sequence_context) != expected)
		return 0;
	auto *executor = static_cast<Executor *>(context->sequence_context);
	if (!executor->accept_code_changes(expected))
		return 0;
	context->code_changes = expected;
	context->code_publications = 0;
	return 1U;
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
		static_cast<const std::byte *>(source), size})) {
		publish_code_write(*context, address, size);
		return 1;
	}
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
			static_cast<const std::byte *>(source), size})) {
			publish_code_write(*context, address, size);
			return 1;
		}
	} catch (...) {
	}
	record_fault(*context, address, ::uml::memory::MemoryAccess::write);
	return 0;
}

} // namespace uml::cpu::x86_64::gadgets
