// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_half_dot_test_support.hpp"
#include "cpu/arm64/detail/half_matrix.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_half_matrix_test {

namespace shared = arm64_half_dot_test;
namespace arm64 = uml::cpu::arm64;

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

[[nodiscard]] constexpr std::uint32_t instruction(bool widening,
	Registers registers = {0U, 1U, 2U}) noexcept
{
	const std::uint32_t base =
		widening ? 0x4e40'ec00U : 0x4ec0'ec00U;
	return base | ((registers.second & 31U) << 16U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

inline void require(bool condition, const char *message)
{
	shared::require(condition, message);
}

} // namespace arm64_half_matrix_test
