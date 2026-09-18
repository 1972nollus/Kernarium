// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_bfloat_dot_test_support.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace arm64_bfloat_matrix_test {

namespace dot = arm64_bfloat_dot_test;
namespace shared = dot::shared;
namespace arm64 = shared::arm64;
namespace engine_tests = shared::engine_tests;

using Registers = dot::Registers;

[[nodiscard]] constexpr std::uint32_t instruction(
	Registers registers = {0U, 1U, 2U}) noexcept
{
	return 0x6e40'ec00U |
		((registers.second & 31U) << 16U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

} // namespace arm64_bfloat_matrix_test
