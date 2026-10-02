// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstdint>

namespace uml::emulator::tests::base_arithmetic {

namespace arm64 = uml::cpu::arm64;

enum class ShiftOperation : unsigned {
	left = 0,
	right = 1,
	arithmetic_right = 2,
	rotate_right = 3,
};

[[nodiscard]] constexpr std::uint32_t encode_carry(
	bool wide, bool subtract, bool set_flags, unsigned destination,
	unsigned left, unsigned right) noexcept
{
	return 0x1a00'0000U | (wide ? 0x8000'0000U : 0U) |
		(subtract ? 0x4000'0000U : 0U) |
		(set_flags ? 0x2000'0000U : 0U) |
		((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_divide(
	bool wide, bool signed_divide, unsigned destination, unsigned dividend,
	unsigned divisor) noexcept
{
	return 0x1ac0'0800U | (wide ? 0x8000'0000U : 0U) |
		(signed_divide ? 0x400U : 0U) |
		((divisor & 31U) << 16) | ((dividend & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_variable_shift(
	bool wide, ShiftOperation operation, unsigned destination,
	unsigned value, unsigned shift) noexcept
{
	return 0x1ac0'2000U | (wide ? 0x8000'0000U : 0U) |
		(static_cast<unsigned>(operation) << 10) |
		((shift & 31U) << 16) | ((value & 31U) << 5) |
		(destination & 31U);
}

} // namespace uml::emulator::tests::base_arithmetic
