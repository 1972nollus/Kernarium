// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_load_store_pair_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint64_t data_base =
	engine_tests::engine_test_data_base;

[[nodiscard]] constexpr std::uint32_t pair(
	unsigned operation, bool vector, bool load, unsigned mode,
	unsigned imm7, unsigned rn, unsigned rt, unsigned rt2)
{
	return 0x2800'0000U | ((operation & 3U) << 30) |
		(static_cast<unsigned>(vector) << 26) | ((mode & 3U) << 23) |
		(static_cast<unsigned>(load) << 22) | ((imm7 & 0x7fU) << 15) |
		((rt2 & 31U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::int64_t signed_imm7(unsigned imm7)
{
	return static_cast<std::int64_t>(
		static_cast<std::uint64_t>(imm7 & 0x7fU) << 57) >> 57;
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			seed ^ (0x8000'0000'1000'0000ULL |
				(static_cast<std::uint64_t>(index) << 9) | index);
	cpu.sp = seed ^ 0x1234'5678'9abc'def0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.fpmr = seed ^ 0x0000'0000'0021'0042ULL;
	cpu.tls = seed ^ 0xfedc'ba98'7654'3210ULL;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 29U + byte * 17U);
	return cpu;
}

inline void fill_memory(std::span<std::byte> memory, unsigned salt)
{
	for (std::size_t offset = 0; offset < memory.size(); ++offset)
		memory[offset] = static_cast<std::byte>(
			0x80U | ((offset * 37U + salt * 23U) & 0x7fU));
}

[[nodiscard]] inline std::uint64_t decode_little_endian(
	std::span<const std::byte> memory, std::size_t offset, std::size_t size)
{
	std::uint64_t value = 0;
	for (std::size_t index = 0; index < size; ++index)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(memory[offset + index])) <<
			(index * 8U);
	return value;
}

inline void encode_little_endian(
	std::span<std::byte> memory, std::size_t offset, std::size_t size,
	std::uint64_t value)
{
	for (std::size_t index = 0; index < size; ++index)
		memory[offset + index] =
			static_cast<std::byte>(value >> (index * 8U));
}

inline void check_memory(
	engine_tests::EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial,
	const arm64::CpuState &expected,
	engine_tests::NativeRequirements native,
	std::span<const std::byte> initial_memory,
	std::span<const std::byte> expected_memory = {},
	engine_tests::ExpectedEvent event =
		engine_tests::ExpectedEvent::system_call,
	engine_tests::MemoryFaultExpectation fault = {},
	bool compare_fault = false)
{
	const std::array regions{
		engine_tests::MemoryRegion{
			.address = data_base,
			.initial = initial_memory,
			.expected = expected_memory,
		},
	};
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = expected,
		.event = event,
		.native = native,
		.fault = fault,
		.compare_fault = compare_fault,
		.memory = std::span<const engine_tests::MemoryRegion>{regions},
	});
}

} // namespace arm64_load_store_pair_test
