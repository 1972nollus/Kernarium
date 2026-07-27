// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlb.hpp"

#include "../cpu_state.hpp"

#include "uml_darwin_static_mmu.h"
#include "uml_darwin_guest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace uml::backend::arm64::gadgets {
namespace {

constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;

enum class Ls64Operation {
	load,
	store,
	store_status,
	store_status_accdata,
};

std::atomic_flag ls64_lock = ATOMIC_FLAG_INIT;

class Ls64Lock final {
public:
	Ls64Lock() noexcept
	{
		while (ls64_lock.test_and_set(std::memory_order_acquire)) {
			while (ls64_lock.test(std::memory_order_relaxed))
				__asm__ volatile("yield");
		}
	}
	~Ls64Lock() { ls64_lock.clear(std::memory_order_release); }
	Ls64Lock(const Ls64Lock &) = delete;
	Ls64Lock &operator=(const Ls64Lock &) = delete;
};

template <Ls64Operation operation, bool base_sp, bool status_discard = false>
[[nodiscard]] std::uint32_t ls64_transfer(CpuState *cpu, Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	/* The gadget engine owns both pointers and guarantees that they are valid.
	 * Keep debug-grade pointer validation in the interpreter rather than adding
	 * two always-not-taken branches to every production LS64 transfer.
	 */
	const unsigned first = operand & 31U;
	const unsigned base = (operand >> 8) & 31U;
	const unsigned status = (operand >> 16) & 31U;
	const auto address = base_sp ? cpu->sp : cpu->registers[base];
	constexpr bool load = operation == Ls64Operation::load;
	if ((address & 63U) != 0) {
		tlb->fault_address = address;
		tlb->fault_access = load ? Access::read : Access::write;
		return 0;
	}

	std::array<std::byte, 64> bytes{};
	if constexpr (!load) {
		for (unsigned index = 0; index < 8; ++index) {
			auto value = cpu->registers[first + index];
			if constexpr (operation == Ls64Operation::store_status_accdata) {
				if (index == 0)
					value = (value & 0xffff'ffff'0000'0000ULL) |
						cpu->accdata;
			}
			std::memcpy(bytes.data() + index * 8, &value, sizeof(value));
		}
	}

	Ls64Lock lock;
	auto *pointer = tlb->translate(
		address, load ? Access::read : Access::write);
	if (pointer == nullptr)
		return 0;
	if constexpr (load) {
		std::memcpy(bytes.data(), pointer, bytes.size());
		for (unsigned index = 0; index < 8; ++index)
			std::memcpy(&cpu->registers[first + index],
				bytes.data() + index * 8, sizeof(std::uint64_t));
	} else {
		std::memcpy(pointer, bytes.data(), bytes.size());
		if constexpr (operation == Ls64Operation::store_status ||
			      operation == Ls64Operation::store_status_accdata) {
			if constexpr (!status_discard)
				cpu->registers[status] = 0;
		}
	}
	return 1;
}

enum class MopsOperation {
	copy_forward,
	copy_general,
	set,
};

enum class MteMemoryOperation {
	load_tag,
	store_tag,
	store_two_tags,
	store_zero_tag,
	store_zero_two_tags,
	store_pair_tag,
};

enum class MteAddressMode {
	post_index,
	signed_offset,
	pre_index,
};

[[nodiscard]] std::uint64_t &cpu_slot(
	CpuState *cpu, std::uint16_t offset) noexcept
{
	return *reinterpret_cast<std::uint64_t *>(
		reinterpret_cast<std::byte *>(cpu) + offset);
}

void mte_fault(Tlb *tlb, std::uint64_t address, Access access) noexcept
{
	tlb->fault_address = address & 0x00ff'ffff'ffff'ffffULL;
	tlb->fault_access = access;
}

[[nodiscard]] bool mte_write_data(Tlb *tlb, std::uint64_t address,
	const std::byte *bytes, std::size_t size) noexcept
{
	/* Translate every page before the first host write. A malformed mapping
	 * therefore cannot expose a partial zero/pair store. The translated fast
	 * path is at most two pages for the 16/32-byte MTE operations.
	 */
	std::array<std::byte *, 2> pointers{};
	std::array<std::size_t, 2> lengths{};
	std::size_t pages = 0;
	std::size_t completed = 0;
	while (completed != size) {
		const auto current = address + completed;
		auto *pointer = tlb->translate(current, Access::write);
		if (pointer == nullptr)
			return false;
		const auto length = std::min(size - completed,
			static_cast<std::size_t>(page_size -
			(current & (page_size - 1))));
		pointers[pages] = pointer;
		lengths[pages] = length;
		++pages;
		completed += length;
	}
	completed = 0;
	for (std::size_t page = 0; page < pages; ++page) {
		std::memcpy(pointers[page], bytes + completed, lengths[page]);
		completed += lengths[page];
	}
	return true;
}

template <MteMemoryOperation operation, MteAddressMode mode,
	bool base_sp, bool first_zero = false, bool second_zero = false>
[[nodiscard]] std::uint32_t mte_memory_transfer(CpuState *cpu, Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	/* Translation packs direct CpuState byte offsets into the operand and
	 * specializes every architectural mode. Valid production code therefore
	 * pays no register-number, opcode, writeback-mode, or XZR branches here.
	 */
	const auto base_offset = static_cast<std::uint16_t>(operand);
	const auto first_offset = static_cast<std::uint16_t>(operand >> 16);
	const auto second_offset = static_cast<std::uint16_t>(operand >> 32);
	const auto immediate = static_cast<std::int64_t>(
		static_cast<std::int16_t>(operand >> 48));
	auto &base = cpu_slot(cpu, base_offset);
	const auto base_address = base;
	constexpr bool load = operation == MteMemoryOperation::load_tag;
	if constexpr (base_sp) {
		if ((base_address & 15U) != 0) {
			mte_fault(tlb, base_address,
				load ? Access::read : Access::write);
			return 0;
		}
	}

	const auto tagged_address = mode == MteAddressMode::post_index ?
		base_address : base_address + static_cast<std::uint64_t>(immediate);
	if constexpr (load) {
		const auto address = tagged_address & 0x00ff'ffff'ffff'fff0ULL;
		std::uint8_t tag{};
		if (!tlb->read_allocation_tag(address, tag))
			return 0;
		if constexpr (!first_zero) {
			auto &destination = cpu_slot(cpu, first_offset);
			destination = (destination & ~0x0f00'0000'0000'0000ULL) |
				(static_cast<std::uint64_t>(tag & 15U) << 56);
		}
		return 1;
	}

	const auto address = tagged_address & 0x00ff'ffff'ffff'ffffULL;
	if constexpr (operation == MteMemoryOperation::store_zero_tag ||
		      operation == MteMemoryOperation::store_zero_two_tags ||
		      operation == MteMemoryOperation::store_pair_tag) {
		if ((address & 15U) != 0) {
			mte_fault(tlb, address, Access::write);
			return 0;
		}
	}

	if constexpr (operation == MteMemoryOperation::store_zero_tag ||
		      operation == MteMemoryOperation::store_zero_two_tags) {
		constexpr std::size_t size =
			operation == MteMemoryOperation::store_zero_two_tags ? 32 : 16;
		constexpr std::array<std::byte, size> zeros{};
		if (!mte_write_data(tlb, address, zeros.data(), zeros.size()))
			return 0;
	}

	std::uint8_t tag;
	if constexpr (operation == MteMemoryOperation::store_pair_tag) {
		std::array<std::byte, 16> bytes{};
		std::uint64_t first{};
		std::uint64_t second{};
		if constexpr (!first_zero)
			first = cpu_slot(cpu, first_offset);
		if constexpr (!second_zero)
			second = cpu_slot(cpu, second_offset);
		std::memcpy(bytes.data(), &first, sizeof(first));
		std::memcpy(bytes.data() + sizeof(first), &second, sizeof(second));
		if (!mte_write_data(tlb, address, bytes.data(), bytes.size()))
			return 0;
		tag = static_cast<std::uint8_t>(tagged_address >> 56) & 15U;
	} else {
		tag = static_cast<std::uint8_t>(
			cpu_slot(cpu, first_offset) >> 56) & 15U;
	}

	if (!tlb->write_allocation_tag(address, tag))
		return 0;
	if constexpr (operation == MteMemoryOperation::store_two_tags ||
		      operation == MteMemoryOperation::store_zero_two_tags) {
		if (!tlb->write_allocation_tag(address + 16, tag))
			return 0;
	}
	if constexpr (mode == MteAddressMode::post_index)
		base = base_address + static_cast<std::uint64_t>(immediate);
	else if constexpr (mode == MteAddressMode::pre_index)
		base = tagged_address;
	return 1;
}

template <MopsOperation operation>
void mops_prologue(CpuState *cpu, std::uintptr_t operand) noexcept
{
	constexpr std::uint64_t max_block = 0x7fff'ffff'ffff'ffffULL;
	constexpr std::uint64_t max_copy = 0x007f'ffff'ffff'ffffULL;
	const unsigned destination = operand & 31U;
	const unsigned count = (operand >> 8) & 31U;
	const unsigned source = (operand >> 16) & 31U;
	auto remaining = cpu->registers[count];
	if constexpr (operation == MopsOperation::copy_general) {
		if (remaining > max_copy)
			remaining = max_copy;
		const auto from = cpu->registers[source];
		const auto to = cpu->registers[destination];
		const auto from_low = from & 0x00ff'ffff'ffff'ffffULL;
		const auto to_low = to & 0x00ff'ffff'ffff'ffffULL;
		const auto from_end = static_cast<unsigned __int128>(from_low) +
			remaining;
		const bool backward = from_low < to_low && from_end > to_low;
		if (backward) {
			cpu->registers[source] = from + remaining;
			cpu->registers[destination] = to + remaining;
			cpu->nzcv = 0xa000'0000U;
		} else {
			cpu->nzcv = 0x2000'0000U;
		}
	} else {
		if ((remaining >> 63) != 0)
			remaining = max_block;
		cpu->nzcv = 0x2000'0000U;
	}
	cpu->registers[count] = remaining;
}

template <MopsOperation operation, bool source_zero = false>
[[nodiscard]] std::uint32_t mops_epilogue(CpuState *cpu, Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	constexpr std::uint64_t max_block = 0x7fff'ffff'ffff'ffffULL;
	constexpr std::uint64_t max_copy = 0x007f'ffff'ffff'ffffULL;
	const unsigned destination = operand & 31U;
	const unsigned count = (operand >> 8) & 31U;
	const unsigned source = (operand >> 16) & 31U;
	auto remaining = cpu->registers[count];
	const auto limit = operation == MopsOperation::copy_general ?
		max_copy : max_block;
	const bool backward = operation == MopsOperation::copy_general &&
		(cpu->nzcv & 0x8000'0000U) != 0;
	if (remaining > limit ||
	    (remaining != 0 && (cpu->nzcv & 0x2000'0000U) == 0) ||
	    (remaining != 0 && operation != MopsOperation::copy_general &&
	     (cpu->nzcv & 0x8000'0000U) != 0))
		return 2;

	auto to = cpu->registers[destination];
	auto from = operation == MopsOperation::set ? std::uint64_t{} :
		cpu->registers[source];
	int fill = 0;
	if constexpr (operation == MopsOperation::set && !source_zero)
		fill = static_cast<int>(cpu->registers[source] & 0xffU);
	const auto commit = [&] {
		cpu->registers[count] = remaining;
		cpu->registers[destination] = to;
		if constexpr (operation != MopsOperation::set)
			cpu->registers[source] = from;
	};
	while (remaining != 0) {
		std::size_t amount;
		std::uint64_t to_address;
		std::uint64_t from_address{};
		if (backward) {
			const auto to_available = static_cast<std::size_t>(
				to & (page_size - 1));
			amount = to_available == 0 ? page_size : to_available;
			if constexpr (operation != MopsOperation::set) {
				const auto from_available = static_cast<std::size_t>(
					from & (page_size - 1));
				amount = std::min(amount,
					from_available == 0 ?
						static_cast<std::size_t>(page_size) :
						from_available);
			}
			amount = static_cast<std::size_t>(std::min<std::uint64_t>(
				remaining, amount));
			to_address = to - amount;
			if constexpr (operation != MopsOperation::set)
				from_address = from - amount;
		} else {
			amount = page_size - static_cast<std::size_t>(
				to & (page_size - 1));
			if constexpr (operation != MopsOperation::set)
				amount = std::min(amount,
					static_cast<std::size_t>(page_size) -
					static_cast<std::size_t>(
						from & (page_size - 1)));
			amount = static_cast<std::size_t>(std::min<std::uint64_t>(
				remaining, amount));
			to_address = to;
			if constexpr (operation != MopsOperation::set)
				from_address = from;
		}

		std::byte *read_pointer{};
		if constexpr (operation != MopsOperation::set) {
			read_pointer = tlb->translate(from_address, Access::read);
			if (read_pointer == nullptr) {
				commit();
				return 0;
			}
		}
		auto *write_pointer = tlb->translate(to_address, Access::write);
		if (write_pointer == nullptr) {
			commit();
			return 0;
		}
		if constexpr (operation == MopsOperation::set)
			std::memset(write_pointer, fill, amount);
		else
			std::memmove(write_pointer, read_pointer, amount);
		if (backward) {
			to -= amount;
			if constexpr (operation != MopsOperation::set)
				from -= amount;
		} else {
			to += amount;
			if constexpr (operation != MopsOperation::set)
				from += amount;
		}
		remaining -= amount;
	}
	commit();
	return 1;
}

[[nodiscard]] std::uint32_t mops_set_tag_prologue(
	CpuState *cpu, Tlb *tlb, std::uintptr_t operand) noexcept
{
	constexpr std::uint64_t limit = 0x7fff'ffff'ffff'fff0ULL;
	const unsigned destination = operand & 31U;
	const unsigned count = (operand >> 8) & 31U;
	auto remaining = cpu->registers[count];
	if ((remaining >> 63) != 0)
		remaining = limit;
	const auto address = cpu->registers[destination];
	if ((remaining & 15U) != 0 ||
	    (remaining != 0 && (address & 15U) != 0)) {
		mte_fault(tlb, address, Access::write);
		return 0;
	}
	cpu->registers[count] = remaining;
	cpu->nzcv = 0x2000'0000U;
	return 1;
}

template <bool source_zero>
[[nodiscard]] std::uint32_t mops_set_tag_epilogue(
	CpuState *cpu, Tlb *tlb, std::uintptr_t operand) noexcept
{
	constexpr std::uint64_t limit = 0x7fff'ffff'ffff'fff0ULL;
	constexpr std::size_t max_block = 64;
	const unsigned destination = operand & 31U;
	const unsigned count = (operand >> 8) & 31U;
	const unsigned source = (operand >> 16) & 31U;
	auto remaining = cpu->registers[count];
	auto to = cpu->registers[destination];
	if (remaining > limit ||
	    (remaining != 0 && (cpu->nzcv & 0x2000'0000U) == 0) ||
	    (remaining != 0 && (cpu->nzcv & 0x8000'0000U) != 0))
		return 2;
	if (remaining != 0 && ((remaining | to) & 15U) != 0) {
		mte_fault(tlb, to, Access::write);
		return 0;
	}
	int fill = 0;
	if constexpr (!source_zero)
		fill = static_cast<int>(cpu->registers[source] & 0xffU);
	const auto commit = [&] {
		cpu->registers[count] = remaining;
		cpu->registers[destination] = to;
	};
	while (remaining != 0) {
		const auto address = to & 0x00ff'ffff'ffff'ffffULL;
		const auto page_available = static_cast<std::size_t>(
			page_size - (address & (page_size - 1)));
		const auto amount = static_cast<std::size_t>(
			std::min<std::uint64_t>(remaining,
				std::min<std::size_t>(max_block, page_available)));
		auto *pointer = tlb->translate(address, Access::write);
		if (pointer == nullptr) {
			commit();
			return 0;
		}
		std::memset(pointer, fill, amount);
		const auto tag = static_cast<std::uint8_t>(to >> 56) & 15U;
		std::size_t tags_written = 0;
		for (auto offset = amount; offset != 0; offset -= 16) {
			if (!tlb->write_allocation_tag(address + offset - 16, tag)) {
				const auto progressed = tags_written * 16;
				to += progressed;
				remaining -= progressed;
				commit();
				return 0;
			}
			++tags_written;
		}
		to += amount;
		remaining -= amount;
	}
	commit();
	return 1;
}

[[nodiscard]] constexpr std::size_t index_for(std::uint64_t address) noexcept
{
	const auto page = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
	return static_cast<std::size_t>(
		(page & (tlb_set_count - 1)) * tlb_way_count);
#else
	return static_cast<std::size_t>((page ^ (page >> 13)) &
		(tlb_set_count - 1));
#endif
}

static_assert(sizeof(TlbEntry) == 32);
static_assert(offsetof(TlbEntry, read_page) == 0);
static_assert(offsetof(TlbEntry, write_page) == 8);
static_assert(offsetof(TlbEntry, read_data_minus_address) == 16);
static_assert(offsetof(TlbEntry, write_data_minus_address) == 24);
static_assert(offsetof(Tlb, entries) == 0);
static_assert(offsetof(Tlb, generation) ==
	tlb_entry_count * sizeof(TlbEntry));
static_assert(offsetof(Tlb, changes) ==
	offsetof(Tlb, generation) + sizeof(std::uint64_t));
static_assert(offsetof(Tlb, fault_address) ==
	offsetof(Tlb, changes) + sizeof(std::uint64_t));
static_assert(offsetof(Tlb, fault_access) ==
	offsetof(Tlb, fault_address) + sizeof(std::uint64_t));

} // namespace

Tlb::Tlb()
{
	flush();
}

void Tlb::flush()
{
	std::ranges::fill(entries, TlbEntry{});
	std::ranges::fill(replacement_way, 0);
}

void Tlb::invalidate(std::uint64_t address, std::uint64_t length)
{
	const auto end = address + length;
	for (auto page = address; page < end; page += page_size) {
		const auto first = index_for(page);
		for (std::size_t way = 0; way < tlb_way_count; ++way) {
			auto &entry = entries[first + way];
			if (entry.read_page == page)
				entry.read_page = 1;
			if (entry.write_page == page)
				entry.write_page = 1;
		}
	}
}

void Tlb::prepare(
	std::uint64_t next_generation, std::uint64_t next_changes)
{
	if (generation != next_generation) {
		flush();
		generation = next_generation;
		changes = next_changes;
		return;
	}
	if (changes == next_changes)
		return;
	reconcile_changes(next_generation, next_changes);
}

/* Keep the mapping-change buffer out of prepare()'s steady-state stack frame.
 * Darwin probes this 8 KiB allocation at function entry even when the
 * generation and sequence are unchanged, which made the nominal fast path
 * show up prominently in CPU profiles.
 */
__attribute__((noinline)) void Tlb::reconcile_changes(
	std::uint64_t next_generation, std::uint64_t next_changes)
{
	std::array<darwin_uml_static_mapping_change, 256> changed_ranges{};
	std::uint64_t current_changes = next_changes;
	const int count = darwin_uml_static_vm_mapping_changes(
		next_generation, changes, changed_ranges.data(),
		changed_ranges.size(), &current_changes);
	if (count < 0) {
		flush();
	} else {
		for (int index = 0; index < count; ++index)
			invalidate(changed_ranges[index].address,
				changed_ranges[index].length);
	}
	generation = next_generation;
	changes = current_changes;
}

std::byte *Tlb::translate(std::uint64_t address, Access access)
{
	const auto page = address & ~(page_size - 1);
	const auto first = index_for(address);
	for (std::size_t way = 0; way < tlb_way_count; ++way) {
		auto &entry = entries[first + way];
		const auto cached_page = access == Access::write ?
			entry.write_page : entry.read_page;
		if (cached_page == page) {
			const auto base = access == Access::write ?
				entry.write_data_minus_address :
				entry.read_data_minus_address;
			return reinterpret_cast<std::byte *>(base + address);
		}
	}

	void *translated = nullptr;
	const auto host_access = access == Access::write ?
		DARWIN_UML_STATIC_WRITE : DARWIN_UML_STATIC_READ;
	const auto result = darwin_uml_static_vm_translate(
		static_cast<unsigned long>(generation),
		static_cast<unsigned long>(page), host_access, &translated);
	if (result != DARWIN_UML_STATIC_TRANSLATED || translated == nullptr) {
		fault_address = address;
		fault_access = access;
		return nullptr;
	}
	std::size_t selected = tlb_way_count;
	for (std::size_t way = 0; way < tlb_way_count; ++way) {
		const auto &candidate = entries[first + way];
		const auto other_page = access == Access::write ?
			candidate.read_page : candidate.write_page;
		if (other_page == page) {
			selected = way;
			break;
		}
	}
	if (selected == tlb_way_count) {
		for (std::size_t way = 0; way < tlb_way_count; ++way) {
			const auto &candidate = entries[first + way];
			const auto cached_page = access == Access::write ?
				candidate.write_page : candidate.read_page;
			if (cached_page == 1) {
				selected = way;
				break;
			}
		}
	}
	const auto set = first / tlb_way_count;
	if (selected == tlb_way_count) {
		selected = replacement_way[set];
		replacement_way[set] = static_cast<std::uint8_t>(
			(selected + 1) % tlb_way_count);
	}
	auto &entry = entries[first + selected];
	const auto host_page = reinterpret_cast<std::uintptr_t>(translated);
	if (access == Access::write) {
		entry.write_data_minus_address = host_page - page;
		entry.write_page = page;
	} else {
		entry.read_data_minus_address = host_page - page;
		entry.read_page = page;
	}
	return static_cast<std::byte *>(translated) +
		(address & (page_size - 1));
}

bool Tlb::read_allocation_tag(std::uint64_t address, std::uint8_t &tag)
{
	if (darwin_uml_static_vm_load_allocation_tag(
		generation, address, &tag) == DARWIN_UML_STATIC_TRANSLATED)
		return true;
	fault_address = address & 0x00ff'ffff'ffff'fff0ULL;
	fault_access = Access::read;
	return false;
}

bool Tlb::write_allocation_tag(std::uint64_t address, std::uint8_t tag)
{
	if (darwin_uml_static_vm_store_allocation_tag(
		generation, address, tag) == DARWIN_UML_STATIC_TRANSLATED)
		return true;
	fault_address = address & 0x00ff'ffff'ffff'fff0ULL;
	fault_access = Access::write;
	return false;
}
} // namespace uml::backend::arm64::gadgets

#define UML_LS64_WRAPPER(name, operation, base_sp, status_discard) \
extern "C" std::uint32_t uml_arm64_gadget_ls64_transfer_##name( \
	uml::backend::arm64::CpuState *cpu, \
	uml::backend::arm64::gadgets::Tlb *tlb, \
	std::uintptr_t operand) noexcept \
{ \
	using namespace uml::backend::arm64::gadgets; \
	return ls64_transfer<Ls64Operation::operation, base_sp, status_discard>( \
		cpu, tlb, operand); \
}

UML_LS64_WRAPPER(load_r, load, false, false)
UML_LS64_WRAPPER(load_s, load, true, false)
UML_LS64_WRAPPER(store_r, store, false, false)
UML_LS64_WRAPPER(store_s, store, true, false)
UML_LS64_WRAPPER(st64bv_rr, store_status, false, false)
UML_LS64_WRAPPER(st64bv_rz, store_status, false, true)
UML_LS64_WRAPPER(st64bv_sr, store_status, true, false)
UML_LS64_WRAPPER(st64bv_sz, store_status, true, true)
UML_LS64_WRAPPER(st64bv0_rr, store_status_accdata, false, false)
UML_LS64_WRAPPER(st64bv0_rz, store_status_accdata, false, true)
UML_LS64_WRAPPER(st64bv0_sr, store_status_accdata, true, false)
UML_LS64_WRAPPER(st64bv0_sz, store_status_accdata, true, true)

#undef UML_LS64_WRAPPER

#define UML_MOPS_PROLOGUE_WRAPPER(name, operation) \
extern "C" void uml_arm64_gadget_mops_prologue_transfer_##name( \
	uml::backend::arm64::CpuState *cpu, std::uintptr_t operand) noexcept \
{ \
	using namespace uml::backend::arm64::gadgets; \
	mops_prologue<MopsOperation::operation>(cpu, operand); \
}

#define UML_MOPS_EPILOGUE_WRAPPER(name, operation, source_zero) \
extern "C" std::uint32_t uml_arm64_gadget_mops_epilogue_transfer_##name( \
	uml::backend::arm64::CpuState *cpu, \
	uml::backend::arm64::gadgets::Tlb *tlb, \
	std::uintptr_t operand) noexcept \
{ \
	using namespace uml::backend::arm64::gadgets; \
	return mops_epilogue<MopsOperation::operation, source_zero>( \
		cpu, tlb, operand); \
}

UML_MOPS_PROLOGUE_WRAPPER(copy_forward, copy_forward)
UML_MOPS_PROLOGUE_WRAPPER(copy_general, copy_general)
UML_MOPS_PROLOGUE_WRAPPER(set, set)
UML_MOPS_EPILOGUE_WRAPPER(copy_forward, copy_forward, false)
UML_MOPS_EPILOGUE_WRAPPER(copy_general, copy_general, false)
UML_MOPS_EPILOGUE_WRAPPER(set_r, set, false)
UML_MOPS_EPILOGUE_WRAPPER(set_z, set, true)

extern "C" std::uint32_t uml_arm64_gadget_mops_set_tag_prologue_transfer(
	uml::backend::arm64::CpuState *cpu,
	uml::backend::arm64::gadgets::Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	using namespace uml::backend::arm64::gadgets;
	return mops_set_tag_prologue(cpu, tlb, operand);
}

#define UML_MOPS_SET_TAG_EPILOGUE_WRAPPER(name, source_zero) \
extern "C" std::uint32_t \
uml_arm64_gadget_mops_set_tag_epilogue_transfer_##name( \
	uml::backend::arm64::CpuState *cpu, \
	uml::backend::arm64::gadgets::Tlb *tlb, \
	std::uintptr_t operand) noexcept \
{ \
	using namespace uml::backend::arm64::gadgets; \
	return mops_set_tag_epilogue<source_zero>(cpu, tlb, operand); \
}

UML_MOPS_SET_TAG_EPILOGUE_WRAPPER(r, false)
UML_MOPS_SET_TAG_EPILOGUE_WRAPPER(z, true)

#undef UML_MOPS_SET_TAG_EPILOGUE_WRAPPER

#undef UML_MOPS_PROLOGUE_WRAPPER
#undef UML_MOPS_EPILOGUE_WRAPPER

#define UML_MTE_MEMORY_WRAPPER(name, operation, mode, base_sp, first_zero, \
	second_zero) \
extern "C" std::uint32_t uml_arm64_gadget_mte_memory_transfer_##name( \
	uml::backend::arm64::CpuState *cpu, \
	uml::backend::arm64::gadgets::Tlb *tlb, \
	std::uintptr_t operand) noexcept \
{ \
	using namespace uml::backend::arm64::gadgets; \
	return mte_memory_transfer<MteMemoryOperation::operation, \
		MteAddressMode::mode, base_sp, first_zero, second_zero>( \
			cpu, tlb, operand); \
}

UML_MTE_MEMORY_WRAPPER(ldg_rr, load_tag, signed_offset, false, false, false)
UML_MTE_MEMORY_WRAPPER(ldg_rz, load_tag, signed_offset, false, true, false)
UML_MTE_MEMORY_WRAPPER(ldg_sr, load_tag, signed_offset, true, false, false)
UML_MTE_MEMORY_WRAPPER(ldg_sz, load_tag, signed_offset, true, true, false)

#define UML_MTE_TAG_STORE_MODES(name, operation) \
	UML_MTE_MEMORY_WRAPPER(name##_post_r, operation, post_index, false, \
		false, false) \
	UML_MTE_MEMORY_WRAPPER(name##_post_s, operation, post_index, true, \
		false, false) \
	UML_MTE_MEMORY_WRAPPER(name##_offset_r, operation, signed_offset, false, \
		false, false) \
	UML_MTE_MEMORY_WRAPPER(name##_offset_s, operation, signed_offset, true, \
		false, false) \
	UML_MTE_MEMORY_WRAPPER(name##_pre_r, operation, pre_index, false, \
		false, false) \
	UML_MTE_MEMORY_WRAPPER(name##_pre_s, operation, pre_index, true, \
		false, false)

UML_MTE_TAG_STORE_MODES(stg, store_tag)
UML_MTE_TAG_STORE_MODES(st2g, store_two_tags)
UML_MTE_TAG_STORE_MODES(stzg, store_zero_tag)
UML_MTE_TAG_STORE_MODES(stz2g, store_zero_two_tags)

#undef UML_MTE_TAG_STORE_MODES

#define UML_MTE_STGP_REGISTERS(mode_name, mode, base_name, base_sp) \
	UML_MTE_MEMORY_WRAPPER(stgp_##mode_name##_##base_name##rr, \
		store_pair_tag, mode, base_sp, false, false) \
	UML_MTE_MEMORY_WRAPPER(stgp_##mode_name##_##base_name##rz, \
		store_pair_tag, mode, base_sp, false, true) \
	UML_MTE_MEMORY_WRAPPER(stgp_##mode_name##_##base_name##zr, \
		store_pair_tag, mode, base_sp, true, false) \
	UML_MTE_MEMORY_WRAPPER(stgp_##mode_name##_##base_name##zz, \
		store_pair_tag, mode, base_sp, true, true)

#define UML_MTE_STGP_MODE(mode_name, mode) \
	UML_MTE_STGP_REGISTERS(mode_name, mode, r, false) \
	UML_MTE_STGP_REGISTERS(mode_name, mode, s, true)

UML_MTE_STGP_MODE(post, post_index)
UML_MTE_STGP_MODE(offset, signed_offset)
UML_MTE_STGP_MODE(pre, pre_index)

#undef UML_MTE_STGP_MODE
#undef UML_MTE_STGP_REGISTERS
#undef UML_MTE_MEMORY_WRAPPER

extern "C" std::uint32_t uml_arm64_gadget_memory_transfer(
	uml::backend::arm64::gadgets::Tlb *tlb, std::uint64_t address,
	void *data, std::size_t size, std::uint32_t access) noexcept
{
	using uml::backend::arm64::gadgets::Access;
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

extern "C" void *uml_arm64_gadget_tlb_miss(
	uml::backend::arm64::gadgets::Tlb *tlb, std::uint64_t address,
	std::uint32_t access)
{
	if (tlb == nullptr)
		return nullptr;
	return tlb->translate(address,
		access == static_cast<std::uint32_t>(
			uml::backend::arm64::gadgets::Access::write) ?
			uml::backend::arm64::gadgets::Access::write :
			uml::backend::arm64::gadgets::Access::read);
}
