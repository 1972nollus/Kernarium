// SPDX-License-Identifier: GPL-2.0-or-later
#include "../dispatcher.hpp"
#include "load_store_memory.hpp"

#include "../detail/atomic_floating.hpp"
#include "../detail/instruction_helpers.hpp"
#include "../detail/pointer_auth.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace uml::cpu::arm64 {

using namespace load_store_detail;

using detail::continue_after;
using detail::decode_little_endian;
using detail::encode_little_endian;
using detail::sign_extend;

RunResult Dispatcher::load_store_64b(CpuState &cpu, GuestMemory &memory,
	std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const auto opcode = instruction & 0xffe0'fc00U;
	const bool load = (instruction & 0xffff'fc00U) == 0xf83f'd000U;
	const bool status_store = opcode == 0xf820'b000U ||
		opcode == 0xf820'a000U;
	const bool accdata_store = opcode == 0xf820'a000U;
	const bool plain_store =
		(instruction & 0xffff'fc00U) == 0xf83f'9000U;
	if (!load && !status_store && !plain_store)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};

	const unsigned first = instruction & 31U;
	if ((first & 1U) != 0 || first >= 24)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const unsigned base_register = (instruction >> 5) & 31U;
	const unsigned status_register = (instruction >> 16) & 31U;
	const auto tagged_address = cpu.read_register(base_register, true);
	const bool tagged = base_register != 31U && cpu.mte_enabled;
	const bool tagchecked = tagged && !cpu.pstate_tco;
	const auto address = tagged ?
		tagged_address & 0x00ff'ffff'ffff'ffffULL : tagged_address;
	if ((address & 63U) != 0)
		return {StopReason::memory_fault, address, instruction, 0, !load};
	if (tagchecked) {
		const auto logical_tag =
			static_cast<std::uint8_t>(tagged_address >> 56) & 15U;
		for (unsigned offset = 0; offset < 64; offset += 16) {
			std::uint8_t allocation_tag{};
			if (!memory.read_allocation_tag(
				    address + offset, allocation_tag) ||
			    allocation_tag != logical_tag)
				return {StopReason::memory_fault, address + offset,
					instruction, 0, !load};
		}
	}

	std::array<std::byte, 64> bytes{};
	if (load) {
		/* Fetch the complete transfer before committing any destination, so a
		 * fault cannot expose a partial register update.
		 */
		{
			std::scoped_lock lock{exclusive_memory_mutex};
			if (!memory.read(address, bytes))
				return {StopReason::memory_fault, address, instruction, 0,
					false};
		}
		for (unsigned index = 0; index < 8; ++index)
			cpu.registers[first + index] = decode_little_endian(
				std::span{bytes}.subspan(index * 8, 8));
	} else {
		for (unsigned index = 0; index < 8; ++index) {
			auto value = cpu.registers[first + index];
			if (accdata_store && index == 0)
				value = (value & 0xffff'ffff'0000'0000ULL) |
					cpu.accdata;
			encode_little_endian(value,
				std::span{bytes}.subspan(index * 8, 8));
		}
		if (!ordinary_store(memory, address, bytes))
			return {StopReason::memory_fault, address, instruction, 0, true};
		/* Normal mapped memory is LS64-capable. Its implementation-defined
		 * successful peripheral status is zero.
		 */
		if (status_store)
			cpu.write_register(status_register, 0, false);
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_allocation_tag(
	CpuState &cpu, GuestMemory &memory, std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const auto illegal = [&] {
		return RunResult{StopReason::illegal_instruction, pc, instruction, 0,
			false};
	};
	const auto fault = [&](std::uint64_t address, bool write) {
		return RunResult{StopReason::memory_fault,
			address & 0x00ff'ffff'ffff'ffffULL, instruction, 0, write};
	};
	const auto sp_alignment_fault = [&](std::uint64_t address, bool write) {
		return (address & 15U) != 0 ? std::optional{fault(address, write)} :
			std::optional<RunResult>{};
	};

	const auto fixed = instruction & 0xffff'fc00U;
	/* GMID_EL1/DCZID_EL0 multiple-tag forms are architecturally undefined at
	 * EL0, which is the only guest exception level represented here. Validate
	 * their exact encodings before observing a register or memory. */
	if (fixed == 0xd9e0'0000U || fixed == 0xd9a0'0000U ||
	    fixed == 0xd920'0000U)
		return illegal();

	const bool stgp =
		(instruction & 0xffc0'0000U) == 0x6880'0000U ||
		(instruction & 0xffc0'0000U) == 0x6900'0000U ||
		(instruction & 0xffc0'0000U) == 0x6980'0000U;
	if (stgp) {
		const unsigned mode = (instruction >> 23) & 3U;
		if (mode == 0)
			return illegal();
		const bool postindex = mode == 1;
		const bool writeback = mode != 2;
		const unsigned first = instruction & 31U;
		const unsigned second = (instruction >> 10) & 31U;
		const unsigned base = (instruction >> 5) & 31U;
		const auto offset = static_cast<std::uint64_t>(
			detail::sign_extend((instruction >> 15) & 0x7fU, 7) * 16);
		const auto base_address = cpu.read_register(base, true);
		if (base == 31)
			if (const auto alignment = sp_alignment_fault(base_address, true))
				return *alignment;
		const auto tagged_address = postindex ? base_address :
			base_address + offset;
		const auto address = tagged_address & 0x00ff'ffff'ffff'ffffULL;
		if ((address & 15U) != 0)
			return fault(address, true);
		std::array<std::byte, 16> bytes{};
		detail::encode_little_endian(cpu.read_register(first, false),
			std::span{bytes}.first<8>());
		detail::encode_little_endian(cpu.read_register(second, false),
			std::span{bytes}.last<8>());
		if (!ordinary_store(memory, address, bytes))
			return fault(address, true);
		const auto tag = static_cast<std::uint8_t>(tagged_address >> 56) & 15U;
		if (!memory.write_allocation_tag(address, tag))
			return fault(address, true);
		if (writeback) {
			const auto updated = postindex ? base_address + offset :
				tagged_address;
			cpu.write_register(base, updated, true);
		}
		cpu.pc += 4;
		return continue_after(pc, instruction);
	}

	if ((instruction & 0xff20'0000U) != 0xd920'0000U)
		return illegal();
	const unsigned opc = (instruction >> 22) & 3U;
	const unsigned mode = (instruction >> 10) & 3U;
	const unsigned transfer = instruction & 31U;
	const unsigned base = (instruction >> 5) & 31U;
	const auto offset = static_cast<std::uint64_t>(
		detail::sign_extend((instruction >> 12) & 0x1ffU, 9) * 16);

	/* LDG is the only valid op2=00 form that is not privileged. */
	if (mode == 0) {
		if (opc != 1)
			return illegal();
		const auto base_address = cpu.read_register(base, true);
		if (base == 31)
			if (const auto alignment = sp_alignment_fault(base_address, false))
				return *alignment;
		const auto address = (base_address + offset) &
			0x00ff'ffff'ffff'fff0ULL;
		std::uint8_t tag{};
		if (!memory.read_allocation_tag(address, tag))
			return fault(address, false);
		const auto original = cpu.read_register(transfer, false);
		const auto result = (original & ~0x0f00'0000'0000'0000ULL) |
			(static_cast<std::uint64_t>(tag & 15U) << 56);
		cpu.write_register(transfer, result, false);
		cpu.pc += 4;
		return continue_after(pc, instruction);
	}

	const bool postindex = mode == 1;
	const bool writeback = mode != 2;
	const bool two_granules = opc == 2 || opc == 3;
	const bool zero_data = opc == 1 || opc == 3;
	const auto base_address = cpu.read_register(base, true);
	if (base == 31)
		if (const auto alignment = sp_alignment_fault(base_address, true))
			return *alignment;
	const auto tagged_address = postindex ? base_address :
		base_address + offset;
	const auto address = tagged_address & 0x00ff'ffff'ffff'ffffULL;
	/* AArch64_MemTagWrite faults every allocation-tag store whose effective
	 * address is not Tag-Granule aligned. The backing tag API deliberately
	 * accepts any byte within a granule, so enforce the architectural
	 * alignment before it can round the address down.
	 */
	if ((address & 15U) != 0)
		return fault(address, true);
	const auto source = cpu.read_register(transfer, true);
	const auto tag = static_cast<std::uint8_t>(source >> 56) & 15U;
	const std::size_t length = two_granules ? 32 : 16;
	if (zero_data) {
		std::array<std::byte, 32> zeros{};
		if (!ordinary_store(memory, address,
			std::span{zeros}.first(length)))
			return fault(address, true);
	}
	if (!memory.write_allocation_tag(address, tag))
		return fault(address, true);
	if (two_granules &&
	    !memory.write_allocation_tag(address + 16, tag))
		return fault(address + 16, true);
	if (writeback) {
		const auto updated = postindex ? base_address + offset : tagged_address;
		cpu.write_register(base, updated, true);
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::memory_copy_set(CpuState &cpu, GuestMemory &memory,
	std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const auto illegal = [&] {
		return RunResult{StopReason::illegal_instruction, pc, instruction, 0,
			false};
	};
	/* This implementation uses architectural MOPS option B. The prologue and
	 * main stages transfer zero bytes, and the epilogue transfers everything
	 * remaining. Both stage-size choices are explicitly implementation defined.
	 * Keeping the pure interpreter byte-granular makes fault restart state exact
	 * and favors diagnostics and safety over throughput.
	 */
	const bool set_tag =
		(instruction & 0x3fe0'0c00U) == 0x1dc0'0400U;
	const bool set = !set_tag &&
		(instruction & 0x3fe0'0c00U) == 0x19c0'0400U;
	/* The broad CPYF mask deliberately ignores its stage field, whose 0b11
	 * value enters the SET allocation. Give SET precedence so each encoding
	 * has exactly one family interpretation. */
	const bool copy_forward = !set && !set_tag &&
		(instruction & 0x3f20'0c00U) == 0x1900'0400U;
	const bool copy_general = !set && !set_tag &&
		(instruction & 0x3f20'0c00U) == 0x1d00'0400U;
	if ((!copy_forward && !copy_general && !set && !set_tag) ||
	    (instruction >> 30) != 0)
		return illegal();

	const unsigned destination = instruction & 31U;
	const unsigned count = (instruction >> 5) & 31U;
	const unsigned source = (instruction >> 16) & 31U;
	if (destination == 31 || count == 31 || destination == count ||
	    source == destination || source == count ||
	    (!set && !set_tag && source == 31))
		return illegal();

	const unsigned stage = (set || set_tag) ? (instruction >> 14) & 3U :
		(instruction >> 22) & 3U;
	if (stage == 3)
		return illegal();

	constexpr std::uint64_t max_block = 0x7fff'ffff'ffff'ffffULL;
	constexpr std::uint64_t max_copy = 0x007f'ffff'ffff'ffffULL;
	constexpr std::uint64_t max_tag_set = 0x7fff'ffff'ffff'fff0ULL;
	if (stage == 0) {
		auto remaining = cpu.registers[count];
		if (copy_general) {
			if (remaining > max_copy)
				remaining = max_copy;
			const auto from = cpu.registers[source];
			const auto to = cpu.registers[destination];
			const auto from_low = from & 0x00ff'ffff'ffff'ffffULL;
			const auto to_low = to & 0x00ff'ffff'ffff'ffffULL;
			const auto from_end = static_cast<unsigned __int128>(from_low) +
				remaining;
			const bool backward = from_low < to_low && from_end > to_low;
			/* For non-overlap, forward is this implementation's permitted
			 * implementation-defined direction choice. */
			if (backward) {
				cpu.registers[source] = from + remaining;
				cpu.registers[destination] = to + remaining;
				cpu.nzcv = 0xa000'0000U;
			} else {
				cpu.nzcv = 0x2000'0000U;
			}
		} else {
			if ((remaining >> 63) != 0)
				remaining = set_tag ? max_tag_set : max_block;
			if (set_tag && (remaining & 15U) != 0)
				return {StopReason::memory_fault,
					cpu.registers[destination] &
						0x00ff'ffff'ffff'ffffULL,
					instruction, 0, true};
			if (set_tag && remaining != 0 &&
			    (cpu.registers[destination] & 15U) != 0)
				return {StopReason::memory_fault,
					cpu.registers[destination] &
						0x00ff'ffff'ffff'ffffULL,
					instruction, 0, true};
			cpu.nzcv = 0x2000'0000U;
		}
		cpu.registers[count] = remaining;
		cpu.pc += 4;
		return continue_after(pc, instruction);
	}

	/* With the selected post-size choice the main stage has no memory or
	 * register effects. A zero stage does not perform the option-format check. */
	if (stage == 1) {
		cpu.pc += 4;
		return continue_after(pc, instruction);
	}

	auto remaining = cpu.registers[count];
	const auto limit = copy_general ? max_copy :
		set_tag ? max_tag_set : max_block;
	const bool backward = copy_general && (cpu.nzcv & 0x8000'0000U) != 0;
	if (remaining > limit ||
	    (remaining != 0 && (cpu.nzcv & 0x2000'0000U) == 0))
		return illegal();

	auto to = cpu.registers[destination];
	if (set_tag && remaining != 0 &&
	    (((remaining | to) & 15U) != 0))
		return {StopReason::memory_fault,
			to & 0x00ff'ffff'ffff'ffffULL,
			instruction, 0, true};
	auto from = (set || set_tag) ? std::uint64_t{} : cpu.registers[source];
	const auto fill = static_cast<std::byte>(
		cpu.read_register(source, false) & 0xffU);
	std::array<std::byte, 1> byte{};
	if (set_tag) {
		while (remaining != 0) {
			const auto tagged_address = to;
			const auto address = tagged_address &
				0x00ff'ffff'ffff'ffffULL;
			bool data_fault = false;
			std::uint64_t data_fault_address = address;
			byte[0] = fill;
			for (unsigned offset = 0; offset < 16; ++offset) {
				if (!ordinary_store(memory, address + offset, byte)) {
					data_fault = true;
					data_fault_address = address + offset;
					break;
				}
			}
			const auto tag = static_cast<std::uint8_t>(
				tagged_address >> 56) & 15U;
			const bool tag_stored =
				memory.write_allocation_tag(address, tag);
			if (tag_stored) {
				to += 16;
				remaining -= 16;
			}
			if (data_fault || !tag_stored) {
				cpu.registers[count] = remaining;
				cpu.registers[destination] = to;
				return {StopReason::memory_fault,
					data_fault ? data_fault_address : address,
					instruction, 0, true};
			}
		}
		cpu.registers[count] = 0;
		cpu.registers[destination] = to;
		cpu.pc += 4;
		return continue_after(pc, instruction);
	}
	while (remaining != 0) {
		const auto to_address = backward ? to - 1 : to;
		if (!set) {
			const auto from_address = backward ? from - 1 : from;
			if (!memory.read(from_address, byte)) {
				cpu.registers[count] = remaining;
				cpu.registers[destination] = to;
				cpu.registers[source] = from;
				return {StopReason::memory_fault, from_address,
					instruction, 0, false};
			}
		} else {
			byte[0] = fill;
		}
		if (!ordinary_store(memory, to_address, byte)) {
			cpu.registers[count] = remaining;
			cpu.registers[destination] = to;
			if (!set)
				cpu.registers[source] = from;
			return {StopReason::memory_fault, to_address, instruction,
				0, true};
		}
		if (backward) {
			--to;
			--from;
		} else {
			++to;
			if (!set)
				++from;
		}
		--remaining;
	}
	cpu.registers[count] = 0;
	cpu.registers[destination] = to;
	if (!set)
		cpu.registers[source] = from;
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_unsigned(CpuState &cpu, GuestMemory &memory,
					  std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const bool vector = (instruction & 0x0400'0000U) != 0;
	const unsigned size_code = instruction >> 30;
	const unsigned operation = (instruction >> 22) & 0x3U;
	if (!vector && size_code == 3 && operation == 2) {
		/* PRFM is only a performance hint in this emulator. */
		cpu.pc += 4;
		return continue_after(pc, instruction);
	}
	if (vector && operation >= 2 && size_code != 0)
		return {StopReason::illegal_instruction, pc, instruction, 0,
			false};
	const std::size_t size = vector && operation >= 2 ? 16 :
		std::size_t{1} << size_code;
	if (!vector && size >= 4 && operation == 3)
		return {StopReason::illegal_instruction, pc, instruction, 0,
			false};
	const bool load = vector ? (operation & 1U) != 0 : operation != 0;
	const unsigned base_register = (instruction >> 5) & 0x1fU;
	const auto base = cpu.read_register(base_register, true);
	/* CheckSPAlignment precedes address generation and memory observation.
	 * The interpreter deliberately diagnoses this before touching either the
	 * transfer register or GuestMemory.
	 */
	if (base_register == 31U && (base & 15U) != 0)
		return {StopReason::memory_fault, base, instruction, 0, !load};
	const auto tagged_address = base +
		static_cast<std::uint64_t>((instruction >> 10) & 0xfffU) * size;
	std::uint64_t address = tagged_address;
	if (vector) {
		std::uint64_t fault_address{};
		if (!simd_structure_address(cpu, memory, tagged_address,
			    base_register == 31U, base_register != 31U, size, size,
			    load ? MemoryAccess::read : MemoryAccess::write,
			    address, fault_address))
			return {StopReason::memory_fault, fault_address, instruction, 0,
				!load};
	}
	std::array<std::byte, 16> bytes{};
	const unsigned transfer = instruction & 0x1fU;
	if (load) {
		if (!memory.read(address, std::span{bytes}.first(size)))
			return {StopReason::memory_fault, address, instruction, 0, false};
		if (vector) {
			cpu.vectors[transfer].bytes.fill(std::byte{});
			std::copy_n(bytes.begin(), size,
				cpu.vectors[transfer].bytes.begin());
		} else {
			auto value = decode_little_endian(std::span{bytes}.first(size));
			if (operation >= 2) {
				value = static_cast<std::uint64_t>(sign_extend(value,
					static_cast<unsigned>(size * 8)));
				if (operation == 3)
					value &= 0xffff'ffffU;
			}
			cpu.write_register(transfer, value, false);
		}
	} else {
		if (vector)
			std::copy_n(cpu.vectors[transfer].bytes.begin(), size,
				bytes.begin());
		else
			encode_little_endian(cpu.read_register(transfer, false),
				std::span{bytes}.first(size));
		if (!ordinary_store(memory, address, std::span{bytes}.first(size)))
			return {StopReason::memory_fault, address, instruction, 0, true};
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_literal(CpuState &cpu, GuestMemory &memory,
				   std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const bool vector = (instruction & 0x0400'0000U) != 0;
	const unsigned operation = instruction >> 30;
	if (vector && operation == 3U)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const std::size_t size = vector ? std::size_t{4} << operation :
		operation == 1U ? 8U : 4U;
	const auto offset = sign_extend(
		static_cast<std::uint64_t>((instruction >> 5) & 0x7ffffU) << 2,
		21);
	const auto address = static_cast<std::uint64_t>(
		static_cast<std::int64_t>(pc) + offset);
	std::array<std::byte, 16> bytes{};
	if (!memory.read(address, std::span{bytes}.first(size)))
		return {StopReason::memory_fault, address, instruction, 0, false};
	const unsigned destination = instruction & 31U;
	if (vector) {
		cpu.vectors[destination].bytes.fill(std::byte{});
		std::copy_n(bytes.begin(), size,
			cpu.vectors[destination].bytes.begin());
	} else {
		auto value = decode_little_endian(std::span{bytes}.first(size));
		if (operation == 2U)
			value = static_cast<std::uint64_t>(sign_extend(value, 32));
		cpu.write_register(destination, value, false);
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_other(CpuState &cpu, GuestMemory &memory,
				       std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const bool vector = (instruction & 0x0400'0000U) != 0;
	const unsigned size_code = instruction >> 30;
	const unsigned operation = (instruction >> 22) & 0x3U;
	if (!vector && size_code == 3 && operation == 2) {
		const bool register_offset = (instruction & 0x0020'0000U) != 0;
		const unsigned mode = (instruction >> 10) & 3U;
		const unsigned option = (instruction >> 13) & 7U;
		const bool valid = register_offset ?
			(mode == 2U && (option & 2U) != 0) : mode == 0U;
		if (!valid)
			return {StopReason::illegal_instruction, pc, instruction, 0,
				false};
		/* Register-offset PRFM and unscaled PRFUM are advisory only. */
		cpu.pc += 4;
		return continue_after(pc, instruction);
	}
	if (vector && operation >= 2 && size_code != 0)
		return {StopReason::illegal_instruction, pc, instruction, 0,
			false};
	const std::size_t size = vector && operation >= 2 ? 16 :
		std::size_t{1} << size_code;
	if (!vector && ((size == 8 && operation != 0 && operation != 1) ||
	    (size == 4 && operation == 3)))
		return {StopReason::illegal_instruction, pc, instruction, 0,
			false};
	const bool register_offset = (instruction & 0x0020'0000U) != 0;
	const unsigned mode = (instruction >> 10) & 0x3U;
	const unsigned option = (instruction >> 13) & 0x7U;
	if ((register_offset &&
	     (mode != 2U || (option & 2U) == 0)) ||
	    (!register_offset && vector && mode == 2U))
		return {StopReason::illegal_instruction, pc, instruction, 0,
			false};
	const bool load = vector ? (operation & 1U) != 0 : operation != 0;
	const unsigned base_register = (instruction >> 5) & 0x1fU;
	const auto base = cpu.read_register(base_register, true);
	/* This check is on the unmodified SP value, before immediate/register
	 * effective-address generation and any transfer-register or memory effect.
	 */
	if (base_register == 31U && (base & 15U) != 0)
		return {StopReason::memory_fault, base, instruction, 0, !load};
	std::uint64_t address = base;
	bool writeback = false;
	std::uint64_t adjusted = base;
	if (register_offset) {
		auto offset = cpu.read_register((instruction >> 16) & 0x1fU, false);
		if (option == 2 || option == 6)
			offset &= 0xffff'ffffU;
		if ((option & 0x4U) != 0) {
			const unsigned width = (option & 1U) != 0 ? 64 : 32;
			const auto source_mask = width == 64 ? ~std::uint64_t{0} :
				0xffff'ffffU;
			if ((offset & (std::uint64_t{1} << (width - 1))) != 0)
				offset |= ~source_mask;
		}
		if ((instruction & 0x0000'1000U) != 0)
			offset <<= vector && operation >= 2 ? 4 : size_code;
		address += offset;
	} else {
		const auto displacement = sign_extend(
			(instruction >> 12) & 0x1ffU, 9);
		adjusted = static_cast<std::uint64_t>(
			static_cast<std::int64_t>(base) + displacement);
		if (mode == 1) {
			address = base;
			writeback = true;
		} else {
			address = adjusted;
			writeback = mode == 3;
		}
	}
	if (vector) {
		const auto tagged_address = address;
		std::uint64_t fault_address{};
		if (!simd_structure_address(cpu, memory, tagged_address,
			    base_register == 31U,
			    register_offset || writeback || base_register != 31U,
			    size, size,
			    load ? MemoryAccess::read : MemoryAccess::write,
			    address, fault_address))
			return {StopReason::memory_fault, fault_address, instruction, 0,
				!load};
	}
	std::array<std::byte, 16> bytes{};
	const unsigned transfer = instruction & 0x1fU;
	if (load) {
		if (!memory.read(address, std::span{bytes}.first(size)))
			return {StopReason::memory_fault, address, instruction, 0, false};
		if (vector) {
			cpu.vectors[transfer].bytes.fill(std::byte{});
			std::copy_n(bytes.begin(), size,
				cpu.vectors[transfer].bytes.begin());
		} else {
			auto value = decode_little_endian(std::span{bytes}.first(size));
			if (operation >= 2) {
				value = static_cast<std::uint64_t>(sign_extend(value,
					static_cast<unsigned>(size * 8)));
				if (operation == 3)
					value &= 0xffff'ffffU;
			}
			cpu.write_register(transfer, value, false);
		}
	} else {
		if (vector)
			std::copy_n(cpu.vectors[transfer].bytes.begin(), size,
				bytes.begin());
		else
			encode_little_endian(cpu.read_register(transfer, false),
				std::span{bytes}.first(size));
		if (!ordinary_store(memory, address, std::span{bytes}.first(size)))
			return {StopReason::memory_fault, address, instruction, 0, true};
	}
	/* The 2026-06 architectural constraint selection suppresses writeback for
	 * a load whose transfer register overlaps a non-SP base. Stores retain
	 * Constraint_NONE and therefore store the pre-writeback base value.
	 */
	if (writeback && load && !vector &&
	    base_register == transfer && base_register != 31U)
		writeback = false;
	if (writeback)
		cpu.write_register(base_register, adjusted, true);
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_pointer_authenticated(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	if (!cpu.pointer_authentication_enabled)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const unsigned destination = instruction & 31U;
	const unsigned base_register = (instruction >> 5) & 31U;
	const bool writeback = (instruction & 0x800U) != 0;
	/* Resolve the permitted constrained-unpredictable choice to UNDEFINED.
	 * Diagnose it before authentication or memory observation. */
	if (writeback && base_register == destination && base_register != 31U)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};

	const auto encoded_offset = ((instruction >> 12) & 0x1ffU) |
		(((instruction >> 22) & 1U) << 9);
	const auto displacement = sign_extend(encoded_offset, 10) * 8;
	auto address = cpu.read_register(base_register, true);
	if (base_register == 31U && (address & 15U) != 0)
		return {StopReason::memory_fault, address, instruction, 0, false};
	address = detail::execute_pointer_auth(
		cpu, address, 0,
		(instruction & 0x0080'0000U) != 0 ?
			detail::PointerAuthOperation::authenticate_data_b :
			detail::PointerAuthOperation::authenticate_data_a);
	address += displacement;
	std::array<std::byte, 8> bytes{};
	if (!memory.read(address, bytes))
		return {StopReason::memory_fault, address, instruction, 0, false};
	const auto value = decode_little_endian(bytes);
	cpu.write_register(destination, value, false);
	if (writeback)
		cpu.write_register(base_register, address, true);
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_pair(CpuState &cpu, GuestMemory &memory,
				      std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const bool vector = (instruction & 0x0400'0000U) != 0;
	const bool load = (instruction & 0x0040'0000U) != 0;
	const unsigned operation = instruction >> 30;
	const unsigned mode = (instruction >> 23) & 0x3U;
	/* SIMD&FP opc=11 is the FEAT_LSUI 128-bit unprivileged pair family.
	 * GPR opc=11 is the corresponding 64-bit family. GPR opc=01 is LDPSW
	 * and has no non-temporal or store allocation. Reject the remaining
	 * encodings before observing the base or memory.
	 */
	if (!vector && operation == 1U && (!load || mode == 0U))
		return {StopReason::illegal_instruction, pc, instruction, 0,
			false};
	const unsigned first = instruction & 0x1fU;
	const unsigned second = (instruction >> 10) & 0x1fU;
	if (load && first == second)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const std::size_t size = vector ?
		(operation >= 2 ? 16U : std::size_t{4} << operation) :
		operation >= 2 ? 8U : 4U;
	const auto displacement = sign_extend((instruction >> 15) & 0x7fU, 7) *
		static_cast<std::int64_t>(size);
	const unsigned base_register = (instruction >> 5) & 0x1fU;
	const auto base = cpu.read_register(base_register, true);
	const auto adjusted = static_cast<std::uint64_t>(
		static_cast<std::int64_t>(base) + displacement);
	const auto tagged_address = mode == 1 ? base : adjusted;
	std::uint64_t address = tagged_address;
	bool writeback = mode == 1 || mode == 3;
	if (writeback && load && !vector && base_register != 31U &&
	    (base_register == first || base_register == second))
		writeback = false;
	if (base_register == 31U && (base & 15U) != 0)
		return {StopReason::memory_fault, base, instruction, 0, !load};
	if (vector) {
		std::uint64_t fault_address{};
		if (!simd_structure_address(cpu, memory, tagged_address,
			    base_register == 31U,
			    writeback || base_register != 31U, size * 2U, size * 2U,
			    load ? MemoryAccess::read : MemoryAccess::write,
			    address, fault_address))
			return {StopReason::memory_fault, fault_address, instruction, 0,
				!load};
	}
	std::array<std::byte, 32> pair_bytes{};
	if (load) {
		if (!memory.read(address, std::span{pair_bytes}.first(size * 2U)))
			return {StopReason::memory_fault, address, instruction, 0, false};
		if (vector) {
			cpu.vectors[first].bytes.fill(std::byte{});
			cpu.vectors[second].bytes.fill(std::byte{});
			std::copy_n(pair_bytes.begin(), size,
				cpu.vectors[first].bytes.begin());
			std::copy_n(pair_bytes.begin() + size, size,
				cpu.vectors[second].bytes.begin());
		} else {
			auto first_value = decode_little_endian(
				std::span{pair_bytes}.first(size));
			auto second_value = decode_little_endian(
				std::span{pair_bytes}.subspan(size, size));
			if (operation == 1) {
				first_value = static_cast<std::uint64_t>(
					static_cast<std::int64_t>(
						static_cast<std::int32_t>(first_value)));
				second_value = static_cast<std::uint64_t>(
					static_cast<std::int64_t>(
						static_cast<std::int32_t>(second_value)));
			}
			cpu.write_register(first, first_value, false);
			cpu.write_register(second, second_value, false);
		}
	} else {
		if (vector) {
			std::copy_n(cpu.vectors[first].bytes.begin(), size,
				pair_bytes.begin());
			std::copy_n(cpu.vectors[second].bytes.begin(), size,
				pair_bytes.begin() + size);
		} else {
			encode_little_endian(cpu.read_register(first, false),
				std::span{pair_bytes}.first(size));
			encode_little_endian(cpu.read_register(second, false),
				std::span{pair_bytes}.subspan(size, size));
		}
		if (!ordinary_store(memory, address,
			std::span{pair_bytes}.first(size * 2)))
			return {StopReason::memory_fault, address, instruction, 0, true};
	}
	if (writeback)
		cpu.write_register(base_register, adjusted, true);
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_simd_structure(
	CpuState &cpu, GuestMemory &memory, std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const unsigned opcode = (instruction >> 12) & 0xfU;
	const bool interleaved = opcode == 8 || opcode == 4 || opcode == 0;
	const unsigned register_count = opcode == 7 ? 1 : opcode == 10 ? 2 :
		opcode == 6 ? 3 : opcode == 2 ? 4 : opcode == 8 ? 2 :
		opcode == 4 ? 3 : opcode == 0 ? 4 : 0;
	if (register_count == 0)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const bool load = (instruction & 0x0040'0000U) != 0;
	const bool post_index = (instruction & 0x0080'0000U) != 0;
	const unsigned offset_register = (instruction >> 16) & 0x1fU;
	if (!post_index && offset_register != 0U)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ?
		16 : 8;
	const unsigned size = (instruction >> 10) & 0x3U;
	if (interleaved && vector_size == 8 && size == 3U)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const std::size_t element_size = std::size_t{1} << size;
	const std::size_t transfer_size = vector_size * register_count;
	const unsigned base_register = (instruction >> 5) & 0x1fU;
	const auto tagged_address = cpu.read_register(base_register, true);
	if (base_register == 31U && (tagged_address & 15U) != 0)
		return {StopReason::memory_fault, tagged_address, instruction, 0,
			!load};
	std::uint64_t address{};
	std::uint64_t tag_fault{};
	if (!simd_structure_address(cpu, memory, tagged_address,
		    base_register == 31U, post_index || base_register != 31U,
		    transfer_size, element_size,
		    load ? MemoryAccess::read : MemoryAccess::write,
		    address, tag_fault))
		return {StopReason::memory_fault, tag_fault, instruction, 0, !load};
	const unsigned first_register = instruction & 0x1fU;
	std::array<std::byte, 64> bytes{};
	if (load) {
		if (!memory.read(address, std::span{bytes}.first(transfer_size)))
			return {StopReason::memory_fault, address, instruction, 0, false};
		for (unsigned index = 0; index < register_count; ++index) {
			auto &destination = cpu.vectors[
				(first_register + index) & 0x1fU].bytes;
			destination.fill(std::byte{});
			if (interleaved) {
				for (std::size_t lane = 0; lane < vector_size / element_size;
				     ++lane)
					std::copy_n(bytes.begin() +
						(lane * register_count + index) * element_size,
						element_size,
						destination.begin() + lane * element_size);
			} else {
				std::copy_n(bytes.begin() + index * vector_size, vector_size,
					destination.begin());
			}
		}
	} else {
		for (unsigned index = 0; index < register_count; ++index) {
			const auto &source = cpu.vectors[
				(first_register + index) & 0x1fU].bytes;
			if (interleaved) {
				for (std::size_t lane = 0; lane < vector_size / element_size;
				     ++lane)
					std::copy_n(source.begin() + lane * element_size,
						element_size, bytes.begin() +
						(lane * register_count + index) * element_size);
			} else {
				std::copy_n(source.begin(), vector_size,
					bytes.begin() + index * vector_size);
			}
		}
		if (!ordinary_store(memory, address,
			std::span{bytes}.first(transfer_size)))
			return {StopReason::memory_fault, address, instruction, 0, true};
	}
	if (post_index) {
		const auto offset = offset_register == 0x1fU ? transfer_size :
			cpu.read_register(offset_register, false);
		cpu.write_register(base_register, tagged_address + offset, true);
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_simd_single_structure(
	CpuState &cpu, GuestMemory &memory, std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const bool full_vector = (instruction & 0x4000'0000U) != 0;
	const bool load = (instruction & 0x0040'0000U) != 0;
	const bool post_index = (instruction & 0x0080'0000U) != 0;
	const unsigned extra_register = (instruction >> 21) & 1U;
	const unsigned opcode = (instruction >> 13) & 7U;
	const unsigned scale_bit = (instruction >> 12) & 1U;
	const unsigned size = (instruction >> 10) & 3U;
	const unsigned offset_register = (instruction >> 16) & 0x1fU;
	if (!post_index && offset_register != 0U)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const unsigned base_register = (instruction >> 5) & 0x1fU;
	const unsigned first_register = instruction & 0x1fU;
	const std::size_t vector_size = full_vector ? 16 : 8;
	unsigned register_count;
	std::size_t element_size;
	std::size_t lane_offset = 0;
	bool replicate = false;

	const unsigned scale = opcode >> 1;
	if (scale == 3U) {
		if (!load || scale_bit != 0U)
			return {StopReason::illegal_instruction, pc, instruction, 0,
				false};
		replicate = true;
		register_count = opcode == 6 ? 1 + extra_register :
			3 + extra_register;
		element_size = std::size_t{1} << size;
	} else {
		register_count = (opcode & 1U) != 0 ?
			3 + extra_register : 1 + extra_register;
		std::size_t lane;
		if (scale == 0U) {
			element_size = 1;
			lane = (static_cast<std::size_t>(full_vector) << 3) |
				(scale_bit << 2) | size;
		} else if (scale == 1U) {
			if ((size & 1U) != 0U)
				return {StopReason::illegal_instruction, pc, instruction,
					0, false};
			element_size = 2;
			lane = (static_cast<std::size_t>(full_vector) << 2) |
				(scale_bit << 1) | (size >> 1);
		} else {
			if ((size & 2U) != 0U || ((size & 1U) != 0U &&
			    scale_bit != 0U))
				return {StopReason::illegal_instruction, pc, instruction,
					0, false};
			element_size = (size & 1U) != 0U ? 8U : 4U;
			lane = (size & 1U) != 0U ?
				static_cast<std::size_t>(full_vector) :
				(static_cast<std::size_t>(full_vector) << 1) |
					scale_bit;
		}
		lane_offset = lane * element_size;
	}

	const std::size_t transfer_size = element_size * register_count;
	const auto tagged_address = cpu.read_register(base_register, true);
	if (base_register == 31U && (tagged_address & 15U) != 0)
		return {StopReason::memory_fault, tagged_address, instruction, 0,
			!load};
	std::uint64_t address{};
	std::uint64_t tag_fault{};
	if (!simd_structure_address(cpu, memory, tagged_address,
		    base_register == 31U, post_index || base_register != 31U,
		    transfer_size, element_size,
		    load ? MemoryAccess::read : MemoryAccess::write,
		    address, tag_fault))
		return {StopReason::memory_fault, tag_fault, instruction, 0, !load};
	std::array<std::byte, 32> bytes{};
	if (load) {
		if (!memory.read(address, std::span{bytes}.first(transfer_size)))
			return {StopReason::memory_fault, address, instruction, 0, false};
		for (unsigned index = 0; index < register_count; ++index) {
			auto &destination = cpu.vectors[
				(first_register + index) & 0x1fU].bytes;
			const auto source = std::span{bytes}.subspan(
				index * element_size, element_size);
			if (replicate) {
				destination.fill(std::byte{});
				for (std::size_t offset = 0; offset < vector_size;
				     offset += element_size)
					std::ranges::copy(source,
						destination.begin() + offset);
			} else {
				std::ranges::copy(source,
					destination.begin() + lane_offset);
			}
		}
	} else {
		for (unsigned index = 0; index < register_count; ++index) {
			const auto &source = cpu.vectors[
				(first_register + index) & 0x1fU].bytes;
			std::copy_n(source.begin() + lane_offset, element_size,
				bytes.begin() + index * element_size);
		}
		if (!ordinary_store(memory, address,
			std::span{bytes}.first(transfer_size)))
			return {StopReason::memory_fault, address, instruction, 0, true};
	}
	if (post_index) {
		const auto offset = offset_register == 0x1fU ? transfer_size :
			cpu.read_register(offset_register, false);
		cpu.write_register(base_register, tagged_address + offset, true);
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

} // namespace uml::cpu::arm64
