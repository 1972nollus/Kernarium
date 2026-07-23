// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_test_support.hpp"
#include "cpu/arm64/detail/fp8.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace arm64_fp8_test {

namespace common = arm64_simd_test;
namespace arm64 = uml::backend::arm64;
namespace detail = uml::backend::arm64::detail;

struct Registers final {
	unsigned destination{0};
	unsigned first{1};
	unsigned second{2};
};

[[nodiscard]] constexpr std::uint32_t convert_up(bool bfloat, bool source2,
	bool upper, Registers registers = {}) noexcept
{
	const unsigned size = (bfloat ? 2U : 0U) | (source2 ? 1U : 0U);
	return 0x2e21'7800U | (upper ? 0x4000'0000U : 0U) |
		(size << 22) | ((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t convert_down(unsigned source_width,
	bool upper, Registers registers = {}) noexcept
{
	return (source_width == 16U ? 0x0e40'f400U : 0x0e00'f400U) |
		(upper ? 0x4000'0000U : 0U) |
		((registers.second & 31U) << 16) |
		((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t dot(unsigned width, bool indexed,
	bool wide, unsigned index = 0, Registers registers = {}) noexcept
{
	std::uint32_t instruction = (indexed ? 0x0f00'0000U : 0x0e00'fc00U) |
		(width == 16U ? 0x0040'0000U : 0U) |
		(wide ? 0x4000'0000U : 0U) |
		((registers.first & 31U) << 5) |
		(registers.destination & 31U);
	if (!indexed)
		return instruction | ((registers.second & 31U) << 16);
	if (width == 16U) {
		instruction |= (registers.second & 15U) << 16;
		instruction |= ((index >> 2) & 1U) << 11;
		instruction |= ((index >> 1) & 1U) << 21;
		instruction |= (index & 1U) << 20;
	} else {
		instruction |= (registers.second & 15U) << 16;
		instruction |= ((registers.second >> 4) & 1U) << 20;
		instruction |= ((index >> 1) & 1U) << 11;
		instruction |= (index & 1U) << 21;
	}
	return instruction;
}

[[nodiscard]] constexpr std::uint32_t multiply(unsigned width, bool indexed,
	unsigned selection, unsigned index = 0, Registers registers = {}) noexcept
{
	std::uint32_t instruction;
	if (width == 16U)
		instruction = (indexed ? 0x0fc0'0000U : 0x0ec0'fc00U) |
			((selection & 1U) << 30);
	else
		instruction = (indexed ? 0x2f00'8000U : 0x0e00'c400U) |
			((selection >> 1) << 30) | ((selection & 1U) << 22);
	instruction |= ((registers.first & 31U) << 5) |
		(registers.destination & 31U);
	if (!indexed)
		return instruction | ((registers.second & 31U) << 16);
	instruction |= (registers.second & 7U) << 16;
	instruction |= (index & 1U) << 19;
	instruction |= ((index >> 1) & 1U) << 20;
	instruction |= ((index >> 2) & 1U) << 21;
	instruction |= ((index >> 3) & 1U) << 11;
	return instruction;
}

[[nodiscard]] constexpr std::uint32_t matrix(unsigned width,
	Registers registers = {}) noexcept
{
	return (width == 16U ? 0x6e00'ec00U : 0x6e80'ec00U) |
		((registers.second & 31U) << 16) |
		((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint64_t fpmr(unsigned source1 = 0,
	unsigned source2 = 0, unsigned destination = 0, unsigned lscale = 0,
	unsigned nscale = 0, bool osm = false, bool osc = false,
	unsigned lscale2 = 0) noexcept
{
	return (source1 & 7U) | ((source2 & 7U) << 3) |
		((destination & 7U) << 6) | ((lscale & 127U) << 16) |
		(static_cast<std::uint64_t>(nscale & 255U) << 24) |
		(static_cast<std::uint64_t>(lscale2 & 63U) << 32) |
		(static_cast<std::uint64_t>(osm) << 14) |
		(static_cast<std::uint64_t>(osc) << 15);
}

inline void set_byte(arm64::CpuState &cpu, unsigned reg, unsigned lane,
	std::uint8_t value)
{
	cpu.vectors[reg].bytes[lane] = static_cast<std::byte>(value);
}

[[nodiscard]] inline std::uint8_t byte(const arm64::CpuState &cpu,
	unsigned reg, unsigned lane)
{
	return std::to_integer<std::uint8_t>(cpu.vectors[reg].bytes[lane]);
}

inline void set_lane(arm64::CpuState &cpu, unsigned reg, unsigned lane,
	unsigned width, std::uint32_t value)
{
	for (unsigned i = 0; i < width / 8U; ++i)
		cpu.vectors[reg].bytes[lane * width / 8U + i] =
			static_cast<std::byte>(value >> (i * 8U));
}

[[nodiscard]] inline std::uint32_t lane(const arm64::CpuState &cpu,
	unsigned reg, unsigned lane, unsigned width)
{
	std::uint32_t result = 0;
	for (unsigned i = 0; i < width / 8U; ++i)
		result |= static_cast<std::uint32_t>(std::to_integer<unsigned>(
			cpu.vectors[reg].bytes[lane * width / 8U + i])) << (i * 8U);
	return result;
}

inline arm64::CpuState run(std::uint32_t instruction,
	const arm64::CpuState &initial, common::Harness &harness)
{
	const auto interpreted = harness.run_interpreter(instruction, initial);
	harness.require_differential(instruction, initial);
	return interpreted;
}

} // namespace arm64_fp8_test
