// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_simd_rearrange_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct PermuteRegisters final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

struct TableRegisters final {
	unsigned destination;
	unsigned first;
	unsigned indices;
};

[[nodiscard]] constexpr std::uint32_t permute_instruction(bool q,
	unsigned size, unsigned opcode, PermuteRegisters registers)
{
	return 0x0e00'0800U | (q ? 0x4000'0000U : 0U) |
		((size & 3U) << 22) | ((registers.right & 31U) << 16) |
		((opcode & 7U) << 12) | ((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t table_instruction(bool q, bool extend,
	unsigned count, TableRegisters registers)
{
	return 0x0e00'0000U | (q ? 0x4000'0000U : 0U) |
		((registers.indices & 31U) << 16) |
		(((count - 1U) & 3U) << 13) | (extend ? 0x1000U : 0U) |
		((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

} // namespace arm64_simd_rearrange_test
