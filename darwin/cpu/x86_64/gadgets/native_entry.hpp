// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "memory/guest_address_space.hpp"
#include "cpu/x86_64/layout/assembly_offsets.h"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::x86_64::gadgets {

struct IndirectLink;
struct TargetLinkEntry;
class CodePageRegistry;

struct alignas(32) NativeTlbEntry final {
	std::uint64_t read_page{1};
	std::uintptr_t read_data_minus_address{};
	std::uint64_t write_page{1};
	std::uintptr_t write_data_minus_address{};
};

inline constexpr std::size_t native_tlb_entry_count = 1U << 15U;

/* The executor retires a write translation when a page gains cached code, so
 * it needs the same index the miss handler fills.
 */
[[nodiscard]] inline constexpr std::size_t native_tlb_index(
	std::uint64_t address) noexcept
{
	const auto page = address >> 12U;
	return static_cast<std::size_t>(
		(page ^ (page >> 15U)) & (native_tlb_entry_count - 1U));
}

struct NativeExecutionContext final {
	::uml::memory::GuestAddressSpace *memory{};
	NativeTlbEntry *tlb_entries{};
	std::uint64_t program_begin{};
	std::uint64_t program_end{};
	::uml::cpu::FaultInfo fault{};
	std::uint8_t write_code{};
	/* The host takes an exclusive access at any alignment inside one 16-byte
	 * granule (FEAT_LSE2); without it an unaligned one is an alignment fault. */
	std::uint8_t unaligned_exclusives{};
	/* This run holds the lock that serializes locked operands no exclusive
	 * access can cover; the fault tail releases it. */
	std::uint8_t split_lock{};
	std::size_t instruction_limit{};
	IndirectLink *indirect_miss{};
	const TargetLinkEntry *target_links{};
	void *execution_context{};
	void *sequence_context{};
	std::uint64_t (*code_change_reader)(void *) noexcept{};
	std::uint64_t code_changes{};
	std::uint64_t code_publications{};
	/* Pages holding a cached translation in any executor of this address
	 * space; null publishes on every executable-page store. */
	CodePageRegistry *code_pages{};
	/* The TLB entry the last stack access translated through. A stack access
	 * re-checks this entry's tag before hashing its address, and a plain-page
	 * tag lets it skip the store epilogue; it is never null. */
	NativeTlbEntry *stack_entry{};
	/* The executable page this program's last publication covered, or no page.
	 * A published record is page granular, so a further store inside that page
	 * owes nothing; a store to any other page owes its own record. A followed
	 * link that settles this program's publications puts it back to no page,
	 * so this is exactly the set of pages `write_code` still speaks for. */
	std::uint64_t code_page{UML_X86_64_CODE_PAGE_NONE};
};

static_assert(sizeof(NativeTlbEntry) == 32);
static_assert(offsetof(NativeTlbEntry, read_page) ==
	UML_X86_64_TLB_READ_PAGE);
static_assert(offsetof(NativeTlbEntry, read_data_minus_address) ==
	UML_X86_64_TLB_READ_DATA_MINUS_ADDRESS);
static_assert(offsetof(NativeTlbEntry, write_page) ==
	UML_X86_64_TLB_WRITE_PAGE);
static_assert(offsetof(NativeTlbEntry, write_data_minus_address) ==
	UML_X86_64_TLB_WRITE_DATA_MINUS_ADDRESS);
static_assert(offsetof(NativeExecutionContext, memory) ==
	UML_X86_64_CONTEXT_MEMORY);
static_assert(offsetof(NativeExecutionContext, tlb_entries) ==
	UML_X86_64_CONTEXT_TLB);
static_assert(offsetof(NativeExecutionContext, program_begin) ==
	UML_X86_64_CONTEXT_PROGRAM_BEGIN);
static_assert(offsetof(NativeExecutionContext, program_end) ==
	UML_X86_64_CONTEXT_PROGRAM_END);
static_assert(offsetof(NativeExecutionContext, fault) ==
	UML_X86_64_CONTEXT_FAULT_ADDRESS);
static_assert(offsetof(NativeExecutionContext, write_code) ==
	UML_X86_64_CONTEXT_WRITE_CODE);
/* A store translation asks whether a publication is still owed by testing this
 * field's low bit, so only the pending state may carry it. */
static_assert((UML_X86_64_WRITE_CODE_PENDING & 1U) == 1U &&
	(UML_X86_64_WRITE_CODE_PUBLISHED & 1U) == 0U);
static_assert(offsetof(NativeExecutionContext, unaligned_exclusives) ==
	UML_X86_64_CONTEXT_UNALIGNED_EXCLUSIVES);
static_assert(offsetof(NativeExecutionContext, split_lock) ==
	UML_X86_64_CONTEXT_SPLIT_LOCK);
static_assert(offsetof(NativeExecutionContext, instruction_limit) ==
	UML_X86_64_CONTEXT_INSTRUCTION_LIMIT);
static_assert(offsetof(NativeExecutionContext, indirect_miss) ==
	UML_X86_64_CONTEXT_INDIRECT_MISS);
static_assert(offsetof(NativeExecutionContext, target_links) ==
	UML_X86_64_CONTEXT_TARGET_LINKS);
static_assert(offsetof(NativeExecutionContext, stack_entry) ==
	UML_X86_64_CONTEXT_STACK_ENTRY);
static_assert(offsetof(NativeExecutionContext, code_page) ==
	UML_X86_64_CONTEXT_CODE_PAGE);

struct NativeEngineResult final {
	std::int32_t event;
	std::uint32_t reserved;
	std::size_t executed;
};

extern "C" NativeEngineResult uml_x86_64_gadget_enter(
	const std::uintptr_t *program, CpuState *cpu,
	NativeExecutionContext *context);

extern "C" std::byte *uml_x86_64_gadget_tlb_miss(
	NativeExecutionContext *context, std::uint64_t address,
	std::uint32_t access, std::uint32_t size) noexcept;
extern "C" unsigned __int128 uml_x86_64_gadget_cross_page_load(
	NativeExecutionContext *context, std::uint64_t address,
	std::uint32_t size) noexcept;
extern "C" std::uint32_t uml_x86_64_gadget_cross_page_store(
	NativeExecutionContext *context, std::uint64_t address,
	std::uint64_t value, std::uint32_t size) noexcept;
extern "C" void uml_x86_64_gadget_publish_code_write(
	NativeExecutionContext *context, std::uint64_t address,
	std::uint32_t size) noexcept;
extern "C" void uml_x86_64_gadget_invalidate_code(
	NativeExecutionContext *context, std::uint64_t address) noexcept;
extern "C" void uml_x86_64_gadget_split_lock_enter(
	NativeExecutionContext *context) noexcept;
extern "C" void uml_x86_64_gadget_split_lock_leave(
	NativeExecutionContext *context) noexcept;
extern "C" std::uint32_t uml_x86_64_gadget_code_link_safe(
	NativeExecutionContext *context) noexcept;
extern "C" std::uint32_t uml_x86_64_gadget_cross_page_vector_load(
	NativeExecutionContext *context, std::uint64_t address,
	void *destination, std::uint32_t size) noexcept;
extern "C" std::uint32_t uml_x86_64_gadget_cross_page_vector_store(
	NativeExecutionContext *context, std::uint64_t address,
	const void *source, std::uint32_t size) noexcept;
extern "C" std::uint32_t uml_x86_64_gadget_bulk_load(
	NativeExecutionContext *context, std::uint64_t address,
	void *destination, std::uint32_t size) noexcept;
extern "C" std::uint32_t uml_x86_64_gadget_bulk_store(
	NativeExecutionContext *context, std::uint64_t address,
	const void *source, std::uint32_t size) noexcept;

} // namespace uml::cpu::x86_64::gadgets
