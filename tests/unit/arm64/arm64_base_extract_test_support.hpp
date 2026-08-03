// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_base_extract_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

[[nodiscard]] constexpr std::uint64_t width_mask(bool wide)
{
	return wide ? ~std::uint64_t{0} : std::uint64_t{0xffff'ffffU};
}

[[nodiscard]] constexpr std::uint64_t result(bool wide, unsigned shift,
	std::uint64_t high, std::uint64_t low)
{
	const unsigned width = wide ? 64U : 32U;
	const auto mask = width_mask(wide);
	high &= mask;
	low &= mask;
	return shift == 0U ? low :
		(low >> shift) | ((high << (width - shift)) & mask);
}

[[nodiscard]] constexpr std::uint32_t instruction(bool wide,
	unsigned shift, unsigned high = 1U, unsigned low = 2U,
	unsigned destination = 7U, unsigned n_field = 2U,
	unsigned op21 = 0U, bool o0 = false)
{
	const unsigned n = n_field < 2U ? n_field :
		static_cast<unsigned>(wide);
	return 0x1380'0000U | (wide ? 0x8000'0000U : 0U) |
		((op21 & 3U) << 29) | ((n & 1U) << 22) |
		(o0 ? 0x0020'0000U : 0U) | ((low & 31U) << 16) |
		((shift & 63U) << 10) | ((high & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < 31U; ++index)
		cpu.registers[index] =
			0x89ab'cdef'0123'4567ULL ^
			(static_cast<std::uint64_t>(seed + index * 13U) *
				0x1111'1111'1111'1111ULL);
	for (unsigned reg = 0; reg < 32U; ++reg)
		for (unsigned byte = 0; byte < 16U; ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 17U + byte * 29U);
	cpu.sp = 0xfedc'ba98'7654'3210ULL ^ seed;
	cpu.nzcv = ((seed ^ 5U) & 15U) << 28;
	cpu.fpsr = 0x0800'0090U;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0x1357'9bdf'2468'ace0ULL;
	cpu.exclusive_address = 0x5100'0080ULL;
	cpu.exclusive_key = 0x1357U;
	cpu.exclusive_value = 0x1122'3344'5566'7788ULL;
	cpu.exclusive_value2 = 0x8877'6655'4433'2211ULL;
	cpu.exclusive_version = 0x1020'3040'5060'7080ULL;
	cpu.exclusive_size = 16U;
	return cpu;
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, bool wide, unsigned shift,
	unsigned high, unsigned low, unsigned destination)
{
	auto expected = initial;
	const auto high_value = high == 31U ? 0U : initial.registers[high];
	const auto low_value = low == 31U ? 0U : initial.registers[low];
	if (destination != 31U)
		expected.registers[destination] =
			result(wide, shift, high_value, low_value);
	return expected;
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, bool wide, unsigned shift, unsigned high,
	unsigned low, unsigned destination, const arm64::CpuState &initial)
{
	harness.check_one(name,
		instruction(wide, shift, high, low, destination), initial,
		expected_state(initial, wide, shift, high, low, destination),
		engine_tests::NativeRequirements::baseline());
}

} // namespace arm64_base_extract_test
