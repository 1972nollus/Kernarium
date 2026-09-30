// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlb.hpp"

#include "../cpu_state.hpp"

#include "uml_darwin_static_mmu.h"
#include "uml_darwin_guest.h"

#include <algorithm>
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

