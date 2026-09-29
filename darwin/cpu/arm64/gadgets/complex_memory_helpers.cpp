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
	const auto address = cpu->untag_data_address(tagged_address, base_sp);
	constexpr bool load = operation == Ls64Operation::load;
	if ((address & 63U) != 0) {
		tlb->fault_alignment = 1;
		tlb->fault_address = address;
		tlb->fault_access = load ? Access::read : Access::write;
		return 0;
	}

	Ls64Lock lock;
	auto *pointer = tlb->translate(
		address, load ? Access::read : Access::write);
	if (pointer == nullptr)
		return 0;
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

} // namespace

} // namespace uml::memory

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

#undef UML_MOPS_PROLOGUE_WRAPPER
#undef UML_MOPS_EPILOGUE_WRAPPER

