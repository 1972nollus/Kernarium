// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_test_support.hpp"
#include "cpu/arm64/detail/bfloat_multiply.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace arm64_bfloat_multiply_test {

namespace common = arm64_simd_test;

enum class Form : unsigned { vector_bottom, vector_top,
	indexed_bottom, indexed_top };

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array vector_register_cases{
	Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
	Registers{31, 30, 29}, Registers{30, 30, 30}};
inline constexpr std::array indexed_register_cases{
	Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
	Registers{31, 30, 15}, Registers{15, 15, 15}};

[[nodiscard]] constexpr bool top(Form form) noexcept
{
	return form == Form::vector_top || form == Form::indexed_top;
}

[[nodiscard]] constexpr bool by_element(Form form) noexcept
{
	return form == Form::indexed_bottom || form == Form::indexed_top;
}

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	unsigned index = 0U, Registers registers = {0, 1, 2}) noexcept
{
	std::uint32_t encoding;
	if (by_element(form)) {
		encoding = 0x0fc0'f000U | ((index & 3U) << 20) |
			((index & 4U) << 9) | ((registers.second & 15U) << 16);
	} else {
		encoding = 0x2ec0'fc00U | ((registers.second & 31U) << 16);
	}
	return encoding | (static_cast<std::uint32_t>(top(form)) << 30) |
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
	unsigned reg, const std::array<std::uint32_t, 4> &expected)
{
	for (unsigned lane = 0; lane < expected.size(); ++lane)
		common::require(single_lane(state, reg, lane) == expected[lane]);
}

} // namespace arm64_bfloat_multiply_test
