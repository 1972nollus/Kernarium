// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_floating_precision_conversion_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_simd_fcvt_precision_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Pair final {
	unsigned source_width;
	unsigned destination_width;
};

struct Registers final {
	unsigned destination;
	unsigned source;
};

inline constexpr std::array pairs{
	Pair{16U, 32U}, Pair{16U, 64U}, Pair{32U, 16U},
	Pair{32U, 64U}, Pair{64U, 16U}, Pair{64U, 32U},
};

[[nodiscard]] constexpr unsigned type_for_width(unsigned width) noexcept
{
	return width == 16U ? 3U : width == 32U ? 0U : 1U;
}

[[nodiscard]] constexpr std::uint32_t instruction(Pair pair,
	Registers registers = {28U, 29U}) noexcept
{
	return 0x1e22'4000U |
		(type_for_width(pair.source_width) << 22) |
		(type_for_width(pair.destination_width) << 15) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t instruction_from_types(
	unsigned source_type, unsigned destination_type,
	Registers registers = {28U, 29U}) noexcept
{
	return 0x1e22'4000U | ((source_type & 3U) << 22) |
		((destination_type & 3U) << 15) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr bool valid_type_pair(unsigned source_type,
	unsigned destination_type) noexcept
{
	return source_type != 2U && destination_type != 2U &&
		source_type != destination_type;
}

[[nodiscard]] constexpr bool belongs_to_bfloat_conversion(
	unsigned source_type, unsigned destination_type) noexcept
{
	return source_type == 1U && destination_type == 2U;
}

} // namespace arm64_simd_fcvt_precision_test
