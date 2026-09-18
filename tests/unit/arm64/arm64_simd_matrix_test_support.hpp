// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_dot_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_simd_matrix_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;
namespace dot = arm64_simd_dot_test;

using Registers = dot::Registers;
using dot::fail;
using dot::require;

enum class Mode {
	signed_signed,
	unsigned_unsigned,
	unsigned_signed,
};

inline constexpr std::array modes{
	Mode::signed_signed,
	Mode::unsigned_unsigned,
	Mode::unsigned_signed,
};

[[nodiscard]] constexpr std::uint32_t raw_instruction(bool q, bool u,
	unsigned size, bool alternate, Registers registers) noexcept
{
	return 0x0e00'a400U |
		(q ? 0x4000'0000U : 0U) |
		(u ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) |
		(alternate ? 0x0000'0800U : 0U) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t instruction(Mode mode,
	Registers registers) noexcept
{
	return raw_instruction(true, mode == Mode::unsigned_unsigned, 2U,
		mode == Mode::unsigned_signed, registers);
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::i8mm);
}

} // namespace arm64_simd_matrix_test
