// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_test_support.hpp"
#include "cpu/arm64/detail/bfloat_dot.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace arm64_bfloat_dot_test {

namespace common = arm64_simd_test;

struct Form final {
	bool by_element;
	bool wide;
	unsigned index;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array register_cases{
	Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
	Registers{31, 30, 29}, Registers{30, 30, 30}};

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	Registers registers = {0, 1, 2}) noexcept
{
	const std::uint32_t base = form.by_element ? 0x0f40'f000U :
		0x2e40'fc00U;
	const std::uint32_t index = form.by_element ?
		((form.index & 1U) << 21) | ((form.index & 2U) << 10) : 0U;
	return base | (static_cast<std::uint32_t>(form.wide) << 30) | index |
		((registers.second & 31U) << 16) |
		((registers.first & 31U) << 5) | (registers.destination & 31U);
}

inline void set_bfloat(common::arm64::CpuState &state, unsigned reg,
	unsigned lane, std::uint16_t bits)
{
	const auto bytes = std::bit_cast<std::array<std::byte, 2>>(bits);
	std::copy(bytes.begin(), bytes.end(),
		state.vectors[reg].bytes.begin() + lane * 2U);
}

inline void set_single(common::arm64::CpuState &state, unsigned reg,
	unsigned lane, std::uint32_t bits)
{
	const auto bytes = std::bit_cast<std::array<std::byte, 4>>(bits);
	std::copy(bytes.begin(), bytes.end(),
		state.vectors[reg].bytes.begin() + lane * 4U);
}

[[nodiscard]] inline std::uint32_t single_lane(
	const common::arm64::CpuState &state, unsigned reg, unsigned lane)
{
	std::array<std::byte, 4> bytes{};
	std::copy_n(state.vectors[reg].bytes.begin() + lane * 4U, 4U,
		bytes.begin());
	return std::bit_cast<std::uint32_t>(bytes);
}

[[nodiscard]] constexpr std::uint16_t bfloat(float value) noexcept
{
	return static_cast<std::uint16_t>(std::bit_cast<std::uint32_t>(value) >> 16);
}

inline void require_lanes(const common::arm64::CpuState &state,
	unsigned reg, const std::array<std::uint32_t, 4> &expected,
	unsigned lanes)
{
	for (unsigned lane = 0; lane < lanes; ++lane)
		common::require(single_lane(state, reg, lane) == expected[lane]);
	for (unsigned lane = lanes; lane < 4U; ++lane)
		common::require(single_lane(state, reg, lane) == 0U);
}

} // namespace arm64_bfloat_dot_test
