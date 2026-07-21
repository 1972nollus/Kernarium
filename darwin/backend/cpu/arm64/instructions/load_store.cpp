// SPDX-License-Identifier: GPL-2.0-or-later
#include "../dispatcher.hpp"

#include "../detail/instruction_helpers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::backend::arm64 {

namespace {

class ExclusiveMemoryMutex final {
public:
	void lock() noexcept
	{
		while (locked_.test_and_set(std::memory_order_acquire)) {
			while (locked_.test(std::memory_order_relaxed))
				__asm__ volatile("yield");
		}
	}

	void unlock() noexcept
	{
		locked_.clear(std::memory_order_release);
	}

private:
	std::atomic_flag locked_ = ATOMIC_FLAG_INIT;
};

ExclusiveMemoryMutex exclusive_memory_mutex;

constexpr std::uintptr_t reservation_granule = 16;
constexpr std::size_t reservation_slot_count = 1U << 15;

struct ReservationSlot final {
	std::uintptr_t key{};
	std::uint64_t version{};
};

std::array<ReservationSlot, reservation_slot_count> reservation_slots;

[[nodiscard]] std::uintptr_t fallback_reservation_key(
	std::uint64_t address) noexcept
{
	/* Direct host locations are naturally aligned.  Mark synthetic guest-VA
	 * keys in bit zero so the two namespaces cannot alias.
	 */
	return static_cast<std::uintptr_t>(
		(address & ~(reservation_granule - 1)) | 1);
}

[[nodiscard]] std::uintptr_t host_reservation_key(std::byte *pointer) noexcept
{
	return reinterpret_cast<std::uintptr_t>(pointer) &
		~(reservation_granule - 1);
}

[[nodiscard]] ReservationSlot &reservation_slot(std::uintptr_t key) noexcept
{
	auto hash = key >> 4;
	hash ^= hash >> 17;
	return reservation_slots[hash & (reservation_slot_count - 1)];
}

[[nodiscard]] std::uint64_t reservation_version(std::uintptr_t key) noexcept
{
	auto &slot = reservation_slot(key);
	if (slot.key != key) {
		slot.key = key;
		++slot.version;
	}
	return slot.version;
}

void reservation_store(std::uintptr_t key) noexcept
{
	auto &slot = reservation_slot(key);
	if (slot.key != key)
		slot.key = key;
	++slot.version;
}

template<typename Integer>
[[nodiscard]] std::uint64_t atomic_load_at(
	std::byte *pointer, std::memory_order order) noexcept
{
	auto &value = *reinterpret_cast<Integer *>(pointer);
	return std::atomic_ref<Integer>{value}.load(order);
}

template<typename Integer>
void atomic_store_at(std::byte *pointer, std::uint64_t value,
			     std::memory_order order) noexcept
{
	auto &destination = *reinterpret_cast<Integer *>(pointer);
	std::atomic_ref<Integer>{destination}.store(
		static_cast<Integer>(value), order);
}

enum class AtomicStoreResult {
	stored,
	reservation_mismatch,
	fault,
};

[[nodiscard]] bool atomic_load_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, std::memory_order order,
	std::uint64_t &value, std::uintptr_t &key)
{
	auto *pointer = memory.atomic_pointer(
		address, size, MemoryAccess::read);
	if (pointer != nullptr &&
	    reinterpret_cast<std::uintptr_t>(pointer) % size == 0) {
		switch (size) {
		case 1: value = atomic_load_at<std::uint8_t>(pointer, order); break;
		case 2: value = atomic_load_at<std::uint16_t>(pointer, order); break;
		case 4: value = atomic_load_at<std::uint32_t>(pointer, order); break;
		case 8: value = atomic_load_at<std::uint64_t>(pointer, order); break;
		default: return false;
		}
		key = host_reservation_key(pointer);
		return true;
	}

	std::array<std::byte, 8> bytes{};
	if (!memory.read(address, std::span{bytes}.first(size)))
		return false;
	value = detail::decode_little_endian(std::span{bytes}.first(size));
	key = fallback_reservation_key(address);
	return true;
}

[[nodiscard]] AtomicStoreResult atomic_store_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, std::uint64_t value,
	std::memory_order order, const std::uintptr_t *expected_key,
	std::uintptr_t &key)
{
	auto *pointer = memory.atomic_pointer(
		address, size, MemoryAccess::write);
	if (pointer != nullptr &&
	    reinterpret_cast<std::uintptr_t>(pointer) % size == 0) {
		key = host_reservation_key(pointer);
		if (expected_key != nullptr && key != *expected_key)
			return AtomicStoreResult::reservation_mismatch;
		switch (size) {
		case 1: atomic_store_at<std::uint8_t>(pointer, value, order); break;
		case 2: atomic_store_at<std::uint16_t>(pointer, value, order); break;
		case 4: atomic_store_at<std::uint32_t>(pointer, value, order); break;
		case 8: atomic_store_at<std::uint64_t>(pointer, value, order); break;
		default: return AtomicStoreResult::fault;
		}
		return AtomicStoreResult::stored;
	}

	key = fallback_reservation_key(address);
	std::array<std::byte, 8> bytes{};
	detail::encode_little_endian(value, std::span{bytes}.first(size));
	if (!memory.write(address, std::span{bytes}.first(size)))
		return AtomicStoreResult::fault;
	/* A backend without a direct write pointer must perform the write before
	 * it can distinguish an access fault (notably a Linux COW fault) from a
	 * supported fallback access.  A successful fallback write is therefore
	 * the same reservation location established by its corresponding load;
	 * direct read/write aliases are checked above before any store occurs.
	 */
	return AtomicStoreResult::stored;
}

} // namespace

using detail::continue_after;
using detail::decode_little_endian;
using detail::encode_little_endian;
using detail::sign_extend;

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
		return {StopReason::unsupported_instruction, pc, instruction, 0,
			false};
	const std::size_t size = vector && operation >= 2 ? 16 :
		std::size_t{1} << size_code;
	if (!vector && size >= 4 && operation == 3)
		return {StopReason::unsupported_instruction, pc, instruction, 0,
			false};
	const bool load = vector ? (operation & 1U) != 0 : operation != 0;
	const auto address = cpu.read_register((instruction >> 5) & 0x1fU, true) +
		static_cast<std::uint64_t>((instruction >> 10) & 0xfffU) * size;
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
		if (!memory.write(address, std::span{bytes}.first(size)))
			return {StopReason::memory_fault, address, instruction, 0, true};
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
		/* Register-offset and unscaled PRFM encodings are advisory only. */
		cpu.pc += 4;
		return continue_after(pc, instruction);
	}
	if (vector && operation >= 2 && size_code != 0)
		return {StopReason::unsupported_instruction, pc, instruction, 0,
			false};
	const std::size_t size = vector && operation >= 2 ? 16 :
		std::size_t{1} << size_code;
	if (!vector && ((size == 8 && operation != 0 && operation != 1) ||
	    (size == 4 && operation == 3)))
		return {StopReason::unsupported_instruction, pc, instruction, 0,
			false};
	const bool load = vector ? (operation & 1U) != 0 : operation != 0;
	const unsigned base_register = (instruction >> 5) & 0x1fU;
	const auto base = cpu.read_register(base_register, true);
	std::uint64_t address = base;
	bool writeback = false;
	std::uint64_t adjusted = base;
	if ((instruction & 0x0020'0000U) != 0) {
		const unsigned option = (instruction >> 13) & 0x7U;
		if (option != 2 && option != 3 && option != 6 && option != 7)
			return {StopReason::unsupported_instruction, pc, instruction, 0,
				false};
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
		const unsigned mode = (instruction >> 10) & 0x3U;
		if (mode == 1) {
			address = base;
			writeback = true;
		} else {
			address = adjusted;
			writeback = mode == 3;
		}
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
		if (!memory.write(address, std::span{bytes}.first(size)))
			return {StopReason::memory_fault, address, instruction, 0, true};
	}
	if (writeback)
		cpu.write_register(base_register, adjusted, true);
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
	if (operation == 3 || (!vector && operation == 1 && !load))
		return {StopReason::unsupported_instruction, pc, instruction, 0,
			false};
	const std::size_t size = vector ? std::size_t{4} << operation :
		operation == 2 ? 8 : 4;
	const auto displacement = sign_extend((instruction >> 15) & 0x7fU, 7) *
		static_cast<std::int64_t>(size);
	const unsigned mode = (instruction >> 23) & 0x3U;
	const unsigned base_register = (instruction >> 5) & 0x1fU;
	const auto base = cpu.read_register(base_register, true);
	const auto adjusted = static_cast<std::uint64_t>(
		static_cast<std::int64_t>(base) + displacement);
	const auto address = mode == 1 ? base : adjusted;
	const bool writeback = mode == 1 || mode == 3;
	const unsigned first = instruction & 0x1fU;
	const unsigned second = (instruction >> 10) & 0x1fU;
	std::array<std::byte, 16> first_bytes{};
	std::array<std::byte, 16> second_bytes{};
	if (load) {
		if (!memory.read(address, std::span{first_bytes}.first(size)))
			return {StopReason::memory_fault, address, instruction, 0, false};
		if (!memory.read(address + size,
				 std::span{second_bytes}.first(size)))
			return {StopReason::memory_fault, address + size, instruction, 0,
				false};
		if (vector) {
			cpu.vectors[first].bytes.fill(std::byte{});
			cpu.vectors[second].bytes.fill(std::byte{});
			std::copy_n(first_bytes.begin(), size,
				cpu.vectors[first].bytes.begin());
			std::copy_n(second_bytes.begin(), size,
				cpu.vectors[second].bytes.begin());
		} else {
			auto first_value = decode_little_endian(
				std::span{first_bytes}.first(size));
			auto second_value = decode_little_endian(
				std::span{second_bytes}.first(size));
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
		std::array<std::byte, 32> pair_bytes{};
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
		if (!memory.write(address, std::span{pair_bytes}.first(size * 2)))
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
		return {StopReason::unsupported_instruction, pc, instruction, 0,
			false};
	const bool load = (instruction & 0x0040'0000U) != 0;
	const bool post_index = (instruction & 0x0080'0000U) != 0;
	const std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ?
		16 : 8;
	const std::size_t element_size = std::size_t{1} <<
		((instruction >> 10) & 0x3U);
	if (element_size > vector_size)
		return {StopReason::unsupported_instruction, pc, instruction, 0,
			false};
	const std::size_t transfer_size = vector_size * register_count;
	const unsigned base_register = (instruction >> 5) & 0x1fU;
	const auto address = cpu.read_register(base_register, true);
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
		if (!memory.write(address, std::span{bytes}.first(transfer_size)))
			return {StopReason::memory_fault, address, instruction, 0, true};
	}
	if (post_index) {
		const unsigned offset_register = (instruction >> 16) & 0x1fU;
		const auto offset = offset_register == 0x1fU ? transfer_size :
			cpu.read_register(offset_register, false);
		cpu.write_register(base_register, address + offset, true);
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
	const unsigned base_register = (instruction >> 5) & 0x1fU;
	const unsigned first_register = instruction & 0x1fU;
	const auto address = cpu.read_register(base_register, true);
	const std::size_t vector_size = full_vector ? 16 : 8;
	unsigned register_count;
	std::size_t element_size;
	std::size_t lane_offset = 0;
	bool replicate = false;

	if ((opcode == 6 || opcode == 7) && scale_bit == 0 && load) {
		replicate = true;
		register_count = opcode == 6 ? 1 + extra_register :
			3 + extra_register;
		element_size = std::size_t{1} << size;
	} else {
		register_count = (opcode & 1U) != 0 ?
			3 + extra_register : 1 + extra_register;
		const unsigned base_opcode = opcode & ~1U;
		std::size_t lane;
		if (base_opcode == 0) {
			element_size = 1;
			lane = (static_cast<std::size_t>(full_vector) << 3) |
				(scale_bit << 2) | size;
		} else if (base_opcode == 2) {
			element_size = 2;
			lane = (static_cast<std::size_t>(full_vector) << 2) |
				(scale_bit << 1) | (size & 1U);
		} else if (base_opcode == 4 && size <= 1) {
			element_size = std::size_t{4} << size;
			lane = (static_cast<std::size_t>(full_vector) <<
				(1U - size)) | scale_bit;
		} else {
			return {StopReason::unsupported_instruction, pc, instruction, 0,
				false};
		}
		lane_offset = lane * element_size;
		if (lane_offset + element_size > 16)
			return {StopReason::unsupported_instruction, pc, instruction, 0,
				false};
	}

	const std::size_t transfer_size = element_size * register_count;
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
		if (!memory.write(address, std::span{bytes}.first(transfer_size)))
			return {StopReason::memory_fault, address, instruction, 0, true};
	}
	if (post_index) {
		const unsigned offset_register = (instruction >> 16) & 0x1fU;
		const auto offset = offset_register == 0x1fU ? transfer_size :
			cpu.read_register(offset_register, false);
		cpu.write_register(base_register, address + offset, true);
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_exclusive(CpuState &cpu,
					    GuestMemory &memory,
					    std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	/* This backend implements the scalar acquire/release and exclusive forms.
	 * Pair and CAS/LSE encodings have different Rt2 fields and remain separate
	 * instruction classes.
	 */
	if (((instruction >> 10) & 0x1fU) != 0x1fU)
		return {StopReason::unsupported_instruction, pc, instruction, 0,
			false};
	const std::size_t size = std::size_t{1} << (instruction >> 30);
	const bool load = (instruction & 0x0040'0000U) != 0;
	const bool ordered = (instruction & 0x0000'8000U) != 0;
	const bool exclusive = (instruction & 0x0080'0000U) == 0;
	const auto address = cpu.read_register((instruction >> 5) & 0x1fU, true);
	const unsigned transfer = instruction & 0x1fU;
	if (load) {
		if (exclusive) {
			std::scoped_lock lock{exclusive_memory_mutex};
			std::uint64_t value = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_value(memory, address, size,
					       std::memory_order_relaxed, value, key))
				return {StopReason::memory_fault, address, instruction, 0,
					false};
			cpu.write_register(transfer, value, false);
			cpu.exclusive_address = address;
			cpu.exclusive_key = key;
			cpu.exclusive_value = value;
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
			const unsigned status = (instruction >> 16) & 0x1fU;
			std::uint64_t current = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_value(memory, address, size,
					       std::memory_order_relaxed, current, key))
				return {StopReason::memory_fault, address, instruction, 0,
					false};
			bool success = cpu.exclusive_address == address &&
				cpu.exclusive_size == size &&
				cpu.exclusive_key == key &&
				cpu.exclusive_version == reservation_version(key) &&
				cpu.exclusive_value == current;
			cpu.exclusive_address = ~std::uint64_t{0};
			cpu.exclusive_key = 0;
			cpu.exclusive_size = 0;
			if (success) {
				std::uintptr_t stored_key = 0;
				const auto store_result = atomic_store_value(
					memory, address, size, value,
					std::memory_order_release, &key, stored_key);
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
					ordered ? std::memory_order_release :
						std::memory_order_relaxed,
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
	if (load && ordered)
		std::atomic_thread_fence(std::memory_order_acquire);
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

} // namespace uml::backend::arm64
