// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_base_move_wide_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

[[nodiscard]] constexpr std::uint32_t instruction(bool wide,
	unsigned operation, unsigned halfword, unsigned immediate,
	unsigned destination = 7U)
{
	return 0x1280'0000U | (wide ? 0x8000'0000U : 0U) |
		((operation & 3U) << 29) | ((halfword & 3U) << 21) |
		((immediate & 0xffffU) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint64_t result(bool wide,
	unsigned operation, unsigned halfword, unsigned immediate,
	std::uint64_t old_value)
{
	const unsigned shift = halfword * 16U;
	const auto field = static_cast<std::uint64_t>(immediate) << shift;
	std::uint64_t value;
	if (operation == 0U)
		value = ~field;
	else if (operation == 2U)
		value = field;
	else
		value = (old_value & ~(std::uint64_t{0xffffU} << shift)) |
			field;
	return wide ? value : value & 0xffff'ffffU;
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < 31U; ++index)
		cpu.registers[index] =
			0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(seed + index * 11U) *
				0x0101'0101'0101'0101ULL);
	for (unsigned reg = 0; reg < 32U; ++reg)
		for (unsigned byte = 0; byte < 16U; ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 13U + byte * 31U);
	cpu.sp = 0xfeed'face'feed'faceULL ^ seed;
	cpu.nzcv = ((seed ^ 9U) & 15U) << 28;
	cpu.fpsr = 0x0800'0090U;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.exclusive_address = 0x5100'00c0ULL;
	cpu.exclusive_key = 0x9753U;
	cpu.exclusive_value = 0x1122'3344'5566'7788ULL;
	cpu.exclusive_value2 = 0x8877'6655'4433'2211ULL;
	cpu.exclusive_version = 0x1020'3040'5060'7080ULL;
	cpu.exclusive_size = 4U;
	return cpu;
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, bool wide, unsigned operation,
	unsigned halfword, unsigned immediate, unsigned destination)
{
	auto expected = initial;
	if (destination != 31U)
		expected.registers[destination] = result(wide, operation,
			halfword, immediate, initial.registers[destination]);
	return expected;
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, bool wide, unsigned operation,
	unsigned halfword, unsigned immediate, unsigned destination,
	const arm64::CpuState &initial)
{
	harness.check_one(name, instruction(wide, operation, halfword,
		immediate, destination), initial,
		expected_state(initial, wide, operation, halfword, immediate,
			destination),
		engine_tests::NativeRequirements::baseline());
}

} // namespace arm64_base_move_wide_test
