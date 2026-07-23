// SPDX-License-Identifier: GPL-2.0-or-later
#include "../dispatcher.hpp"

#include "../detail/atomic_floating.hpp"
#include "../detail/instruction_helpers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

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
	 * pure interpreter favors a compact, race-free reference operation over
	 * weakening the host instruction for each encoding variant. Fixed adjacent
	 * registers satisfy CASP's architectural pair constraints.
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
	if (order != std::memory_order_relaxed)
		std::atomic_thread_fence(std::memory_order_acquire);
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
	if (order != std::memory_order_relaxed)
		std::atomic_thread_fence(std::memory_order_release);
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
	observed = detail::decode_little_endian(std::span{bytes}.first(size));
	if (observed == (compare & (size == 8 ? ~std::uint64_t{0} :
		((std::uint64_t{1} << (size * 8)) - 1)))) {
		if (release)
			std::atomic_thread_fence(std::memory_order_release);
		detail::encode_little_endian(replacement,
			std::span{bytes}.first(size));
		if (!memory.write(address, std::span{bytes}.first(size)))
			return false;
	}
	if (acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	key = fallback_reservation_key(address);
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
		key = fallback_reservation_key(address);
	}
	observed_first = detail::decode_little_endian(
		std::span{bytes}.first(element_size));
	observed_second = detail::decode_little_endian(
		std::span{bytes}.subspan(element_size, element_size));
	const auto mask = element_size == 8 ? ~std::uint64_t{0} :
		std::uint64_t{0xffff'ffffU};
	if (observed_first != (compare_first & mask) ||
	    observed_second != (compare_second & mask)) {
		if (acquire)
			std::atomic_thread_fence(std::memory_order_acquire);
		return true;
	}
	if (release)
		std::atomic_thread_fence(std::memory_order_release);
	detail::encode_little_endian(replacement_first,
		std::span{bytes}.first(element_size));
	detail::encode_little_endian(replacement_second,
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
		return detail::sign_extend(observed, static_cast<unsigned>(size * 8)) >=
			detail::sign_extend(operand, static_cast<unsigned>(size * 8)) ?
			observed : operand;
	case 5:
		return detail::sign_extend(observed, static_cast<unsigned>(size * 8)) <=
			detail::sign_extend(operand, static_cast<unsigned>(size * 8)) ?
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
	detail::AtomicFloatingOperation operation, std::uint64_t operand,
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
			detail::atomic_floating_result(operation, observed, operand,
				width, bfloat, fpcr));
		if (reference.compare_exchange_weak(
				expected, desired, order, failure_order))
			return observed;
	}
}

[[nodiscard]] bool atomic_floating_rmw_value(GuestMemory &memory,
	std::uint64_t address, const detail::AtomicFloating &decoded,
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
	observed = detail::decode_little_endian(std::span{bytes}.first(size));
	const auto desired = detail::atomic_floating_result(decoded.operation,
		observed, operand, decoded.width, decoded.bfloat, fpcr);
	if (decoded.release)
		std::atomic_thread_fence(std::memory_order_release);
	detail::encode_little_endian(desired, std::span{bytes}.first(size));
	if (!memory.write(address, std::span{bytes}.first(size)))
		return false;
	if (decoded.acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	key = fallback_reservation_key(address);
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
	observed = detail::decode_little_endian(std::span{bytes}.first(size));
	if (release)
		std::atomic_thread_fence(std::memory_order_release);
	detail::encode_little_endian(
		atomic_operation_result(operation, observed, operand, size),
		std::span{bytes}.first(size));
	if (!memory.write(address, std::span{bytes}.first(size)))
		return false;
	if (acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	key = fallback_reservation_key(address);
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
	observed = detail::decode_little_endian(bytes);
	const auto [desired, flags] = rcw_result(
		operation, soft, observed, compare, operand);
	if (release)
		std::atomic_thread_fence(std::memory_order_release);
	detail::encode_little_endian(desired, bytes);
	/* Choosing the permitted write-back-on-failed-check behavior makes the
	 * access permission and reservation effects deterministic in the strict
	 * interpreter, even when the comparison or RCWS check fails.
	 */
	if (!memory.write(address, bytes))
		return false;
	if (acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	nzcv = flags;
	key = fallback_reservation_key(address);
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
	observed_first = detail::decode_little_endian(
		std::span{bytes}.first<8>());
	observed_second = detail::decode_little_endian(
		std::span{bytes}.subspan<8, 8>());
	const auto outcome = rcw_pair_result(operation, soft, observed_first,
		observed_second, compare_first, compare_second, operand_first,
		operand_second);
	if (release)
		std::atomic_thread_fence(std::memory_order_release);
	detail::encode_little_endian(outcome.desired_first,
		std::span{bytes}.first<8>());
	detail::encode_little_endian(outcome.desired_second,
		std::span{bytes}.subspan<8, 8>());
	/* Use the permitted write-back-on-failed-check behavior for the same
	 * deterministic access-permission and reservation semantics as scalar RCW. */
	if (!memory.write(address, bytes))
		return false;
	if (acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	nzcv = outcome.nzcv;
	key = fallback_reservation_key(address);
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
		key = fallback_reservation_key(address);
	}

	observed_first = detail::decode_little_endian(
		std::span{bytes}.first<8>());
	observed_second = detail::decode_little_endian(
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
	detail::encode_little_endian(desired_first,
		std::span{bytes}.first<8>());
	detail::encode_little_endian(desired_second,
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
			 * tearing behavior in the strict interpreter. LDXP supplies one
			 * atomic 128-bit snapshot on the AArch64 host; CLREX prevents this
			 * internal implementation detail from retaining a reservation. */
			atomic_load_pair_128_at(pointer, first, second);
			if (order != std::memory_order_relaxed)
				std::atomic_thread_fence(order);
			return true;
		}
		std::copy_n(pointer, total_size, bytes.begin());
	} else {
		if (!memory.read(address, std::span{bytes}.first(total_size)))
			return false;
		key = fallback_reservation_key(address);
	}
	if (order != std::memory_order_relaxed)
		std::atomic_thread_fence(order);
	first = detail::decode_little_endian(
		std::span{bytes}.first(element_size));
	second = detail::decode_little_endian(
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
	detail::encode_little_endian(first,
		std::span{bytes}.first(element_size));
	detail::encode_little_endian(second,
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
			/* CASP gives the reference interpreter a true 128-bit atomic
			 * store on hosts without FEAT_LSE128. The speculative LDP only
			 * seeds the comparison; CASP validates it before committing. */
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

	key = fallback_reservation_key(address);
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
		auto *pointer = memory.atomic_pointer(
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
	/* Serialize the write itself with interpreter-exclusive operations. Doing
	 * only the version update under the lock leaves a window in which STXR can
	 * incorrectly succeed after a conflicting ordinary store has committed.
	 */
	std::scoped_lock lock{exclusive_memory_mutex};
	if (!memory.write(address, bytes))
		return false;
	invalidate_reservations_for_store_locked(memory, address, bytes.size());
	return true;
}

} // namespace

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
	const auto address = cpu.read_register(base_register, true);
	if ((address & 63U) != 0)
		return {StopReason::memory_fault, address, instruction, 0, !load};

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
	const auto source = cpu.read_register(transfer, true);
	const auto tag = static_cast<std::uint8_t>(source >> 56) & 15U;
	const std::size_t length = two_granules ? 32 : 16;
	if (zero_data) {
		if ((address & 15U) != 0)
			return fault(address, true);
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
	    (remaining != 0 && (cpu.nzcv & 0x2000'0000U) == 0) ||
	    (remaining != 0 && !copy_general &&
	     (cpu.nzcv & 0x8000'0000U) != 0))
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
	if (writeback && load && base_register == transfer && base_register != 31U)
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
	if ((instruction & 0x0080'0000U) != 0)
		__asm__ volatile("autdzb %0" : "+r"(address));
	else
		__asm__ volatile("autdza %0" : "+r"(address));
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
	if (!vector && operation == 1 && (!load || mode == 0U))
		return {StopReason::illegal_instruction, pc, instruction, 0,
			false};
	const std::size_t size = vector ?
		(operation >= 2 ? 16U : std::size_t{4} << operation) :
		operation >= 2 ? 8U : 4U;
	const auto displacement = sign_extend((instruction >> 15) & 0x7fU, 7) *
		static_cast<std::int64_t>(size);
	const unsigned base_register = (instruction >> 5) & 0x1fU;
	const auto base = cpu.read_register(base_register, true);
	const auto adjusted = static_cast<std::uint64_t>(
		static_cast<std::int64_t>(base) + displacement);
	const auto address = mode == 1 ? base : adjusted;
	bool writeback = mode == 1 || mode == 3;
	const unsigned first = instruction & 0x1fU;
	const unsigned second = (instruction >> 10) & 0x1fU;
	if (load && first == second)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	if (writeback && load && !vector && base_register != 31U &&
	    (base_register == first || base_register == second))
		writeback = false;
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
		if (!ordinary_store(memory, address,
			std::span{bytes}.first(transfer_size)))
			return {StopReason::memory_fault, address, instruction, 0, true};
	}
	if (post_index) {
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
	const unsigned offset_register = (instruction >> 16) & 0x1fU;
	if (!post_index && offset_register != 0U)
		return {StopReason::illegal_instruction, pc, instruction, 0, false};
	const unsigned base_register = (instruction >> 5) & 0x1fU;
	const unsigned first_register = instruction & 0x1fU;
	const auto address = cpu.read_register(base_register, true);
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
		cpu.write_register(base_register, address + offset, true);
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult Dispatcher::load_store_simd_ordered(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction) const
{
	const auto pc = cpu.pc;
	const bool lane_form =
		(instruction & 0xbf3f'fc00U) == 0x0d01'8400U;
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
	const auto address = cpu.read_register((instruction >> 5) & 0x1fU, true);
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
		if ((address & (total_size - 1U)) != 0)
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
	if ((address & (size - 1U)) != 0)
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
	if ((address & (size - 1U)) != 0)
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
	if ((address & (total_size - 1U)) != 0)
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
	if ((address & (size - 1U)) != 0)
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
	const auto address = cpu.read_register(base_register, true);
	if ((address & (size - 1U)) != 0U)
		return {StopReason::memory_fault, address, instruction, 0, true};

	/* Capture the complete source before an aliased destination is written.
	 * Lazy FP materialization also happens only after all decode checks pass.
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
	if ((o3 != 0U && opcode != 0U) ||
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
	if ((address & 7U) != 0)
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

} // namespace uml::backend::arm64
