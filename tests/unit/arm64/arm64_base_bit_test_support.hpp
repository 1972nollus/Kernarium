// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstdint>

namespace base_bit_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

[[nodiscard]] constexpr bool allocated(bool wide, unsigned operation) noexcept
{
	if (operation <= 2U)
		return true;
	if (operation == 3U)
		return wide;
	return operation <= 8U;
}

[[nodiscard]] constexpr std::uint32_t instruction(bool wide,
	unsigned operation, unsigned source, unsigned destination) noexcept
{
	return 0x5ac0'0000U | (wide ? 0x8000'0000U : 0U) |
		((operation & 31U) << 10) | ((source & 31U) << 5) |
		(destination & 31U);
}

} // namespace base_bit_test
