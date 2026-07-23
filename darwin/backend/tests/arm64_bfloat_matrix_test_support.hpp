// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_bfloat_dot_test_support.hpp"
#include "cpu/arm64/detail/bfloat_matrix.hpp"

#include <array>

namespace arm64_bfloat_matrix_test {

namespace common = arm64_simd_test;
namespace dot = arm64_bfloat_dot_test;

using Registers = dot::Registers;
inline constexpr auto register_cases = dot::register_cases;

[[nodiscard]] constexpr std::uint32_t instruction(
	Registers registers = {0, 1, 2}) noexcept
{
	return 0x6e40'ec00U | ((registers.second & 31U) << 16) |
		((registers.first & 31U) << 5) | (registers.destination & 31U);
}

} // namespace arm64_bfloat_matrix_test
