// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_test_support.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace arm64_bfloat_conversion_test {

namespace common = arm64_simd_test;

enum class Form : unsigned { scalar, lower, upper };

struct Registers final {
	unsigned destination;
	unsigned source;
};

inline constexpr std::array register_cases{
	Registers{0, 1}, Registers{1, 1}, Registers{31, 30}, Registers{30, 30}};

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	Registers registers = {0, 1}) noexcept
{
	const std::uint32_t base = form == Form::scalar ? 0x1e63'4000U :
		form == Form::upper ? 0x4ea1'6800U : 0x0ea1'6800U;
	return base | (registers.destination & 31U) |
		((registers.source & 31U) << 5);
}

inline void set_single(common::arm64::CpuState &state, unsigned reg,
	unsigned lane, std::uint32_t bits)
{
	auto bytes = std::bit_cast<std::array<std::byte, 4>>(bits);
	std::copy(bytes.begin(), bytes.end(),
		state.vectors[reg].bytes.begin() + lane * 4U);
}

[[nodiscard]] inline std::uint16_t bfloat_lane(
	const common::arm64::CpuState &state, unsigned reg, unsigned lane)
{
	std::array<std::byte, 2> bytes{};
	std::copy_n(state.vectors[reg].bytes.begin() + lane * 2U, 2U,
		bytes.begin());
	return std::bit_cast<std::uint16_t>(bytes);
}

} // namespace arm64_bfloat_conversion_test
