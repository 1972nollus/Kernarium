// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_bfloat_arithmetic_test_support.hpp"
#include "arm64_fp8_convert_up_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_fp8_dot_engine_test {
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Form final {
	unsigned width;
	bool by_element;
	bool wide;
	unsigned index;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array register_aliases{
	Registers{0U, 1U, 2U}, Registers{1U, 1U, 2U},
	Registers{2U, 1U, 2U}, Registers{1U, 2U, 2U},
	Registers{3U, 3U, 3U}, Registers{31U, 30U, 29U},
};

[[nodiscard]] constexpr unsigned index_count(const Form &form) noexcept
{
	return form.width == 16U ? 8U : 4U;
}

[[nodiscard]] constexpr std::uint32_t instruction(
	const Form &form, Registers registers = {0U, 1U, 2U}) noexcept
{
	std::uint32_t result =
		(form.by_element ? 0x0f00'0000U : 0x0e00'fc00U) |
		(form.width == 16U ? 0x0040'0000U : 0U) |
		(form.wide ? 0x4000'0000U : 0U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
	if (!form.by_element)
		return result | ((registers.second & 31U) << 16U);
	if (form.width == 16U) {
		result |= (registers.second & 15U) << 16U;
		result |= ((form.index >> 2U) & 1U) << 11U;
		result |= ((form.index >> 1U) & 1U) << 21U;
		result |= (form.index & 1U) << 20U;
	} else {
		result |= (registers.second & 15U) << 16U;
		result |= ((registers.second >> 4U) & 1U) << 20U;
		result |= ((form.index >> 1U) & 1U) << 11U;
		result |= (form.index & 1U) << 21U;
	}
	return result;
}

} // namespace arm64_fp8_dot_engine_test
