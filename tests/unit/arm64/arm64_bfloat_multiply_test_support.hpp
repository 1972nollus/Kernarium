// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_bfloat_arithmetic_test_support.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace arm64_bfloat_multiply_test {

namespace shared = arm64_bfloat_arithmetic_test;
namespace arm64 = shared::arm64;
namespace engine_tests = shared::engine_tests;

enum class Form : unsigned {
	vector_bottom,
	vector_top,
	indexed_bottom,
	indexed_top,
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array forms{
	Form::vector_bottom, Form::vector_top,
	Form::indexed_bottom, Form::indexed_top,
};

[[nodiscard]] constexpr bool top(Form form) noexcept
{
	return form == Form::vector_top || form == Form::indexed_top;
}

[[nodiscard]] constexpr bool by_element(Form form) noexcept
{
	return form == Form::indexed_bottom ||
		form == Form::indexed_top;
}

[[nodiscard]] constexpr unsigned index_count(Form form) noexcept
{
	return by_element(form) ? 8U : 1U;
}

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	unsigned index = 0U,
	Registers registers = {0U, 1U, 2U}) noexcept
{
	std::uint32_t encoding;
	if (by_element(form)) {
		encoding = 0x0fc0'f000U |
			((index & 3U) << 20U) |
			((index & 4U) << 9U) |
			((registers.second & 15U) << 16U);
	} else {
		encoding = 0x2ec0'fc00U |
			((registers.second & 31U) << 16U);
	}
	return encoding |
		(static_cast<std::uint32_t>(top(form)) << 30U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

} // namespace arm64_bfloat_multiply_test
