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

namespace uml::memory {
namespace {

using ::uml::cpu::arm64::CpuState;

constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;

[[nodiscard]] std::uint64_t &cpu_slot(
	CpuState *cpu, std::uint16_t offset) noexcept
{
	return *reinterpret_cast<std::uint64_t *>(
		reinterpret_cast<std::byte *>(cpu) + offset);
}

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
	/* The gadget engine owns both pointers and guarantees that they are valid,
	 * so production LS64 transfers carry no always-not-taken validation
	 * branches. Translation also packs direct CpuState byte offsets, so the
	 * hot helper does not decode architectural register numbers.
	 */
	const auto first_offset = static_cast<std::uint16_t>(operand);
	const auto base_offset = static_cast<std::uint16_t>(operand >> 16);
	const auto status_offset = static_cast<std::uint16_t>(operand >> 32);
	auto *const transfer = reinterpret_cast<std::byte *>(cpu) + first_offset;
	const auto tagged_address =
		base_sp ? cpu->sp : cpu_slot(cpu, base_offset);
	const bool tagged = !base_sp && cpu->tagged_addresses_enabled();
	const bool tagchecked = !base_sp && cpu->mte_enabled && !cpu->pstate_tco;
	const auto address = tagged ?
		tagged_address & 0x00ff'ffff'ffff'ffffULL : tagged_address;
	constexpr bool load = operation == Ls64Operation::load;
	if ((address & 63U) != 0) {
		tlb->fault_address = address;
		tlb->fault_access = load ? Access::read : Access::write;
		return 0;
	}

	Ls64Lock lock;
	auto *pointer = tlb->translate(
		address, load ? Access::read : Access::write);
	if (pointer == nullptr)
		return 0;
	if (tagchecked) {
		const auto logical_tag =
			static_cast<std::uint8_t>(tagged_address >> 56) & 15U;
		for (unsigned offset = 0; offset < 64; offset += 16) {
			std::uint8_t allocation_tag{};
			if (!tlb->read_allocation_tag(
				    address + offset, allocation_tag)) {
				tlb->fault_access =
					load ? Access::read : Access::write;
				return 0;
			}
			if (allocation_tag != logical_tag) {
				tlb->fault_address = address + offset;
				tlb->fault_access =
					load ? Access::read : Access::write;
				return 0;
			}
		}
	}
	if constexpr (load) {
		std::memcpy(transfer, pointer, 64);
	} else {
		if constexpr (operation ==
			      Ls64Operation::store_status_accdata) {
			std::array<std::byte, 64> bytes;
			std::memcpy(bytes.data(), transfer, bytes.size());
			std::memcpy(
				bytes.data(), &cpu->accdata, sizeof(cpu->accdata));
			std::memcpy(pointer, bytes.data(), bytes.size());
		} else {
			std::memcpy(pointer, transfer, 64);
		}
		if constexpr (operation == Ls64Operation::store_status ||
			      operation == Ls64Operation::store_status_accdata) {
			if constexpr (!status_discard)
				cpu_slot(cpu, status_offset) = 0;
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
	/* The static allocation-tag API accepts an address anywhere in a
	 * granule. Every architectural allocation-tag store is stricter and
	 * faults before translation unless the effective address is aligned.
	 */
	if ((address & 15U) != 0) {
		mte_fault(tlb, address, Access::write);
		return 0;
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
	const auto destination_offset = static_cast<std::uint16_t>(operand);
	const auto count_offset = static_cast<std::uint16_t>(operand >> 16);
	const auto source_offset = static_cast<std::uint16_t>(operand >> 32);
	auto &count = cpu_slot(cpu, count_offset);
	auto remaining = count;
	if constexpr (operation == MopsOperation::copy_general) {
		if (remaining > max_copy)
			remaining = max_copy;
		auto &source = cpu_slot(cpu, source_offset);
		auto &destination = cpu_slot(cpu, destination_offset);
		const auto from = source;
		const auto to = destination;
		const auto from_low = from & 0x00ff'ffff'ffff'ffffULL;
		const auto to_low = to & 0x00ff'ffff'ffff'ffffULL;
		const auto from_end = static_cast<unsigned __int128>(from_low) +
			remaining;
		const bool backward = from_low < to_low && from_end > to_low;
		if (backward) {
			source = from + remaining;
			destination = to + remaining;
			cpu->nzcv = 0xa000'0000U;
		} else {
			cpu->nzcv = 0x2000'0000U;
		}
	} else {
		if ((remaining >> 63) != 0)
			remaining = max_block;
		cpu->nzcv = 0x2000'0000U;
	}
	count = remaining;
}

template <MopsOperation operation, bool source_zero = false>
[[nodiscard]] std::uint32_t mops_epilogue(CpuState *cpu, Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	constexpr std::uint64_t max_block = 0x7fff'ffff'ffff'ffffULL;
	constexpr std::uint64_t max_copy = 0x007f'ffff'ffff'ffffULL;
	constexpr std::size_t max_transfer_block = 64;
	const auto destination_offset = static_cast<std::uint16_t>(operand);
	const auto count_offset = static_cast<std::uint16_t>(operand >> 16);
	const auto source_offset = static_cast<std::uint16_t>(operand >> 32);
	auto &destination = cpu_slot(cpu, destination_offset);
	auto &count = cpu_slot(cpu, count_offset);
	auto remaining = count;
	const auto limit = operation == MopsOperation::copy_general ?
		max_copy : max_block;
	const bool backward = operation == MopsOperation::copy_general &&
		(cpu->nzcv & 0x8000'0000U) != 0;
	if (remaining > limit ||
	    (remaining != 0 && (cpu->nzcv & 0x2000'0000U) == 0))
		return 2;

	auto to = destination;
	auto from = operation == MopsOperation::set ? std::uint64_t{} :
		cpu_slot(cpu, source_offset);
	int fill = 0;
	if constexpr (operation == MopsOperation::set && !source_zero)
		fill = static_cast<int>(cpu_slot(cpu, source_offset) & 0xffU);
	const auto commit = [&] {
		count = remaining;
		destination = to;
		if constexpr (operation != MopsOperation::set)
			cpu_slot(cpu, source_offset) = from;
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
				remaining, std::min(amount, max_transfer_block)));
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
				remaining, std::min(amount, max_transfer_block)));
			to_address = to;
			if constexpr (operation != MopsOperation::set)
				from_address = from;
		}
		to_address = cpu->untag_data_address(to_address, false);
		if constexpr (operation != MopsOperation::set)
			from_address =
				cpu->untag_data_address(from_address, false);

		std::byte *read_pointer{};
		if constexpr (operation != MopsOperation::set) {
			read_pointer = tlb->translate(from_address, Access::read);
			if (read_pointer == nullptr) {
				if (backward) {
					tlb->fault_address = cpu->untag_data_address(
						from - 1, false);
					tlb->fault_access = Access::read;
				}
				commit();
				return 0;
			}
		}
		auto *write_pointer = tlb->translate(to_address, Access::write);
		if (write_pointer == nullptr) {
			if (backward) {
				tlb->fault_address = cpu->untag_data_address(
					to - 1, false);
				tlb->fault_access = Access::write;
			}
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
	const auto destination_offset = static_cast<std::uint16_t>(operand);
	const auto count_offset = static_cast<std::uint16_t>(operand >> 16);
	auto &destination = cpu_slot(cpu, destination_offset);
	auto &count = cpu_slot(cpu, count_offset);
	auto remaining = count;
	if ((remaining >> 63) != 0)
		remaining = limit;
	const auto address = destination;
	if ((remaining & 15U) != 0 ||
	    (remaining != 0 && (address & 15U) != 0)) {
		mte_fault(tlb, address, Access::write);
		return 0;
	}
	count = remaining;
	cpu->nzcv = 0x2000'0000U;
	return 1;
}

template <bool source_zero>
[[nodiscard]] std::uint32_t mops_set_tag_epilogue(
	CpuState *cpu, Tlb *tlb, std::uintptr_t operand) noexcept
{
	constexpr std::uint64_t limit = 0x7fff'ffff'ffff'fff0ULL;
	constexpr std::size_t max_block = 64;
	const auto destination_offset = static_cast<std::uint16_t>(operand);
	const auto count_offset = static_cast<std::uint16_t>(operand >> 16);
	const auto source_offset = static_cast<std::uint16_t>(operand >> 32);
	auto &destination = cpu_slot(cpu, destination_offset);
	auto &count = cpu_slot(cpu, count_offset);
	auto remaining = count;
	auto to = destination;
	if (remaining > limit ||
	    (remaining != 0 && (cpu->nzcv & 0x2000'0000U) == 0))
		return 2;
	if (remaining != 0 && ((remaining | to) & 15U) != 0) {
		mte_fault(tlb, to, Access::write);
		return 0;
	}
	int fill = 0;
	if constexpr (!source_zero)
		fill = static_cast<int>(cpu_slot(cpu, source_offset) & 0xffU);
	const auto commit = [&] {
		count = remaining;
		destination = to;
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

enum class GcsStatus : std::uint32_t {
	success,
	memory_fault,
	illegal,
	gcs_fault,
};

void gcs_memory_fault(Tlb *tlb, std::uint64_t address,
	Access access) noexcept
{
	tlb->fault_address = address;
	tlb->fault_access = access;
}

[[nodiscard]] bool gcs_read_u64(Tlb *tlb, std::uint64_t address,
	std::uint64_t &value) noexcept
{
	auto *pointer = tlb->translate(address, Access::read);
	if (pointer == nullptr)
		return false;
	const auto available = page_size -
		static_cast<std::size_t>(address & (page_size - 1));
	if (available >= sizeof(value)) {
		std::memcpy(&value, pointer, sizeof(value));
		return true;
	}
	auto *second = tlb->translate(address + available, Access::read);
	if (second == nullptr)
		return false;
	std::array<std::byte, sizeof(value)> bytes{};
	std::memcpy(bytes.data(), pointer, available);
	std::memcpy(bytes.data() + available, second, sizeof(value) - available);
	std::memcpy(&value, bytes.data(), sizeof(value));
	return true;
}

[[nodiscard]] bool gcs_write_u64(Tlb *tlb, std::uint64_t address,
	std::uint64_t value) noexcept
{
	auto *pointer = tlb->translate(address, Access::write);
	if (pointer == nullptr)
		return false;
	const auto available = page_size -
		static_cast<std::size_t>(address & (page_size - 1));
	if (available >= sizeof(value)) {
		std::memcpy(pointer, &value, sizeof(value));
		return true;
	}
	/* Validate both pages before the first write so a faulting GCSSTR never
	 * leaves a partial architectural store. */
	auto *second = tlb->translate(address + available, Access::write);
	if (second == nullptr)
		return false;
	std::memcpy(pointer, &value, available);
	std::memcpy(second,
		reinterpret_cast<const std::byte *>(&value) + available,
		sizeof(value) - available);
	return true;
}

[[nodiscard]] std::uint32_t guarded_call_record(CpuState *cpu, Tlb *tlb,
	std::uint64_t return_address) noexcept
{
	if (!cpu->gcs_implemented || !cpu->gcs_push_enabled)
		return static_cast<std::uint32_t>(GcsStatus::success);
	const auto address = cpu->gcs_pointer - sizeof(return_address);
	if (!gcs_write_u64(tlb, address, return_address))
		return static_cast<std::uint32_t>(GcsStatus::memory_fault);
	cpu->gcs_pointer = address & ~std::uint64_t{7};
	return static_cast<std::uint32_t>(GcsStatus::success);
}

[[nodiscard]] std::uint32_t guarded_return_record(CpuState *cpu, Tlb *tlb,
	std::uint64_t target) noexcept
{
	cpu->gadget_zero = target;
	if (!cpu->gcs_implemented || !cpu->gcs_push_enabled)
		return static_cast<std::uint32_t>(GcsStatus::success);
	const auto address = cpu->gcs_pointer;
	std::uint64_t recorded;
	if (!gcs_read_u64(tlb, address, recorded))
		return static_cast<std::uint32_t>(GcsStatus::memory_fault);
	if (recorded != target)
		return static_cast<std::uint32_t>(GcsStatus::gcs_fault);
	cpu->gadget_zero = recorded;
	cpu->gcs_pointer = (address + sizeof(recorded)) & ~std::uint64_t{7};
	return static_cast<std::uint32_t>(GcsStatus::success);
}

template<bool discard>
[[nodiscard]] std::uint32_t gcs_pop_main(CpuState *cpu, Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	if (!cpu->gcs_implemented)
		return static_cast<std::uint32_t>(GcsStatus::illegal);
	const auto address = cpu->gcs_pointer;
	std::uint64_t value;
	if (!gcs_read_u64(tlb, address, value))
		return static_cast<std::uint32_t>(GcsStatus::memory_fault);
	if ((value & 3U) != 0)
		return static_cast<std::uint32_t>(GcsStatus::gcs_fault);
	cpu->gcs_pointer = (address + 8) & ~std::uint64_t{7};
	if constexpr (!discard)
		cpu_slot(cpu, static_cast<std::uint16_t>(operand)) = value;
	return static_cast<std::uint32_t>(GcsStatus::success);
}

template<bool source_zero>
[[nodiscard]] std::uint32_t gcs_push_main(CpuState *cpu, Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	if (!cpu->gcs_implemented || !cpu->gcs_push_enabled)
		return static_cast<std::uint32_t>(GcsStatus::illegal);
	const auto address = cpu->gcs_pointer - 8;
	const auto value = source_zero ? std::uint64_t{} :
		cpu_slot(cpu, static_cast<std::uint16_t>(operand));
	if (!gcs_write_u64(tlb, address, value))
		return static_cast<std::uint32_t>(GcsStatus::memory_fault);
	cpu->gcs_pointer = address & ~std::uint64_t{7};
	return static_cast<std::uint32_t>(GcsStatus::success);
}

template<bool source_zero>
[[nodiscard]] std::uint32_t gcs_switch_stack_1(CpuState *cpu, Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	if (!cpu->gcs_implemented)
		return static_cast<std::uint32_t>(GcsStatus::illegal);
	const auto incoming = source_zero ? std::uint64_t{} :
		cpu_slot(cpu, static_cast<std::uint16_t>(operand));
	if ((incoming & 7U) != 0) {
		gcs_memory_fault(tlb, incoming, Access::write);
		return static_cast<std::uint32_t>(GcsStatus::memory_fault);
	}
	auto *pointer = tlb->translate(incoming, Access::write);
	if (pointer == nullptr)
		return static_cast<std::uint32_t>(GcsStatus::memory_fault);
	const auto compare = (incoming & ~std::uint64_t{0xfff}) | 1U;
	const auto replacement =
		(cpu->gcs_pointer & ~std::uint64_t{7}) | 5U;
	auto expected = compare;
	auto &storage = *reinterpret_cast<std::uint64_t *>(pointer);
	(void)std::atomic_ref<std::uint64_t>{storage}.compare_exchange_strong(
		expected, replacement, std::memory_order_relaxed,
		std::memory_order_relaxed);
	if (expected != compare)
		return static_cast<std::uint32_t>(GcsStatus::gcs_fault);
	cpu->gcs_pointer = incoming & ~std::uint64_t{7};
	return static_cast<std::uint32_t>(GcsStatus::success);
}

template<bool discard>
[[nodiscard]] std::uint32_t gcs_switch_stack_2(CpuState *cpu, Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	if (!cpu->gcs_implemented)
		return static_cast<std::uint32_t>(GcsStatus::illegal);
	const auto incoming = cpu->gcs_pointer;
	std::uint64_t outgoing_value;
	if (!gcs_read_u64(tlb, incoming, outgoing_value))
		return static_cast<std::uint32_t>(GcsStatus::memory_fault);
	if ((outgoing_value & 7U) != 5U)
		return static_cast<std::uint32_t>(GcsStatus::gcs_fault);
	const auto outgoing =
		(outgoing_value & ~std::uint64_t{7}) - 8;
	const auto replacement =
		(outgoing & ~std::uint64_t{0xfff}) | 1U;
	if (!gcs_write_u64(tlb, outgoing, replacement))
		return static_cast<std::uint32_t>(GcsStatus::memory_fault);
	cpu->gcs_pointer = (incoming + 8) & ~std::uint64_t{7};
	std::atomic_thread_fence(std::memory_order_seq_cst);
	if constexpr (!discard)
		cpu_slot(cpu, static_cast<std::uint16_t>(operand)) = outgoing;
	return static_cast<std::uint32_t>(GcsStatus::success);
}

template<bool base_sp, bool source_zero>
[[nodiscard]] std::uint32_t gcs_store(CpuState *cpu, Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	if (!cpu->gcs_implemented)
		return static_cast<std::uint32_t>(GcsStatus::illegal);
	if (!cpu->gcs_store_enabled)
		return static_cast<std::uint32_t>(GcsStatus::gcs_fault);
	const auto base_offset = static_cast<std::uint16_t>(operand);
	const auto source_offset = static_cast<std::uint16_t>(operand >> 16);
	const auto address = base_sp ? cpu->sp : cpu_slot(cpu, base_offset);
	if constexpr (base_sp) {
		if ((address & 15U) != 0) {
			gcs_memory_fault(tlb, address, Access::write);
			return static_cast<std::uint32_t>(GcsStatus::memory_fault);
		}
	}
	const auto value = source_zero ? std::uint64_t{} :
		cpu_slot(cpu, source_offset);
	if (!gcs_write_u64(tlb, address, value))
		return static_cast<std::uint32_t>(GcsStatus::memory_fault);
	return static_cast<std::uint32_t>(GcsStatus::success);
}

} // namespace

} // namespace uml::memory

extern "C" std::uint32_t uml_arm64_guarded_call_record(
	uml::cpu::arm64::CpuState *cpu,
	::uml::memory::Tlb *tlb,
	std::uint64_t return_address) noexcept
{
	using namespace uml::memory;
	return guarded_call_record(cpu, tlb, return_address);
}

extern "C" std::uint32_t uml_arm64_guarded_return_record(
	uml::cpu::arm64::CpuState *cpu,
	::uml::memory::Tlb *tlb,
	std::uint64_t target) noexcept
{
	using namespace uml::memory;
	return guarded_return_record(cpu, tlb, target);
}

#define UML_GCS_WRAPPER(name, helper, specialized) \
extern "C" std::uint32_t uml_arm64_gadget_gcs_transfer_##name( \
	uml::cpu::arm64::CpuState *cpu, \
	::uml::memory::Tlb *tlb, \
	std::uintptr_t operand) noexcept \
{ \
	using namespace uml::memory; \
	return helper<specialized>(cpu, tlb, operand); \
}

UML_GCS_WRAPPER(popm_r, gcs_pop_main, false)
UML_GCS_WRAPPER(popm_z, gcs_pop_main, true)
UML_GCS_WRAPPER(pushm_r, gcs_push_main, false)
UML_GCS_WRAPPER(pushm_z, gcs_push_main, true)
UML_GCS_WRAPPER(ss1_r, gcs_switch_stack_1, false)
UML_GCS_WRAPPER(ss1_z, gcs_switch_stack_1, true)
UML_GCS_WRAPPER(ss2_r, gcs_switch_stack_2, false)
UML_GCS_WRAPPER(ss2_z, gcs_switch_stack_2, true)

#undef UML_GCS_WRAPPER

#define UML_GCS_STORE_WRAPPER(name, base_sp, source_zero) \
extern "C" std::uint32_t uml_arm64_gadget_gcs_transfer_store_##name( \
	uml::cpu::arm64::CpuState *cpu, \
	::uml::memory::Tlb *tlb, \
	std::uintptr_t operand) noexcept \
{ \
	using namespace uml::memory; \
	return gcs_store<base_sp, source_zero>(cpu, tlb, operand); \
}

UML_GCS_STORE_WRAPPER(rr, false, false)
UML_GCS_STORE_WRAPPER(rz, false, true)
UML_GCS_STORE_WRAPPER(sr, true, false)
UML_GCS_STORE_WRAPPER(sz, true, true)

#undef UML_GCS_STORE_WRAPPER

#define UML_LS64_WRAPPER(name, operation, base_sp, status_discard) \
extern "C" std::uint32_t uml_arm64_gadget_ls64_transfer_##name( \
	uml::cpu::arm64::CpuState *cpu, \
	::uml::memory::Tlb *tlb, \
	std::uintptr_t operand) noexcept \
{ \
	using namespace uml::memory; \
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
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept \
{ \
	using namespace uml::memory; \
	mops_prologue<MopsOperation::operation>(cpu, operand); \
}

#define UML_MOPS_EPILOGUE_WRAPPER(name, operation, source_zero) \
extern "C" std::uint32_t uml_arm64_gadget_mops_epilogue_transfer_##name( \
	uml::cpu::arm64::CpuState *cpu, \
	::uml::memory::Tlb *tlb, \
	std::uintptr_t operand) noexcept \
{ \
	using namespace uml::memory; \
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
	uml::cpu::arm64::CpuState *cpu,
	::uml::memory::Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	using namespace uml::memory;
	return mops_set_tag_prologue(cpu, tlb, operand);
}

#define UML_MOPS_SET_TAG_EPILOGUE_WRAPPER(name, source_zero) \
extern "C" std::uint32_t \
uml_arm64_gadget_mops_set_tag_epilogue_transfer_##name( \
	uml::cpu::arm64::CpuState *cpu, \
	::uml::memory::Tlb *tlb, \
	std::uintptr_t operand) noexcept \
{ \
	using namespace uml::memory; \
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
	uml::cpu::arm64::CpuState *cpu, \
	::uml::memory::Tlb *tlb, \
	std::uintptr_t operand) noexcept \
{ \
	using namespace uml::memory; \
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
