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

RunResult Dispatcher::load_store_simd_ordered(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const bool lane_form =
		(instruction & 0xbfbf'fc00U) == 0x0d01'8400U;
	const bool load = (instruction & 0x0040'0000U) != 0U;
	const unsigned base_register = (instruction >> 5) & 31U;
	const unsigned transfer = instruction & 31U;

	if (lane_form) {
		/* LDAP1/STL1 are the only allocated R=1,o2=1,opcode=100,size=01
		 * members of this class. Their Q bit selects D[0] or D[1]. */
		const auto address = cpu.read_register(base_register, true);
		if ((address & 7U) != 0U)
			return {StopReason::memory_fault, address, instruction, 0, !load};
		const std::size_t lane = instruction >> 30;
		if (load) {
			std::uint64_t value = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_value(memory, address, 8,
					std::memory_order_acquire, value, key))
				return {StopReason::memory_fault, address, instruction, 0,
					false};
			detail::set_vector_half(cpu.vectors[transfer], lane, value);
		} else {
			const auto value = detail::vector_half(cpu.vectors[transfer], lane);
			std::scoped_lock lock{exclusive_memory_mutex};
			std::uintptr_t key = 0;
			if (atomic_store_value(memory, address, 8, value,
					std::memory_order_release, nullptr, key) !=
			    AtomicStoreResult::stored)
				return {StopReason::memory_fault, address, instruction, 0,
					true};
			reservation_store(key);
		}
		cpu.pc += 4;
		return continue_after(pc, instruction);
	}

	const unsigned size_code = instruction >> 30;
	const unsigned opc = (instruction >> 22) & 3U;
	/* opc[1] selects the 128-bit form and is allocated only with size=00. */
	if ((opc & 2U) != 0U && size_code != 0U)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const std::size_t size = (opc & 2U) != 0U ? 16U :
		(std::size_t{1} << size_code);
	const auto offset = sign_extend((instruction >> 12) & 0x1ffU, 9);
	const auto address = cpu.read_register(base_register, true) +
		static_cast<std::uint64_t>(offset);
	if ((address & (size - 1U)) != 0U)
		return {StopReason::memory_fault, address, instruction, 0, !load};

	if (load) {
		std::array<std::byte, 16> result{};
		if (size <= 8U) {
			std::uint64_t value = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_value(memory, address, size,
					std::memory_order_acquire, value, key))
				return {StopReason::memory_fault, address, instruction, 0,
					false};
			encode_little_endian(value, std::span{result}.first(size));
		} else {
			/* A 128-bit ASIMD acquire is not required to be single-copy atomic.
			 * Commit the destination only after the complete checked read, then
			 * use a stronger acquire fence in this safety-first reference path. */
			if (!memory.read(address, result))
				return {StopReason::memory_fault, address, instruction, 0,
					false};
			std::atomic_thread_fence(std::memory_order_acquire);
		}
		cpu.vectors[transfer].bytes = result;
	} else {
		const auto &source = cpu.vectors[transfer].bytes;
		if (size <= 8U) {
			const auto value = decode_little_endian(
				std::span{source}.first(size));
			std::scoped_lock lock{exclusive_memory_mutex};
			std::uintptr_t key = 0;
			if (atomic_store_value(memory, address, size, value,
					std::memory_order_release, nullptr, key) !=
			    AtomicStoreResult::stored)
				return {StopReason::memory_fault, address, instruction, 0,
					true};
			reservation_store(key);
		} else {
			std::atomic_thread_fence(std::memory_order_release);
			if (!ordinary_store(memory, address, source))
				return {StopReason::memory_fault, address, instruction, 0,
					true};
		}
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_exclusive(CpuState &cpu,
					    GuestMemory &memory,
					    std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const bool load = (instruction & 0x0040'0000U) != 0;
	const bool pair = (instruction & 0x0020'0000U) != 0;
	const bool exclusive = (instruction & 0x0080'0000U) == 0;
	const unsigned status = (instruction >> 16) & 0x1fU;
	/* Compare-and-swap encodings are a separate class. */
	if (pair && !exclusive)
		return {StopReason::unsupported_instruction, pc, instruction, 0,
			false};
	const unsigned size_code = instruction >> 30;
	const unsigned transfer = instruction & 0x1fU;
	const unsigned second_transfer = (instruction >> 10) & 0x1fU;
	const unsigned base_register = (instruction >> 5) & 0x1fU;
	const auto address = cpu.read_register(base_register, true);
	const bool ordered = (instruction & 0x0000'8000U) != 0;
	if (pair) {
		/* Pair-exclusive encodings fix bit 31, use Rt2 as a real transfer
		 * register, and select UNDEFINED for duplicate load destinations in
		 * this architecture profile.
		 */
		if (size_code < 2U || (load && status != 0x1fU) ||
		    (load && transfer == second_transfer))
			return {StopReason::illegal_instruction, pc, instruction, 0,
				false};
		const std::size_t element_size = std::size_t{1} << size_code;
		const auto total_size = element_size * 2;
		if ((base_register == 31U && (address & 15U) != 0) ||
		    (address & (total_size - 1U)) != 0)
			return {StopReason::memory_fault, address, instruction, 0,
				!load};
		if (load) {
			std::scoped_lock lock{exclusive_memory_mutex};
			std::uint64_t first = 0;
			std::uint64_t second = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_pair_value(memory, address, element_size,
					ordered ? std::memory_order_acquire :
						std::memory_order_relaxed,
					first, second, key))
				return {StopReason::memory_fault, address, instruction, 0,
					false};
			cpu.write_register(transfer, first, false);
			cpu.write_register(second_transfer, second, false);
			cpu.exclusive_address = address;
			cpu.exclusive_key = key;
			/* Store the monitored bytes in address order rather than in the
			 * instruction's element shape. A 32-bit LDXP and a 64-bit LDXR
			 * cover the same eight-byte exclusive region and may pair with the
			 * corresponding store form.
			 */
			cpu.exclusive_value = element_size == 4 ?
				first | (second << 32) : first;
			cpu.exclusive_value2 = element_size == 8 ? second : 0;
			cpu.exclusive_version = reservation_version(key);
			cpu.exclusive_size = static_cast<std::uint8_t>(total_size);
		} else {
			const auto first = cpu.read_register(transfer, false);
			const auto second = cpu.read_register(second_transfer, false);
			std::scoped_lock lock{exclusive_memory_mutex};
			std::uint64_t current_first = 0;
			std::uint64_t current_second = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_pair_value(memory, address, element_size,
					std::memory_order_relaxed, current_first,
					current_second, key))
				return {StopReason::memory_fault, address, instruction, 0,
					true};
			const auto monitored_first = element_size == 4 ?
				current_first | (current_second << 32) : current_first;
			const auto monitored_second = element_size == 8 ?
				current_second : 0;
			bool success = cpu.exclusive_address == address &&
				cpu.exclusive_size == total_size &&
				cpu.exclusive_key == key &&
				cpu.exclusive_version == reservation_version(key) &&
				cpu.exclusive_value == monitored_first &&
				cpu.exclusive_value2 == monitored_second;
			cpu.exclusive_address = ~std::uint64_t{0};
			cpu.exclusive_key = 0;
			cpu.exclusive_value = 0;
			cpu.exclusive_value2 = 0;
			cpu.exclusive_version = 0;
			cpu.exclusive_size = 0;
			if (success) {
				std::uintptr_t stored_key = 0;
				const auto store_result = atomic_store_pair_value(memory,
					address, element_size, first, second,
					ordered ? std::memory_order_release :
						std::memory_order_relaxed,
					&key, stored_key);
				if (store_result == AtomicStoreResult::fault)
					return {StopReason::memory_fault, address,
						instruction, 0, true};
				if (store_result == AtomicStoreResult::stored)
					reservation_store(stored_key);
				else
					success = false;
			}
			cpu.write_register(status, success ? 0 : 1, false);
		}
		cpu.pc += 4;
		return continue_after(pc, instruction);
	}
	if (((instruction >> 10) & 0x1fU) != 0x1fU ||
	    (!exclusive && status != 0x1fU) ||
	    (exclusive && load && status != 0x1fU))
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const std::size_t size = std::size_t{1} << size_code;
	if ((base_register == 31U && (address & 15U) != 0) ||
	    (address & (size - 1U)) != 0)
		return {StopReason::memory_fault, address, instruction, 0, !load};
	if (load) {
		if (exclusive) {
			std::scoped_lock lock{exclusive_memory_mutex};
			std::uint64_t value = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_value(memory, address, size,
					       ordered ? std::memory_order_acquire :
						std::memory_order_relaxed,
					       value, key))
				return {StopReason::memory_fault, address, instruction, 0,
					false};
			cpu.write_register(transfer, value, false);
			cpu.exclusive_address = address;
			cpu.exclusive_key = key;
			cpu.exclusive_value = value;
			cpu.exclusive_value2 = 0;
			cpu.exclusive_version = reservation_version(key);
			cpu.exclusive_size = static_cast<std::uint8_t>(size);
		} else {
			/* LDAR does not establish or inspect an exclusive reservation.
			 * Serializing polling acquire-loads with LDXR/STXR lets an
			 * aggressive waiter starve the thread that must advance the
			 * observed value.  The acquire fence below provides the ordered
			 * load semantics without involving the exclusive monitor.
			 */
			std::uint64_t value = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_value(memory, address, size,
					       std::memory_order_acquire, value, key))
				return {StopReason::memory_fault, address, instruction, 0,
					false};
			cpu.write_register(transfer, value, false);
		}
	} else {
		const auto value = cpu.read_register(transfer, false);
		std::scoped_lock lock{exclusive_memory_mutex};
		if (exclusive) {
			std::uint64_t current = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_value(memory, address, size,
					       std::memory_order_relaxed, current, key))
				return {StopReason::memory_fault, address, instruction, 0,
					true};
			bool success = cpu.exclusive_address == address &&
				cpu.exclusive_size == size &&
				cpu.exclusive_key == key &&
				cpu.exclusive_version == reservation_version(key) &&
				cpu.exclusive_value == current;
			cpu.exclusive_address = ~std::uint64_t{0};
			cpu.exclusive_key = 0;
			cpu.exclusive_value = 0;
			cpu.exclusive_value2 = 0;
			cpu.exclusive_version = 0;
			cpu.exclusive_size = 0;
			if (success) {
				std::uintptr_t stored_key = 0;
				const auto store_result = atomic_store_value(
					memory, address, size, value,
					ordered ? std::memory_order_release :
						std::memory_order_relaxed,
					&key, stored_key);
				if (store_result == AtomicStoreResult::fault)
					return {StopReason::memory_fault, address,
						instruction, 0, true};
				if (store_result == AtomicStoreResult::stored)
					reservation_store(stored_key);
				else {
					success = false;
				}
			}
			cpu.write_register(status, success ? 0 : 1, false);
		} else {
			std::uintptr_t key = 0;
			if (atomic_store_value(memory, address, size, value,
					std::memory_order_release,
					nullptr, key) != AtomicStoreResult::stored)
				return {StopReason::memory_fault, address, instruction, 0,
					true};
			/* ARM reservations are invalidated by a write even when the
			 * stored bits equal the old value.  The sequence is keyed by the
			 * translated host reservation granule, so shared aliases observe
			 * the invalidation without unrelated atomics causing livelock.
			 */
			reservation_store(key);
		}
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_compare_swap(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	if (((instruction >> 10) & 31U) != 31U)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const bool unprivileged = (instruction & 0x0100'0000U) != 0;
	const unsigned size_code = instruction >> 30;
	if (unprivileged && size_code != 3U)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const std::size_t size = std::size_t{1} << size_code;
	const unsigned compare_register = (instruction >> 16) & 31U;
	const unsigned replacement_register = instruction & 31U;
	const unsigned base_register = (instruction >> 5) & 31U;
	const auto address = cpu.read_register(base_register, true);
	if ((base_register == 31U && (address & 15U) != 0) ||
	    (address & (size - 1U)) != 0)
		return {StopReason::memory_fault, address, instruction, 0, true};
	const auto compare = cpu.read_register(compare_register, false);
	const auto replacement = cpu.read_register(replacement_register, false);
	const bool acquire = (instruction & 0x0040'0000U) != 0 &&
		compare_register != 31U;
	const bool release = (instruction & 0x0000'8000U) != 0;
	std::scoped_lock lock{exclusive_memory_mutex};
	std::uint64_t observed = 0;
	std::uintptr_t key = 0;
	if (!atomic_compare_swap_value(memory, address, size, compare, replacement,
			acquire, release, observed, key))
		return {StopReason::memory_fault, address, instruction, 0, true};
	/* The architecture permits a CAS read to clear reservations even when its
	 * comparison fails. This implementation consistently chooses that behavior.
	 */
	reservation_store(key);
	cpu.write_register(compare_register, observed, false);
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_compare_swap_pair(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const unsigned compare_register = (instruction >> 16) & 31U;
	const unsigned replacement_register = instruction & 31U;
	if (((instruction >> 10) & 31U) != 31U ||
	    (compare_register & 1U) != 0 || (replacement_register & 1U) != 0)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const bool unprivileged = (instruction & 0x0100'0000U) != 0;
	const unsigned size_code = instruction >> 30;
	if (size_code > 1U || (unprivileged && size_code != 1U))
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const std::size_t element_size = std::size_t{4} << size_code;
	const auto total_size = element_size * 2;
	const unsigned base_register = (instruction >> 5) & 31U;
	const auto address = cpu.read_register(base_register, true);
	if ((base_register == 31U && (address & 15U) != 0) ||
	    (address & (total_size - 1U)) != 0)
		return {StopReason::memory_fault, address, instruction, 0, true};
	const auto compare_first = cpu.read_register(compare_register, false);
	const auto compare_second = cpu.read_register(compare_register + 1, false);
	const auto replacement_first = cpu.read_register(
		replacement_register, false);
	const auto replacement_second = cpu.read_register(
		replacement_register + 1, false);
	const bool acquire = (instruction & 0x0040'0000U) != 0;
	const bool release = (instruction & 0x0000'8000U) != 0;
	std::scoped_lock lock{exclusive_memory_mutex};
	std::uint64_t observed_first = 0;
	std::uint64_t observed_second = 0;
	std::uintptr_t key = 0;
	if (!atomic_compare_swap_pair_value(memory, address, element_size,
			compare_first, compare_second, replacement_first,
			replacement_second, acquire, release, observed_first,
			observed_second, key))
		return {StopReason::memory_fault, address, instruction, 0, true};
	reservation_store(key);
	cpu.write_register(compare_register, observed_first, false);
	cpu.write_register(compare_register + 1, observed_second, false);
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_atomic(CpuState &cpu, GuestMemory &memory,
	std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const bool unprivileged = (instruction & 0x0100'0000U) != 0;
	const unsigned size_code = instruction >> 30;
	const unsigned o3 = (instruction >> 15) & 1U;
	const unsigned opcode = (instruction >> 12) & 7U;
	unsigned operation;
	std::size_t size;
	if (unprivileged) {
		if (size_code > 1U ||
		    (!o3 && opcode != 0U && opcode != 1U && opcode != 3U) ||
		    (o3 && opcode != 0U))
			return {StopReason::illegal_instruction, pc, instruction, 0,
				false};
		size = std::size_t{4} << size_code;
		operation = o3 ? 8U : opcode;
	} else {
		if (o3 && opcode != 0U)
			return {StopReason::illegal_instruction, pc, instruction, 0,
				false};
		size = std::size_t{1} << size_code;
		operation = o3 ? 8U : opcode;
	}
	const unsigned source = (instruction >> 16) & 31U;
	const unsigned destination = instruction & 31U;
	const unsigned base_register = (instruction >> 5) & 31U;
	const auto address = cpu.read_register(base_register, true);
	if ((base_register == 31U && (address & 15U) != 0) ||
	    (address & (size - 1U)) != 0)
		return {StopReason::memory_fault, address, instruction, 0, true};
	const auto operand = cpu.read_register(source, false);
	const bool acquire = (instruction & 0x0080'0000U) != 0 &&
		destination != 31U;
	const bool release = (instruction & 0x0040'0000U) != 0;
	std::scoped_lock lock{exclusive_memory_mutex};
	std::uint64_t observed = 0;
	std::uintptr_t key = 0;
	if (!atomic_rmw_value(memory, address, size, operation, operand, acquire,
			release, observed, key))
		return {StopReason::memory_fault, address, instruction, 0, true};
	reservation_store(key);
	cpu.write_register(destination, observed, false);
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_atomic_floating(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const auto decoded = detail::decode_atomic_floating(instruction);
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const std::size_t size = decoded.width / 8U;
	const unsigned base_register = (instruction >> 5) & 31U;
	const auto tagged_address = cpu.read_register(base_register, true);
	/* CheckSPAlignment precedes element alignment, address translation, tag
	 * checking, FP-state materialization, and memory observation. */
	if (base_register == 31U && (tagged_address & 15U) != 0U)
		return {StopReason::memory_fault, tagged_address, instruction, 0,
			true};
	const auto untagged_address =
		base_register != 31U && cpu.mte_enabled ?
		tagged_address & 0x00ff'ffff'ffff'ffffULL : tagged_address;
	if ((untagged_address & (size - 1U)) != 0U)
		return {StopReason::memory_fault, untagged_address, instruction, 0,
			true};
	std::uint64_t address{};
	std::uint64_t tag_fault{};
	if (!simd_structure_address(cpu, memory, tagged_address,
		    base_register == 31U, base_register != 31U, size, size,
		    MemoryAccess::write, address, tag_fault))
		return {StopReason::memory_fault, tag_fault, instruction, 0, true};

	/* Capture the complete source before an aliased destination is written.
	 * Lazy FP materialization happens only after every decode, alignment,
	 * permission, and allocation-tag check has passed.
	 */
	cpu.access_fp_state();
	const unsigned source = (instruction >> 16) & 31U;
	const auto operand = detail::decode_little_endian(
		std::span{cpu.vectors[source].bytes}.first(size));
	std::scoped_lock lock{exclusive_memory_mutex};
	std::uint64_t observed = 0;
	std::uintptr_t key = 0;
	if (!atomic_floating_rmw_value(memory, address, decoded, operand,
			cpu.fpcr, observed, key))
		return {StopReason::memory_fault, address, instruction, 0, true};
	reservation_store(key);
	if (decoded.return_old) {
		const unsigned destination = instruction & 31U;
		cpu.vectors[destination].bytes = {};
		detail::encode_little_endian(observed,
			std::span{cpu.vectors[destination].bytes}.first(size));
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_atomic_pair_128(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const unsigned o3 = (instruction >> 15) & 1U;
	const unsigned opcode = (instruction >> 12) & 7U;
	const unsigned operation = o3 ? 8U : opcode;
	const unsigned first = instruction & 31U;
	const unsigned second = (instruction >> 16) & 31U;
	if ((instruction & 0x4000'0000U) != 0U ||
	    (o3 != 0U && opcode != 0U) ||
	    (operation != 1U && operation != 3U && operation != 8U) ||
	    first == 31U || second == 31U || first == second)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const unsigned base_register = (instruction >> 5) & 31U;
	const auto address = cpu.read_register(base_register, true);
	if ((address & 15U) != 0)
		return {StopReason::memory_fault, address, instruction, 0, true};
	/* Capture both operands before either result can overwrite an overlapping
	 * base or transfer register. */
	const auto operand_first = cpu.read_register(first, false);
	const auto operand_second = cpu.read_register(second, false);
	const bool acquire = (instruction & 0x0080'0000U) != 0;
	const bool release = (instruction & 0x0040'0000U) != 0;
	std::scoped_lock lock{exclusive_memory_mutex};
	std::uint64_t observed_first = 0;
	std::uint64_t observed_second = 0;
	std::uintptr_t key = 0;
	if (!atomic_rmw_pair_128_value(memory, address, operation, operand_first,
			operand_second, acquire, release, observed_first,
			observed_second, key))
		return {StopReason::memory_fault, address, instruction, 0, true};
	reservation_store(key);
	cpu.write_register(first, observed_first, false);
	cpu.write_register(second, observed_second, false);
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_rcw(CpuState &cpu, GuestMemory &memory,
	std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const bool compare_swap =
		(instruction & 0xbf20'fc00U) == 0x1920'0800U;
	const bool modify =
		(instruction & 0xbf20'8c00U) == 0x3820'8000U;
	const unsigned encoded_operation = (instruction >> 12) & 7U;
	if ((!compare_swap && !modify) ||
	    (modify && (encoded_operation < 1U || encoded_operation > 3U)))
		return {StopReason::illegal_instruction, pc, instruction, 0, false};

	const bool soft = (instruction & 0x4000'0000U) != 0;
	const unsigned base_register = (instruction >> 5) & 31U;
	const auto address = cpu.read_register(base_register, true);
	if ((base_register == 31U && (address & 15U) != 0) ||
	    (address & 7U) != 0)
		return {StopReason::memory_fault, address, instruction, 0, true};
	const unsigned first = (instruction >> 16) & 31U;
	const unsigned second = instruction & 31U;
	const unsigned result_register = compare_swap ? first : second;
	const unsigned source_register = compare_swap ? second : first;
	const auto compare = compare_swap ?
		cpu.read_register(first, false) : 0;
	const auto operand = cpu.read_register(source_register, false);
	const bool acquire = (instruction & 0x0080'0000U) != 0 &&
		result_register != 31U;
	const bool release = (instruction & 0x0040'0000U) != 0;
	const unsigned operation = compare_swap ? 0U : encoded_operation;

	std::scoped_lock lock{exclusive_memory_mutex};
	std::uint64_t observed = 0;
	std::uint32_t nzcv = 0;
	std::uintptr_t key = 0;
	if (!atomic_rcw_value(memory, address, operation, soft, compare, operand,
		acquire, release, observed, nzcv, key))
		return {StopReason::memory_fault, address, instruction, 0, true};
	reservation_store(key);
	cpu.write_register(result_register, observed, false);
	cpu.nzcv = nzcv;
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_rcw_pair(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const bool compare_swap =
		(instruction & 0xbf20'fc00U) == 0x1920'0c00U;
	const bool modify =
		(instruction & 0xbf20'8c00U) == 0x1920'8000U;
	const unsigned encoded_operation = (instruction >> 12) & 7U;
	if ((!compare_swap && !modify) ||
	    (modify && (encoded_operation < 1U || encoded_operation > 3U)))
		return {StopReason::illegal_instruction, pc, instruction, 0, false};

	const unsigned first = instruction & 31U;
	const unsigned second = (instruction >> 16) & 31U;
	if ((compare_swap && ((first & 1U) != 0 || (second & 1U) != 0)) ||
	    (modify && (first == 31U || second == 31U || first == second)))
		return {StopReason::illegal_instruction, pc, instruction, 0, false};

	const unsigned base_register = (instruction >> 5) & 31U;
	const auto address = cpu.read_register(base_register, true);
	if ((address & 15U) != 0)
		return {StopReason::memory_fault, address, instruction, 0, true};

	std::uint64_t compare_first = 0;
	std::uint64_t compare_second = 0;
	std::uint64_t operand_first;
	std::uint64_t operand_second;
	unsigned result_first;
	unsigned result_second;
	if (compare_swap) {
		result_first = second;
		result_second = second + 1U;
		compare_first = cpu.read_register(result_first, false);
		compare_second = cpu.read_register(result_second, false);
		operand_first = cpu.read_register(first, false);
		operand_second = cpu.read_register(first + 1U, false);
	} else {
		result_first = first;
		result_second = second;
		operand_first = cpu.read_register(first, false);
		operand_second = cpu.read_register(second, false);
	}

	const bool soft = (instruction & 0x4000'0000U) != 0;
	const bool acquire = (instruction & 0x0080'0000U) != 0;
	const bool release = (instruction & 0x0040'0000U) != 0;
	const unsigned operation = compare_swap ? 0U : encoded_operation;
	std::scoped_lock lock{exclusive_memory_mutex};
	std::uint64_t observed_first = 0;
	std::uint64_t observed_second = 0;
	std::uint32_t nzcv = 0;
	std::uintptr_t key = 0;
	if (!atomic_rcw_pair_value(memory, address, operation, soft,
			compare_first, compare_second, operand_first, operand_second,
			acquire, release, observed_first, observed_second, nzcv, key))
		return {StopReason::memory_fault, address, instruction, 0, true};
	reservation_store(key);
	cpu.write_register(result_first, observed_first, false);
	cpu.write_register(result_second, observed_second, false);
	cpu.nzcv = nzcv;
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_rcpc(CpuState &cpu, GuestMemory &memory,
	std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const bool no_offset =
		(instruction & 0x3fff'fc00U) == 0x38bf'c000U;
	const unsigned size_code = instruction >> 30;
	const unsigned operation = no_offset ? 1U : (instruction >> 22) & 3U;
	/* Unscaled opc=00 is STLUR, opc=01 is zero-extending LDAPUR, and the
	 * remaining allocated combinations are the signed LDAPUR forms. */
	if (!no_offset && ((operation == 2U && size_code == 3U) ||
	    (operation == 3U && size_code >= 2U)))
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const bool load = operation != 0U;
	const bool signed_load = operation >= 2U;
	const bool result_64 = size_code == 3U || operation == 2U;
	const std::size_t size = std::size_t{1} << size_code;
	const auto offset = no_offset ? std::int64_t{0} : sign_extend(
		(instruction >> 12) & 0x1ffU, 9);
	const unsigned base_register = (instruction >> 5) & 31U;
	const unsigned transfer = instruction & 31U;
	const auto base = cpu.read_register(base_register, true);
	const auto address = base + static_cast<std::uint64_t>(offset);
	if ((address & (size - 1U)) != 0)
		return {StopReason::memory_fault, address, instruction, 0, !load};

	if (load) {
		std::uint64_t value = 0;
		std::uintptr_t key = 0;
		if (!atomic_load_value(memory, address, size,
				std::memory_order_acquire, value, key))
			return {StopReason::memory_fault, address, instruction, 0, false};
		if (signed_load)
			value = static_cast<std::uint64_t>(sign_extend(value,
				static_cast<unsigned>(size * 8)));
		if (!result_64)
			value &= 0xffff'ffffU;
		cpu.write_register(transfer, value, false);
	} else {
		const auto value = cpu.read_register(transfer, false);
		std::scoped_lock lock{exclusive_memory_mutex};
		std::uintptr_t key = 0;
		if (atomic_store_value(memory, address, size, value,
				std::memory_order_release, nullptr, key) !=
		    AtomicStoreResult::stored)
			return {StopReason::memory_fault, address, instruction, 0, true};
		reservation_store(key);
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_ordered_pair(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const auto fixed = instruction & 0xffe0'fc00U;
	bool load;
	bool writeback;
	bool consistent_pair = false;
	switch (fixed) {
	case 0x9900'0800U: // STILP W, pre-index
	case 0xd900'0800U: // STILP X, pre-index
		load = false;
		writeback = true;
		break;
	case 0x9900'1800U: // STILP W, no offset
	case 0xd900'1800U: // STILP X, no offset
		load = false;
		writeback = false;
		break;
	case 0x9940'0800U: // LDIAPP W, post-index
	case 0xd940'0800U: // LDIAPP X, post-index
		load = true;
		writeback = true;
		break;
	case 0x9940'1800U: // LDIAPP W, no offset
	case 0xd940'1800U: // LDIAPP X, no offset
		load = true;
		writeback = false;
		break;
	case 0xd900'5800U: // STLP X
		load = false;
		writeback = false;
		consistent_pair = true;
		break;
	case 0xd940'5800U: // LDAP X
	case 0xd940'7800U: // LDAPP X
		load = true;
		writeback = false;
		consistent_pair = true;
		break;
	default:
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	}
	const unsigned size_code = instruction >> 30;
	const std::size_t element_size = std::size_t{1} << size_code;
	const auto total_size = element_size * 2;
	const unsigned first = instruction & 31U;
	const unsigned second = (instruction >> 16) & 31U;
	/* Select UNDEF for visible duplicate load destinations. XZR/XZR has no
	 * data result and deterministically selects the allowed UNKNOWN outcome. */
	if (load && first == second && first != 31U)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const unsigned base_register = (instruction >> 5) & 31U;
	const auto base = cpu.read_register(base_register, true);
	const auto address = load || !writeback ? base : base - total_size;
	if ((address & (element_size - 1U)) != 0)
		return {StopReason::memory_fault, address, instruction, 0, !load};

	std::scoped_lock lock{exclusive_memory_mutex};
	std::uintptr_t key = 0;
	if (load) {
		std::uint64_t first_value = 0;
		std::uint64_t second_value = 0;
		/* LDAP/LDAPP suppress their acquire effect unless both results are
		 * visible. LDIAPP always has AcquirePC semantics. The reference
		 * interpreter deliberately uses the stronger C++ acquire order for
		 * both architectural acquire models.
		 */
		const auto order = !consistent_pair ||
			(first != 31U && second != 31U) ?
			std::memory_order_acquire : std::memory_order_relaxed;
		if (!atomic_load_pair_value(memory, address, element_size,
				order, first_value, second_value, key))
			return {StopReason::memory_fault, address, instruction, 0, false};
		cpu.write_register(first, first_value, false);
		cpu.write_register(second, second_value, false);
		/* Post-index overlap selects WBSUPPRESS. */
		if (writeback && (base_register == 31U ||
		    (base_register != first && base_register != second)))
			cpu.write_register(base_register, base + total_size, true);
	} else {
		/* Capture both sources before pre-index writeback, including base/source
		 * overlap, for the selected Constraint_NONE behavior. */
		const auto first_value = cpu.read_register(first, false);
		const auto second_value = cpu.read_register(second, false);
		if (atomic_store_pair_value(memory, address, element_size,
				first_value, second_value, std::memory_order_release,
				nullptr, key) != AtomicStoreResult::stored)
			return {StopReason::memory_fault, address, instruction, 0, true};
		reservation_store(key);
		if (writeback)
			cpu.write_register(base_register, address, true);
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_ordered_writeback(
	CpuState &cpu, GuestMemory &memory, std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const unsigned size_code = instruction >> 30;
	if (size_code < 2U)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const std::size_t size = std::size_t{1} << size_code;
	const bool load = (instruction & 0x0040'0000U) != 0;
	const unsigned base_register = (instruction >> 5) & 31U;
	const unsigned transfer = instruction & 31U;
	const auto base = cpu.read_register(base_register, true);
	const auto address = load ? base : base - size;
	const auto adjusted = load ? base + size : base - size;
	if ((address & (size - 1U)) != 0)
		return {StopReason::memory_fault, address, instruction, 0, !load};

	if (load) {
		std::uint64_t value = 0;
		std::uintptr_t key = 0;
		if (!atomic_load_value(memory, address, size,
				std::memory_order_acquire, value, key))
			return {StopReason::memory_fault, address, instruction, 0,
				false};
		cpu.write_register(transfer, value, false);
	} else {
		const auto value = cpu.read_register(transfer, false);
		std::scoped_lock lock{exclusive_memory_mutex};
		std::uintptr_t key = 0;
		if (atomic_store_value(memory, address, size, value,
				std::memory_order_release, nullptr, key) !=
		    AtomicStoreResult::stored)
			return {StopReason::memory_fault, address, instruction, 0,
				true};
		reservation_store(key);
	}
	/* The profile selects WBSUPPRESS for LDAPR base/destination overlap and
	 * Constraint_NONE for STLR, which stores the original base value. */
	if (!load || base_register == 31U || base_register != transfer)
		cpu.write_register(base_register, adjusted, true);
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

} // namespace uml::cpu::arm64
