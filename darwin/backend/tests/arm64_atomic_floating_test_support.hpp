// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_atomic_test_support.hpp"
#include "cpu/arm64/detail/atomic_floating.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>

namespace arm64_atomic_floating_test {

namespace atomic = arm64_atomic_test;
namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace detail = uml::backend::arm64::detail;
namespace gadgets = uml::backend::arm64::gadgets;

inline constexpr std::uint64_t pc = 0x4d0'0000;
inline constexpr std::uint64_t interpreter_address = atomic::interpreter_base;

enum class Operation : unsigned {
	add,
	maximum,
	maximum_number,
	minimum,
	minimum_number,
};

inline constexpr std::array operations{
	Operation::add, Operation::maximum, Operation::maximum_number,
	Operation::minimum, Operation::minimum_number};

[[nodiscard]] constexpr unsigned opcode(Operation operation) noexcept
{
	switch (operation) {
	case Operation::add: return 0U;
	case Operation::maximum: return 4U;
	case Operation::maximum_number: return 6U;
	case Operation::minimum: return 5U;
	case Operation::minimum_number: return 7U;
	}
	return 0U;
}

[[nodiscard]] constexpr std::uint32_t instruction_raw(unsigned size,
	bool acquire, bool release, bool store_only, unsigned encoded_opcode,
	unsigned rn, unsigned rs, unsigned rt) noexcept
{
	return 0x3c20'0000U | ((size & 3U) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) | ((rs & 31U) << 16) |
		(static_cast<unsigned>(store_only) << 15) |
		((encoded_opcode & 7U) << 12) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t instruction(unsigned size,
	Operation operation, bool acquire, bool release, bool store_only,
	unsigned rn = 2U, unsigned rs = 3U, unsigned rt = 32U) noexcept
{
	return instruction_raw(size, acquire, release, store_only,
		opcode(operation), rn, rs,
		rt == 32U ? (store_only ? 31U : 4U) : rt);
}

[[nodiscard]] constexpr std::size_t size_bytes(unsigned size) noexcept
{
	return std::size_t{2} << (size == 0U ? 0U : size - 1U);
}

inline void set_vector(arm64::CpuState &cpu, unsigned reg,
	std::uint64_t bits, std::size_t size, std::byte upper = std::byte{0})
{
	cpu.vectors[reg].bytes.fill(upper);
	std::memcpy(cpu.vectors[reg].bytes.data(), &bits, size);
}

[[nodiscard]] inline std::uint64_t vector_bits(const arm64::CpuState &cpu,
	unsigned reg, std::size_t size)
{
	std::uint64_t result{};
	std::memcpy(&result, cpu.vectors[reg].bytes.data(), size);
	return result;
}

[[nodiscard]] inline bool vector_upper_zero(const arm64::CpuState &cpu,
	unsigned reg, std::size_t size)
{
	return std::all_of(cpu.vectors[reg].bytes.begin() + size,
		cpu.vectors[reg].bytes.end(), [](std::byte value) {
			return value == std::byte{};
		});
}

inline arm64::RunResult execute_interpreter(arm64::CpuState &cpu,
	atomic::AtomicMemory &memory, std::uint32_t encoded,
	std::uint64_t observed, std::uint64_t operand, bool direct = true,
	unsigned rn = 2U, unsigned rs = 3U)
{
	const auto size = size_bytes(encoded >> 30);
	memory.direct = direct;
	memory.put(interpreter_address, observed, size);
	cpu.pc = pc;
	if (rn == 31U)
		cpu.sp = interpreter_address;
	else
		cpu.registers[rn] = interpreter_address;
	set_vector(cpu, rs, operand, size, std::byte{0xa5});
	return arm64::Dispatcher{}.execute(cpu, memory, encoded);
}

inline void put_mapping(test::StaticMapping &mapping, std::uint64_t address,
	std::uint64_t bits, std::size_t size)
{
	std::memcpy(mapping.at(address), &bits, size);
}

[[nodiscard]] inline std::uint64_t get_mapping(test::StaticMapping &mapping,
	std::uint64_t address, std::size_t size)
{
	std::uint64_t bits{};
	std::memcpy(&bits, mapping.at(address), size);
	return bits;
}

} // namespace arm64_atomic_floating_test
