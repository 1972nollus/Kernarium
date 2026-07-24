// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_test_support.hpp"
#include "cpu/arm64/detail/half_matrix.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace arm64_half_matrix_test {

namespace common = arm64_simd_test;

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array register_cases{
	Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
	Registers{31, 30, 29}, Registers{30, 30, 30}};

[[nodiscard]] constexpr std::uint32_t instruction(bool widening,
	Registers registers = {0, 1, 2}) noexcept
{
	const std::uint32_t base = widening ? 0x4e40'ec00U : 0x4ec0'ec00U;
	return base | ((registers.second & 31U) << 16) |
		((registers.first & 31U) << 5) | (registers.destination & 31U);
}

[[nodiscard]] inline std::uint16_t half(float value) noexcept
{
	return std::bit_cast<std::uint16_t>(static_cast<_Float16>(value));
}

inline void set_half(common::arm64::CpuState &state, unsigned reg,
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

[[nodiscard]] inline std::uint16_t half_lane(
	const common::arm64::CpuState &state, unsigned reg, unsigned lane)
{
	std::array<std::byte, 2> bytes{};
	std::copy_n(state.vectors[reg].bytes.begin() + lane * 2U, 2U,
		bytes.begin());
	return std::bit_cast<std::uint16_t>(bytes);
}

[[nodiscard]] inline std::uint32_t single_lane(
	const common::arm64::CpuState &state, unsigned reg, unsigned lane)
{
	std::array<std::byte, 4> bytes{};
	std::copy_n(state.vectors[reg].bytes.begin() + lane * 4U, 4U,
		bytes.begin());
	return std::bit_cast<std::uint32_t>(bytes);
}

inline void clear_vectors(common::arm64::CpuState &state)
{
	for (auto &vector : state.vectors)
		vector.bytes.fill(std::byte{});
}

} // namespace arm64_half_matrix_test
