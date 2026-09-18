// SPDX-License-Identifier: GPL-2.0-or-later
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

ExclusiveMemoryMutex exclusive_memory_mutex;

constexpr std::uintptr_t reservation_granule = 16;
constexpr std::size_t reservation_slot_count = 1U << 15;

struct ReservationSlot final {
	/*
	 * Hash collisions are allowed to fail a guest store-conditional
	 * spuriously.  Therefore a monotonically increasing version per hash
	 * bucket is sufficient: every store to any colliding reservation
	 * granule invalidates the snapshot, without needing a separately locked
	 * key/value pair.
	 *
	 * Native JIT store blocks may finish concurrently on several vCPUs.
	 * Keeping the version atomic lets their post-store invalidation remain
	 * lock-free while semantic exclusive operations continue to serialize
	 * the actual memory transaction with exclusive_memory_mutex.
	 */
	std::atomic<std::uint64_t> version{};
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

[[nodiscard]] std::uintptr_t reservation_key(GuestMemory &memory,
	std::uint64_t address, std::size_t size, MemoryAccess access) noexcept
{
	auto *pointer = memory.reservation_pointer(address, size, access);
	return pointer != nullptr ? host_reservation_key(pointer) :
		fallback_reservation_key(address);
}

[[nodiscard]] ReservationSlot &reservation_slot(std::uintptr_t key) noexcept
{
	auto hash = key >> 4;
	hash ^= hash >> 17;
	return reservation_slots[hash & (reservation_slot_count - 1)];
}

[[nodiscard]] std::uint64_t reservation_version(std::uintptr_t key) noexcept
{
	return reservation_slot(key).version.load(std::memory_order_acquire);
}

void reservation_store(std::uintptr_t key) noexcept
{
	reservation_slot(key).version.fetch_add(1, std::memory_order_release);
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

template<typename Integer>
[[nodiscard]] std::uint64_t atomic_compare_swap_at(std::byte *pointer,
	std::uint64_t compare, std::uint64_t replacement,
	std::memory_order success_order, std::memory_order failure_order) noexcept
{
	auto &value = *reinterpret_cast<Integer *>(pointer);
	auto expected = static_cast<Integer>(compare);
	(void)std::atomic_ref<Integer>{value}.compare_exchange_strong(
		expected, static_cast<Integer>(replacement), success_order,
		failure_order);
	return expected;
}

void atomic_compare_swap_pair_128_at(std::byte *pointer,
	std::uint64_t compare_first, std::uint64_t compare_second,
	std::uint64_t replacement_first, std::uint64_t replacement_second,
	std::uint64_t &observed_first, std::uint64_t &observed_second) noexcept
{
	/* CASPAL is deliberately stronger than the requested guest ordering. The
	 * slow path favors a compact, race-free operation over weakening the host
	 * instruction for each encoding variant. Fixed adjacent registers satisfy
	 * CASP's architectural pair constraints.
	 */
	__asm__ volatile(
		"mov x9, %6\n"
		"mov x12, %2\n"
		"mov x13, %3\n"
		"mov x14, %4\n"
		"mov x15, %5\n"
		"caspal x12, x13, x14, x15, [x9]\n"
		"mov %0, x12\n"
		"mov %1, x13"
		: "=r"(observed_first), "=r"(observed_second)
		: "r"(compare_first), "r"(compare_second),
		  "r"(replacement_first), "r"(replacement_second), "r"(pointer)
		: "x9", "x12", "x13", "x14", "x15", "cc", "memory");
}

void atomic_load_pair_128_at(std::byte *pointer, std::uint64_t &first,
	std::uint64_t &second) noexcept
{
	/* The snapshot only seeds a later validating CASP or supplies an atomic
	 * pair load. Clear the implementation reservation immediately.
	 */
	__asm__ volatile(
		"ldxp %0, %1, [%2]\n"
		"clrex"
		: "=&r"(first), "=&r"(second)
		: "r"(pointer)
		: "memory");
}

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
	value = arm64::detail::decode_little_endian(std::span{bytes}.first(size));
	if (order != std::memory_order_relaxed)
		std::atomic_thread_fence(std::memory_order_acquire);
	key = reservation_key(memory, address, size, MemoryAccess::read);
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

	key = reservation_key(memory, address, size, MemoryAccess::write);
	std::array<std::byte, 8> bytes{};
	arm64::detail::encode_little_endian(value, std::span{bytes}.first(size));
	if (order != std::memory_order_relaxed)
		std::atomic_thread_fence(std::memory_order_release);
	if (!memory.write(address, std::span{bytes}.first(size)))
		return AtomicStoreResult::fault;
	/* A emulator without a direct write pointer must perform the write before
	 * it can distinguish an access fault (notably a Linux COW fault) from a
	 * supported fallback access.  A successful fallback write is therefore
	 * the same reservation location established by its corresponding load;
	 * direct read/write aliases are checked above before any store occurs.
	 */
	return AtomicStoreResult::stored;
}

[[nodiscard]] bool atomic_compare_swap_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, std::uint64_t compare,
	std::uint64_t replacement, bool acquire, bool release,
	std::uint64_t &observed, std::uintptr_t &key)
{
	const auto success_order = acquire && release ? std::memory_order_acq_rel :
		acquire ? std::memory_order_acquire :
		release ? std::memory_order_release : std::memory_order_relaxed;
	const auto failure_order = acquire ? std::memory_order_acquire :
		std::memory_order_relaxed;
	auto *pointer = memory.atomic_pointer(address, size, MemoryAccess::write);
	if (pointer != nullptr &&
	    reinterpret_cast<std::uintptr_t>(pointer) % size == 0) {
		switch (size) {
		case 1:
			observed = atomic_compare_swap_at<std::uint8_t>(pointer,
				compare, replacement, success_order, failure_order);
			break;
		case 2:
			observed = atomic_compare_swap_at<std::uint16_t>(pointer,
				compare, replacement, success_order, failure_order);
			break;
		case 4:
			observed = atomic_compare_swap_at<std::uint32_t>(pointer,
				compare, replacement, success_order, failure_order);
			break;
		case 8:
			observed = atomic_compare_swap_at<std::uint64_t>(pointer,
				compare, replacement, success_order, failure_order);
			break;
		default: return false;
		}
		key = host_reservation_key(pointer);
		return true;
	}

	std::array<std::byte, 8> bytes{};
	if (!memory.read(address, std::span{bytes}.first(size)))
		return false;
	observed = arm64::detail::decode_little_endian(std::span{bytes}.first(size));
	if (observed == (compare & (size == 8 ? ~std::uint64_t{0} :
		((std::uint64_t{1} << (size * 8)) - 1)))) {
		if (release)
			std::atomic_thread_fence(std::memory_order_release);
		arm64::detail::encode_little_endian(replacement,
			std::span{bytes}.first(size));
	}
	/* Select the architecture-permitted write-back behavior on a failed
	 * comparison too. Besides deterministic reservation effects, this
	 * validates write permission for every atomic read-modify-write access. */
	if (!memory.write(address, std::span{bytes}.first(size)))
		return false;
	if (acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	key = reservation_key(memory, address, size, MemoryAccess::write);
	return true;
}

[[nodiscard]] bool atomic_compare_swap_pair_value(GuestMemory &memory,
	std::uint64_t address, std::size_t element_size,
	std::uint64_t compare_first, std::uint64_t compare_second,
	std::uint64_t replacement_first, std::uint64_t replacement_second,
	bool acquire, bool release, std::uint64_t &observed_first,
	std::uint64_t &observed_second, std::uintptr_t &key)
{
	const auto total_size = element_size * 2;
	std::array<std::byte, 16> bytes{};
	auto *pointer = memory.atomic_pointer(
		address, total_size, MemoryAccess::write);
	if (pointer != nullptr &&
	    reinterpret_cast<std::uintptr_t>(pointer) % total_size == 0) {
		key = host_reservation_key(pointer);
		if (total_size == 8) {
			const auto packed_compare =
				(compare_first & 0xffff'ffffU) |
				((compare_second & 0xffff'ffffU) << 32);
			const auto packed_replacement =
				(replacement_first & 0xffff'ffffU) |
				((replacement_second & 0xffff'ffffU) << 32);
			const auto success_order = acquire && release ?
				std::memory_order_acq_rel : acquire ?
				std::memory_order_acquire : release ?
				std::memory_order_release : std::memory_order_relaxed;
			const auto failure_order = acquire ?
				std::memory_order_acquire : std::memory_order_relaxed;
			const auto packed_observed = atomic_compare_swap_at<std::uint64_t>(
				pointer, packed_compare, packed_replacement,
				success_order, failure_order);
			observed_first = packed_observed & 0xffff'ffffU;
			observed_second = packed_observed >> 32;
			return true;
		}
		atomic_compare_swap_pair_128_at(pointer, compare_first,
			compare_second, replacement_first, replacement_second,
			observed_first, observed_second);
		return true;
	} else {
		if (!memory.read(address, std::span{bytes}.first(total_size)))
			return false;
		key = reservation_key(
			memory, address, total_size, MemoryAccess::write);
	}
	observed_first = arm64::detail::decode_little_endian(
		std::span{bytes}.first(element_size));
	observed_second = arm64::detail::decode_little_endian(
		std::span{bytes}.subspan(element_size, element_size));
	const auto mask = element_size == 8 ? ~std::uint64_t{0} :
		std::uint64_t{0xffff'ffffU};
	if (observed_first != (compare_first & mask) ||
	    observed_second != (compare_second & mask)) {
		/* Choose the permitted write-back of the observed pair so failed
		 * comparisons still validate atomic write permission. */
		if (!memory.write(address, std::span{bytes}.first(total_size)))
			return false;
		if (acquire)
			std::atomic_thread_fence(std::memory_order_acquire);
		return true;
	}
	if (release)
		std::atomic_thread_fence(std::memory_order_release);
	arm64::detail::encode_little_endian(replacement_first,
		std::span{bytes}.first(element_size));
	arm64::detail::encode_little_endian(replacement_second,
		std::span{bytes}.subspan(element_size, element_size));
	if (!memory.write(address, std::span{bytes}.first(total_size)))
		return false;
	if (acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	return true;
}

[[nodiscard]] constexpr std::uint64_t atomic_operation_result(unsigned operation,
	std::uint64_t observed, std::uint64_t operand, std::size_t size) noexcept
{
	const auto mask = size == 8 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << (size * 8)) - 1;
	observed &= mask;
	operand &= mask;
	switch (operation) {
	case 0: return (observed + operand) & mask;
	case 1: return observed & ~operand & mask;
	case 2: return observed ^ operand;
	case 3: return observed | operand;
	case 4:
		return arm64::detail::sign_extend(observed, static_cast<unsigned>(size * 8)) >=
			arm64::detail::sign_extend(operand, static_cast<unsigned>(size * 8)) ?
			observed : operand;
	case 5:
		return arm64::detail::sign_extend(observed, static_cast<unsigned>(size * 8)) <=
			arm64::detail::sign_extend(operand, static_cast<unsigned>(size * 8)) ?
			observed : operand;
	case 6: return std::max(observed, operand);
	case 7: return std::min(observed, operand);
	case 8: return operand;
	default: return observed;
	}
}

template<typename Integer>
[[nodiscard]] std::uint64_t atomic_rmw_at(std::byte *pointer,
	unsigned operation, std::uint64_t operand, std::memory_order order) noexcept
{
	auto &value = *reinterpret_cast<Integer *>(pointer);
	auto reference = std::atomic_ref<Integer>{value};
	auto expected = reference.load(std::memory_order_relaxed);
	const auto failure_order = order == std::memory_order_acquire ||
		order == std::memory_order_acq_rel ? std::memory_order_acquire :
		std::memory_order_relaxed;
	for (;;) {
		const auto observed = expected;
		const auto desired = static_cast<Integer>(atomic_operation_result(
			operation, observed, operand, sizeof(Integer)));
		if (reference.compare_exchange_weak(
				expected, desired, order, failure_order))
			return observed;
	}
}

template<typename Integer>
[[nodiscard]] std::uint64_t atomic_floating_rmw_at(std::byte *pointer,
	arm64::detail::AtomicFloatingOperation operation, std::uint64_t operand,
	unsigned width, bool bfloat, std::uint32_t fpcr,
	std::memory_order order) noexcept
{
	auto &value = *reinterpret_cast<Integer *>(pointer);
	auto reference = std::atomic_ref<Integer>{value};
	auto expected = reference.load(std::memory_order_relaxed);
	const auto failure_order = order == std::memory_order_acquire ||
		order == std::memory_order_acq_rel ? std::memory_order_acquire :
		std::memory_order_relaxed;
	for (;;) {
		const auto observed = expected;
		const auto desired = static_cast<Integer>(
			arm64::detail::atomic_floating_result(operation, observed, operand,
				width, bfloat, fpcr));
		if (reference.compare_exchange_weak(
				expected, desired, order, failure_order))
			return observed;
	}
}

[[nodiscard]] bool atomic_floating_rmw_value(GuestMemory &memory,
	std::uint64_t address, const arm64::detail::AtomicFloating &decoded,
	std::uint64_t operand, std::uint32_t fpcr, std::uint64_t &observed,
	std::uintptr_t &key)
{
	const auto order = decoded.acquire && decoded.release ?
		std::memory_order_acq_rel : decoded.acquire ?
		std::memory_order_acquire : decoded.release ?
		std::memory_order_release : std::memory_order_relaxed;
	const std::size_t size = decoded.width / 8U;
	auto *pointer = memory.atomic_pointer(address, size, MemoryAccess::write);
	if (pointer != nullptr &&
	    reinterpret_cast<std::uintptr_t>(pointer) % size == 0) {
		switch (size) {
		case 2:
			observed = atomic_floating_rmw_at<std::uint16_t>(pointer,
				decoded.operation, operand, decoded.width, decoded.bfloat,
				fpcr, order);
			break;
		case 4:
			observed = atomic_floating_rmw_at<std::uint32_t>(pointer,
				decoded.operation, operand, decoded.width, decoded.bfloat,
				fpcr, order);
			break;
		case 8:
			observed = atomic_floating_rmw_at<std::uint64_t>(pointer,
				decoded.operation, operand, decoded.width, decoded.bfloat,
				fpcr, order);
			break;
		default: return false;
		}
		key = host_reservation_key(pointer);
		return true;
	}

	std::array<std::byte, 8> bytes{};
	if (!memory.read(address, std::span{bytes}.first(size)))
		return false;
	observed = arm64::detail::decode_little_endian(std::span{bytes}.first(size));
	const auto desired = arm64::detail::atomic_floating_result(decoded.operation,
		observed, operand, decoded.width, decoded.bfloat, fpcr);
	if (decoded.release)
		std::atomic_thread_fence(std::memory_order_release);
	arm64::detail::encode_little_endian(desired, std::span{bytes}.first(size));
	if (!memory.write(address, std::span{bytes}.first(size)))
		return false;
	if (decoded.acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	key = reservation_key(memory, address, size, MemoryAccess::write);
	return true;
}

[[nodiscard]] bool atomic_rmw_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, unsigned operation,
	std::uint64_t operand, bool acquire, bool release,
	std::uint64_t &observed, std::uintptr_t &key)
{
	const auto order = acquire && release ? std::memory_order_acq_rel :
		acquire ? std::memory_order_acquire :
		release ? std::memory_order_release : std::memory_order_relaxed;
	auto *pointer = memory.atomic_pointer(address, size, MemoryAccess::write);
	if (pointer != nullptr &&
	    reinterpret_cast<std::uintptr_t>(pointer) % size == 0) {
		switch (size) {
		case 1: observed = atomic_rmw_at<std::uint8_t>(pointer,
			operation, operand, order); break;
		case 2: observed = atomic_rmw_at<std::uint16_t>(pointer,
			operation, operand, order); break;
		case 4: observed = atomic_rmw_at<std::uint32_t>(pointer,
			operation, operand, order); break;
		case 8: observed = atomic_rmw_at<std::uint64_t>(pointer,
			operation, operand, order); break;
		default: return false;
		}
		key = host_reservation_key(pointer);
		return true;
	}

	std::array<std::byte, 8> bytes{};
	if (!memory.read(address, std::span{bytes}.first(size)))
		return false;
	observed = arm64::detail::decode_little_endian(std::span{bytes}.first(size));
	if (release)
		std::atomic_thread_fence(std::memory_order_release);
	arm64::detail::encode_little_endian(
		atomic_operation_result(operation, observed, operand, size),
		std::span{bytes}.first(size));
	if (!memory.write(address, std::span{bytes}.first(size)))
		return false;
	if (acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	key = reservation_key(memory, address, size, MemoryAccess::write);
	return true;
}

[[nodiscard]] constexpr std::pair<std::uint64_t, std::uint32_t>
rcw_result(unsigned operation, bool soft, std::uint64_t observed,
	std::uint64_t compare, std::uint64_t operand) noexcept
{
	std::uint64_t desired;
	if (operation == 0U) {
		if (observed != compare)
			return {observed, 0xa000'0000U};
		desired = operand;
	} else if (operation == 1U) {
		desired = observed & ~operand;
	} else if (operation == 2U) {
		desired = operand;
	} else {
		desired = observed | operand;
	}
	/* The userspace executor has translation-table protection disabled and
	 * models RCWSMASK_EL1 as maximally permissive. The remaining RCWS state
	 * check forbids changing descriptor bit zero. Z therefore remains clear;
	 * C reports the software-check result.
	 */
	if (soft && ((observed ^ desired) & 1U) != 0)
		return {observed, 0U};
	return {desired, 0x2000'0000U};
}

[[nodiscard]] bool atomic_rcw_value(GuestMemory &memory,
	std::uint64_t address, unsigned operation, bool soft,
	std::uint64_t compare, std::uint64_t operand, bool acquire, bool release,
	std::uint64_t &observed, std::uint32_t &nzcv, std::uintptr_t &key)
{
	const auto success_order = acquire && release ? std::memory_order_acq_rel :
		acquire ? std::memory_order_acquire :
		release ? std::memory_order_release : std::memory_order_relaxed;
	const auto failure_order = acquire ? std::memory_order_acquire :
		std::memory_order_relaxed;
	auto *pointer = memory.atomic_pointer(address, 8, MemoryAccess::write);
	if (pointer != nullptr &&
	    reinterpret_cast<std::uintptr_t>(pointer) % 8 == 0) {
		auto &storage = *reinterpret_cast<std::uint64_t *>(pointer);
		auto reference = std::atomic_ref<std::uint64_t>{storage};
		auto expected = reference.load(std::memory_order_relaxed);
		for (;;) {
			observed = expected;
			const auto [desired, flags] = rcw_result(
				operation, soft, observed, compare, operand);
			if (reference.compare_exchange_weak(expected, desired,
				success_order, failure_order)) {
				nzcv = flags;
				key = host_reservation_key(pointer);
				return true;
			}
		}
	}

	std::array<std::byte, 8> bytes{};
	if (!memory.read(address, bytes))
		return false;
	observed = arm64::detail::decode_little_endian(bytes);
	const auto [desired, flags] = rcw_result(
		operation, soft, observed, compare, operand);
	if (release)
		std::atomic_thread_fence(std::memory_order_release);
	arm64::detail::encode_little_endian(desired, bytes);
	/* Choosing the permitted write-back-on-failed-check behavior makes the
	 * access permission and reservation effects deterministic in the slow
	 * path, even when the comparison or RCWS check fails.
	 */
	if (!memory.write(address, bytes))
		return false;
	if (acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	nzcv = flags;
	key = reservation_key(memory, address, 8, MemoryAccess::write);
	return true;
}

struct RcwPairResult final {
	std::uint64_t desired_first;
	std::uint64_t desired_second;
	std::uint32_t nzcv;
};

[[nodiscard]] constexpr RcwPairResult rcw_pair_result(unsigned operation,
	bool soft, std::uint64_t observed_first, std::uint64_t observed_second,
	std::uint64_t compare_first, std::uint64_t compare_second,
	std::uint64_t operand_first, std::uint64_t operand_second) noexcept
{
	std::uint64_t desired_first;
	std::uint64_t desired_second;
	if (operation == 0U) {
		if (observed_first != compare_first ||
		    observed_second != compare_second)
			return {observed_first, observed_second, 0xa000'0000U};
		desired_first = operand_first;
		desired_second = operand_second;
	} else if (operation == 1U) {
		desired_first = observed_first & ~operand_first;
		desired_second = observed_second & ~operand_second;
	} else if (operation == 2U) {
		desired_first = operand_first;
		desired_second = operand_second;
	} else {
		desired_first = observed_first | operand_first;
		desired_second = observed_second | operand_second;
	}
	/* RCWSMASK_EL1 is maximally permissive and translation-table protection
	 * is disabled in the userspace executor. For 128-bit descriptors the
	 * remaining software state check is the low bit of the first little-endian
	 * doubleword. */
	if (soft && ((observed_first ^ desired_first) & 1U) != 0)
		return {observed_first, observed_second, 0U};
	return {desired_first, desired_second, 0x2000'0000U};
}

[[nodiscard]] bool atomic_rcw_pair_value(GuestMemory &memory,
	std::uint64_t address, unsigned operation, bool soft,
	std::uint64_t compare_first, std::uint64_t compare_second,
	std::uint64_t operand_first, std::uint64_t operand_second, bool acquire,
	bool release, std::uint64_t &observed_first,
	std::uint64_t &observed_second, std::uint32_t &nzcv,
	std::uintptr_t &key)
{
	constexpr std::size_t size = 16;
	auto *pointer = memory.atomic_pointer(address, size, MemoryAccess::write);
	if (pointer != nullptr &&
	    reinterpret_cast<std::uintptr_t>(pointer) % size == 0) {
		key = host_reservation_key(pointer);
		atomic_load_pair_128_at(pointer, observed_first, observed_second);
		for (;;) {
			const auto outcome = rcw_pair_result(operation, soft,
				observed_first, observed_second, compare_first,
				compare_second, operand_first, operand_second);
			std::uint64_t actual_first;
			std::uint64_t actual_second;
			atomic_compare_swap_pair_128_at(pointer, observed_first,
				observed_second, outcome.desired_first,
				outcome.desired_second, actual_first, actual_second);
			if (actual_first == observed_first &&
			    actual_second == observed_second) {
				nzcv = outcome.nzcv;
				return true;
			}
			observed_first = actual_first;
			observed_second = actual_second;
		}
	}

	std::array<std::byte, size> bytes{};
	if (!memory.read(address, bytes))
		return false;
	observed_first = arm64::detail::decode_little_endian(
		std::span{bytes}.first<8>());
	observed_second = arm64::detail::decode_little_endian(
		std::span{bytes}.subspan<8, 8>());
	const auto outcome = rcw_pair_result(operation, soft, observed_first,
		observed_second, compare_first, compare_second, operand_first,
		operand_second);
	if (release)
		std::atomic_thread_fence(std::memory_order_release);
	arm64::detail::encode_little_endian(outcome.desired_first,
		std::span{bytes}.first<8>());
	arm64::detail::encode_little_endian(outcome.desired_second,
		std::span{bytes}.subspan<8, 8>());
	/* Use the permitted write-back-on-failed-check behavior for the same
	 * deterministic access-permission and reservation semantics as scalar RCW. */
	if (!memory.write(address, bytes))
		return false;
	if (acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	nzcv = outcome.nzcv;
	key = reservation_key(memory, address, size, MemoryAccess::write);
	return true;
}

[[nodiscard]] bool atomic_rmw_pair_128_value(GuestMemory &memory,
	std::uint64_t address, unsigned operation, std::uint64_t operand_first,
	std::uint64_t operand_second, bool acquire, bool release,
	std::uint64_t &observed_first, std::uint64_t &observed_second,
	std::uintptr_t &key)
{
	constexpr std::size_t size = 16;
	std::array<std::byte, size> bytes{};
	auto *pointer = memory.atomic_pointer(address, size, MemoryAccess::write);
	if (pointer != nullptr &&
	    reinterpret_cast<std::uintptr_t>(pointer) % size == 0) {
		key = host_reservation_key(pointer);
		atomic_load_pair_128_at(pointer, observed_first, observed_second);
		for (;;) {
			std::uint64_t desired_first;
			std::uint64_t desired_second;
			switch (operation) {
			case 1:
				desired_first = observed_first & ~operand_first;
				desired_second = observed_second & ~operand_second;
				break;
			case 3:
				desired_first = observed_first | operand_first;
				desired_second = observed_second | operand_second;
				break;
			case 8:
				desired_first = operand_first;
				desired_second = operand_second;
				break;
			default:
				return false;
			}
			std::uint64_t actual_first;
			std::uint64_t actual_second;
			atomic_compare_swap_pair_128_at(pointer, observed_first,
				observed_second, desired_first, desired_second,
				actual_first, actual_second);
			if (actual_first == observed_first &&
			    actual_second == observed_second)
				return true;
			observed_first = actual_first;
			observed_second = actual_second;
		}
	} else {
		if (!memory.read(address, bytes))
			return false;
		key = reservation_key(memory, address, size, MemoryAccess::write);
	}

	observed_first = arm64::detail::decode_little_endian(
		std::span{bytes}.first<8>());
	observed_second = arm64::detail::decode_little_endian(
		std::span{bytes}.subspan<8, 8>());
	std::uint64_t desired_first;
	std::uint64_t desired_second;
	switch (operation) {
	case 1:
		desired_first = observed_first & ~operand_first;
		desired_second = observed_second & ~operand_second;
		break;
	case 3:
		desired_first = observed_first | operand_first;
		desired_second = observed_second | operand_second;
		break;
	case 8:
		desired_first = operand_first;
		desired_second = operand_second;
		break;
	default:
		return false;
	}
	if (release)
		std::atomic_thread_fence(std::memory_order_release);
	arm64::detail::encode_little_endian(desired_first,
		std::span{bytes}.first<8>());
	arm64::detail::encode_little_endian(desired_second,
		std::span{bytes}.subspan<8, 8>());
	if (!memory.write(address, bytes))
		return false;
	if (acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	return true;
}

[[nodiscard]] bool atomic_load_pair_value(GuestMemory &memory,
	std::uint64_t address, std::size_t element_size, std::memory_order order,
	std::uint64_t &first, std::uint64_t &second, std::uintptr_t &key)
{
	const auto total_size = element_size * 2;
	std::array<std::byte, 16> bytes{};
	auto *pointer = memory.atomic_pointer(
		address, total_size, MemoryAccess::read);
	if (pointer != nullptr &&
	    reinterpret_cast<std::uintptr_t>(pointer) % element_size == 0) {
		key = host_reservation_key(pointer);
		if (total_size == 16 &&
		    reinterpret_cast<std::uintptr_t>(pointer) % 16 == 0) {
			/* A pair marked single-copy atomic must not inherit memcpy's
			 * tearing behavior in the slow path. LDXP supplies one atomic
			 * 128-bit snapshot on the AArch64 host; CLREX prevents this internal
			 * implementation detail from retaining a reservation. */
			atomic_load_pair_128_at(pointer, first, second);
			if (order != std::memory_order_relaxed)
				std::atomic_thread_fence(order);
			return true;
		}
		std::copy_n(pointer, total_size, bytes.begin());
	} else {
		if (!memory.read(address, std::span{bytes}.first(total_size)))
			return false;
		key = reservation_key(
			memory, address, total_size, MemoryAccess::read);
	}
	if (order != std::memory_order_relaxed)
		std::atomic_thread_fence(order);
	first = arm64::detail::decode_little_endian(
		std::span{bytes}.first(element_size));
	second = arm64::detail::decode_little_endian(
		std::span{bytes}.subspan(element_size, element_size));
	return true;
}

[[nodiscard]] AtomicStoreResult atomic_store_pair_value(GuestMemory &memory,
	std::uint64_t address, std::size_t element_size, std::uint64_t first,
	std::uint64_t second, std::memory_order order,
	const std::uintptr_t *expected_key, std::uintptr_t &key)
{
	const auto total_size = element_size * 2;
	std::array<std::byte, 16> bytes{};
	arm64::detail::encode_little_endian(first,
		std::span{bytes}.first(element_size));
	arm64::detail::encode_little_endian(second,
		std::span{bytes}.subspan(element_size, element_size));
	auto *pointer = memory.atomic_pointer(
		address, total_size, MemoryAccess::write);
	if (pointer != nullptr &&
	    reinterpret_cast<std::uintptr_t>(pointer) % element_size == 0) {
		key = host_reservation_key(pointer);
		if (expected_key != nullptr && key != *expected_key)
			return AtomicStoreResult::reservation_mismatch;
		if (order != std::memory_order_relaxed)
			std::atomic_thread_fence(order);
		if (total_size == 16 &&
		    reinterpret_cast<std::uintptr_t>(pointer) % 16 == 0) {
			/* CASP gives the slow path a true 128-bit atomic store on
			 * hosts without FEAT_LSE128. The speculative LDP only seeds the
			 * comparison; CASP validates it before committing. */
			__asm__ volatile(
				"mov x9, %0\n"
				"mov x12, %1\n"
				"mov x13, %2\n"
				"ldp x14, x15, [x9]\n"
				"1:\n"
				"mov x10, x14\n"
				"mov x11, x15\n"
				"casp x14, x15, x12, x13, [x9]\n"
				"cmp x14, x10\n"
				"ccmp x15, x11, #0, eq\n"
				"b.ne 1b"
				:
				: "r"(pointer), "r"(first), "r"(second)
				: "x9", "x10", "x11", "x12", "x13", "x14", "x15",
				  "cc", "memory");
		} else {
			std::copy_n(bytes.begin(), total_size, pointer);
		}
		return AtomicStoreResult::stored;
	}

	key = reservation_key(
		memory, address, total_size, MemoryAccess::write);
	if (expected_key != nullptr && key != *expected_key)
		return AtomicStoreResult::reservation_mismatch;
	if (order != std::memory_order_relaxed)
		std::atomic_thread_fence(order);
	return memory.write(address, std::span{bytes}.first(total_size)) ?
		AtomicStoreResult::stored : AtomicStoreResult::fault;
}

void invalidate_reservations_for_store_locked(GuestMemory &memory,
	std::uint64_t address, std::size_t size)
{
	/* Every successful ordinary store invalidates every reservation granule it
	 * touches, even when the stored bits compare equal to the old value. Keep
	 * direct host aliases and fallback guest-address reservations in the same
	 * namespaces used by the exclusive helpers above.
	 */
	for (std::size_t offset = 0; offset < size;) {
		const auto current = address + offset;
		auto *pointer = memory.reservation_pointer(
			current, 1, MemoryAccess::write);
		reservation_store(pointer != nullptr ? host_reservation_key(pointer) :
			fallback_reservation_key(current));
		const auto remaining_in_granule = reservation_granule -
			(static_cast<std::uintptr_t>(current) &
			 (reservation_granule - 1));
		offset += std::min<std::size_t>(
			size - offset, remaining_in_granule);
	}
}

[[nodiscard]] bool ordinary_store(GuestMemory &memory, std::uint64_t address,
	std::span<const std::byte> bytes)
{
	/* Serialize the write itself with the exclusive operations. Doing
	 * only the version update under the lock leaves a window in which STXR can
	 * incorrectly succeed after a conflicting ordinary store has committed.
	 */
	std::scoped_lock lock{exclusive_memory_mutex};
	if (!memory.write(address, bytes))
		return false;
	invalidate_reservations_for_store_locked(memory, address, bytes.size());
	return true;
}

[[nodiscard]] bool simd_structure_address(CpuState &cpu,
	GuestMemory &memory, std::uint64_t tagged_address, bool base_sp,
	bool tagchecked, std::size_t transfer_size, std::size_t element_size,
	MemoryAccess access, std::uint64_t &address, std::uint64_t &fault_address)
{
	const bool tagged = !base_sp && cpu.tagged_addresses_enabled();
	address = tagged ?
		tagged_address & 0x00ff'ffff'ffff'ffffULL : tagged_address;
	const bool check_tags =
		tagchecked && cpu.mte_enabled && !cpu.pstate_tco;
	const auto logical_tag = !base_sp && cpu.mte_enabled ?
		static_cast<std::uint8_t>(tagged_address >> 56) & 15U : 0U;
	for (std::size_t completed = 0; completed < transfer_size;
	     completed += element_size) {
		const auto current = address + completed;
		const auto access_size =
			std::min(element_size, transfer_size - completed);
		if (!memory.validate_access(current, access_size, access)) {
			fault_address = memory.last_fault().address;
			return false;
		}
		if (!check_tags)
			continue;
		for (std::size_t checked = 0; checked < access_size;) {
			const auto tagged_current = current + checked;
			std::uint8_t allocation_tag{};
			if (!memory.read_allocation_tag(
				    tagged_current, allocation_tag) ||
			    allocation_tag != logical_tag) {
				fault_address = tagged_current;
				return false;
			}
			checked += std::min<std::size_t>(
				access_size - checked,
				16U - (tagged_current & 15U));
		}
	}
	return true;
}

void invalidate_store_ranges(std::span<const JitStoreRange> ranges) noexcept
{
	for (const auto &range : ranges) {
		for (std::uint64_t offset = 0; offset < range.size;) {
			const auto current = range.address + offset;
			reservation_store(host_reservation_key(
				reinterpret_cast<std::byte *>(current)));
			const auto remaining_in_granule = reservation_granule -
				(static_cast<std::uintptr_t>(current) &
				 (reservation_granule - 1));
			offset += std::min<std::uint64_t>(
				range.size - offset, remaining_in_granule);
		}
	}
}

} // namespace uml::cpu::arm64::jit::semantic
