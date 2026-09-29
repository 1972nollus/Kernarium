// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_runtime_contract.hpp"

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/detail/atomic_floating.hpp"
#include "cpu/arm64/memory.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

/*
 * The JIT's software reservation state. A versioned table of host
 * reservation granules backs the guest exclusive monitor. The semantic slow
 * path performs each atomic, exclusive and ordered access as one host access
 * on the translated page (GuestMemory::atomic_pointer), serializes its
 * reservation checks and stores with exclusive_memory_mutex, and invalidates
 * every granule it stores to. Tracked native store blocks invalidate the
 * granules they wrote before their publication ends.
 */

namespace uml::cpu::arm64::jit::semantic {

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

extern ExclusiveMemoryMutex exclusive_memory_mutex;

enum class AtomicStoreResult {
	stored,
	reservation_mismatch,
	fault,
};

void reservation_store(std::uintptr_t key) noexcept;
[[nodiscard]] std::uint64_t reservation_version(std::uintptr_t key) noexcept;

[[nodiscard]] bool atomic_load_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, std::memory_order order,
	std::uint64_t &value, std::uintptr_t &key);

[[nodiscard]] AtomicStoreResult atomic_store_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, std::uint64_t value,
	std::memory_order order, const std::uintptr_t *expected_key,
	std::uintptr_t &key);

[[nodiscard]] bool atomic_compare_swap_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, std::uint64_t compare,
	std::uint64_t replacement, bool acquire, bool release,
	std::uint64_t &observed, std::uintptr_t &key);

[[nodiscard]] bool atomic_compare_swap_pair_value(GuestMemory &memory,
	std::uint64_t address, std::size_t element_size,
	std::uint64_t compare_first, std::uint64_t compare_second,
	std::uint64_t replacement_first, std::uint64_t replacement_second,
	bool acquire, bool release,
	std::uint64_t &observed_first, std::uint64_t &observed_second,
	std::uintptr_t &key);

[[nodiscard]] bool atomic_floating_rmw_value(GuestMemory &memory,
	std::uint64_t address, const arm64::detail::AtomicFloating &decoded,
	std::uint64_t operand, std::uint32_t fpcr,
	std::uint64_t &observed, std::uintptr_t &key);

[[nodiscard]] bool atomic_rmw_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, unsigned operation,
	std::uint64_t operand, bool acquire, bool release,
	std::uint64_t &observed, std::uintptr_t &key);

[[nodiscard]] bool atomic_rcw_value(GuestMemory &memory,
	std::uint64_t address, unsigned operation, bool soft,
	std::uint64_t compare, std::uint64_t operand, bool acquire, bool release,
	std::uint64_t &observed, std::uint32_t &nzcv, std::uintptr_t &key);

[[nodiscard]] bool atomic_rcw_pair_value(GuestMemory &memory,
	std::uint64_t address, unsigned operation, bool soft,
	std::uint64_t compare_first, std::uint64_t compare_second,
	std::uint64_t operand_first, std::uint64_t operand_second, bool acquire,
	bool release, std::uint64_t &observed_first,
	std::uint64_t &observed_second, std::uint32_t &nzcv,
	std::uintptr_t &key);

[[nodiscard]] bool atomic_rmw_pair_128_value(GuestMemory &memory,
	std::uint64_t address, unsigned operation, std::uint64_t operand_first,
	std::uint64_t operand_second, bool acquire, bool release,
	std::uint64_t &observed_first, std::uint64_t &observed_second,
	std::uintptr_t &key);

[[nodiscard]] bool atomic_load_pair_value(GuestMemory &memory,
	std::uint64_t address, std::size_t element_size, std::memory_order order,
	std::uint64_t &first, std::uint64_t &second, std::uintptr_t &key);

[[nodiscard]] AtomicStoreResult atomic_store_pair_value(GuestMemory &memory,
	std::uint64_t address, std::size_t element_size, std::uint64_t first,
	std::uint64_t second, std::memory_order order,
	const std::uintptr_t *expected_key, std::uintptr_t &key);

[[nodiscard]] bool ordinary_store(GuestMemory &memory, std::uint64_t address,
	std::span<const std::byte> bytes);

[[nodiscard]] bool simd_structure_address(CpuState &cpu,
	GuestMemory &memory, std::uint64_t tagged_address, bool base_sp,
	std::size_t transfer_size, std::size_t element_size,
	MemoryAccess access, std::uint64_t &address, std::uint64_t &fault_address);

/* Invalidates every reservation granule that a tracked native store block
 * recorded. Each range names the translated host backing address, so guest
 * aliases of one location share its reservation identity. */
void invalidate_store_ranges(std::span<const JitStoreRange> ranges) noexcept;

} // namespace uml::cpu::arm64::jit::semantic
