// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_semantic_instructions.hpp"
#include "jit_semantic_reservation.hpp"

#include "cpu/arm64/detail/atomic_floating.hpp"
#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/pointer_auth.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace uml::cpu::arm64::jit::semantic {

using arm64::detail::decode_little_endian;
using arm64::detail::encode_little_endian;
using arm64::detail::sign_extend;

RunResult load_store_64b(CpuState &cpu, GuestMemory &memory,
	std::uint32_t instruction)
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
	const bool tagged = base_register != 31U &&
		cpu.tagged_addresses_enabled();
	const bool tagchecked = base_register != 31U && cpu.mte_enabled &&
		!cpu.pstate_tco;
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

RunResult memory_copy_set(CpuState &cpu, GuestMemory &memory,
	std::uint32_t instruction)
{
	const auto pc = cpu.pc;
	const auto illegal = [&] {
		return RunResult{StopReason::illegal_instruction, pc, instruction, 0,
			false};
	};
	/* This implementation uses architectural MOPS option B. The prologue and
	 * main stages transfer zero bytes, and the epilogue transfers everything
	 * remaining. Both stage-size choices are explicitly implementation defined.
	 * Keeping the semantic slow path byte-granular makes fault restart state
	 * exact and favors diagnostics and safety over throughput.
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
		const auto tagged_to_address = backward ? to - 1 : to;
		const auto to_address =
			cpu.untag_data_address(tagged_to_address, false);
		if (!set) {
			const auto tagged_from_address = backward ? from - 1 : from;
			const auto from_address =
				cpu.untag_data_address(tagged_from_address, false);
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

RunResult load_store_unsigned(CpuState &cpu, GuestMemory &memory,
					  std::uint32_t instruction)
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
	 * The slow path deliberately diagnoses this before touching either the
	 * transfer register or GuestMemory.
	 */
	if (base_register == 31U && (base & 15U) != 0)
		return {StopReason::memory_fault, base, instruction, 0, !load};
	const auto tagged_address = base +
		static_cast<std::uint64_t>((instruction >> 10) & 0xfffU) * size;
	std::uint64_t address{};
	std::uint64_t fault_address{};
	if (!simd_structure_address(cpu, memory, tagged_address,
		    base_register == 31U, base_register != 31U, size, size,
		    load ? MemoryAccess::read : MemoryAccess::write,
		    address, fault_address))
		return {StopReason::memory_fault, fault_address, instruction, 0,
			!load};
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

RunResult load_literal(CpuState &cpu, GuestMemory &memory,
				   std::uint32_t instruction)
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

RunResult load_store_other(CpuState &cpu, GuestMemory &memory,
				       std::uint32_t instruction)
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

RunResult load_pointer_authenticated(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction)
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
	address = arm64::detail::execute_pointer_auth(
		cpu, address, 0,
		(instruction & 0x0080'0000U) != 0 ?
			arm64::detail::PointerAuthOperation::authenticate_data_b :
			arm64::detail::PointerAuthOperation::authenticate_data_a);
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

} // namespace uml::cpu::arm64::jit::semantic
