// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace arm64_atomic_engine_test {

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;

using Page = std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE>;

inline constexpr std::uint64_t data_page = engine::engine_test_data_base;
inline constexpr std::size_t data_offset = 0x100;
inline constexpr std::uint64_t data_address = data_page + data_offset;
inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

inline void put(Page &page, std::size_t offset, std::uint64_t value,
	std::size_t size)
{
	for (std::size_t byte = 0; byte < size; ++byte)
		page[offset + byte] = static_cast<std::byte>(value >> (byte * 8U));
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(seed + reg + 1U) *
			 0x0101'0101'0101'0101ULL);
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed * 47U + reg * 29U + byte * 13U + 5U);
	cpu.sp = data_address;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = seed & 1U;
	cpu.fpmr = 0x1234'5678'9abc'def0ULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	return cpu;
}

[[nodiscard]] inline std::uint64_t read_register(const arm64::CpuState &cpu,
	unsigned reg, bool stack_pointer = false)
{
	return reg == 31U ? (stack_pointer ? cpu.sp : 0U) :
		cpu.registers[reg];
}

inline void write_register(arm64::CpuState &cpu, unsigned reg,
	std::uint64_t value, bool stack_pointer = false)
{
	if (reg == 31U) {
		if (stack_pointer)
			cpu.sp = value;
		return;
	}
	cpu.registers[reg] = value;
}

[[nodiscard]] constexpr std::uint32_t compare_swap(unsigned size_code,
	bool acquire, bool release, unsigned rn, unsigned compare,
	unsigned replacement, unsigned rt2 = 31U)
{
	return 0x08a0'0000U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(acquire) << 22) |
		((compare & 31U) << 16) |
		(static_cast<unsigned>(release) << 15) |
		((rt2 & 31U) << 10) | ((rn & 31U) << 5) |
		(replacement & 31U);
}

[[nodiscard]] constexpr std::uint32_t atomic_raw(unsigned size_code,
	bool acquire, bool release, bool o3, unsigned opcode, unsigned rn,
	unsigned source, unsigned destination)
{
	return 0x3820'0000U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) |
		((source & 31U) << 16) | (static_cast<unsigned>(o3) << 15) |
		((opcode & 7U) << 12) | ((rn & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t atomic(unsigned size_code,
	bool acquire, bool release, unsigned operation, unsigned rn,
	unsigned source, unsigned destination)
{
	return atomic_raw(size_code, acquire, release, operation == 8U,
		operation == 8U ? 0U : operation, rn, source, destination);
}

[[nodiscard]] inline engine::MemoryRegion region(const Page &initial,
	const Page &expected, bool readable = true, bool writable = true)
{
	return {
		.address = data_page,
		.initial = initial,
		.expected = expected,
		.readable = readable,
		.writable = writable,
	};
}

[[nodiscard]] inline engine::NativeRequirements lse128()
{
	return engine::NativeRequirements::features(
		::uml::cpu::ArmFeature::lse128);
}

} // namespace arm64_atomic_engine_test
