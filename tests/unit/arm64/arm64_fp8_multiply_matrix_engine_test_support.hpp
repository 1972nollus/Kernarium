// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_fp8_dot_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_fp8_multiply_matrix_engine_test {
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Family : std::uint8_t {
	multiply,
	matrix,
};

struct Form final {
	Family family;
	unsigned width;
	bool by_element;
	unsigned selection;
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

[[nodiscard]] constexpr unsigned selection_count(
	const Form &form) noexcept
{
	return form.width == 16U ? 2U : 4U;
}

[[nodiscard]] constexpr unsigned index_count(const Form &) noexcept
{
	return 16U;
}

[[nodiscard]] constexpr std::uint32_t instruction(
	const Form &form, Registers registers = {0U, 1U, 2U}) noexcept
{
	if (form.family == Family::matrix)
		return (form.width == 16U ? 0x6e00'ec00U : 0x6e80'ec00U) |
			((registers.second & 31U) << 16U) |
			((registers.first & 31U) << 5U) |
			(registers.destination & 31U);

	std::uint32_t result;
	if (form.width == 16U)
		result = (form.by_element ? 0x0fc0'0000U :
			0x0ec0'fc00U) |
			((form.selection & 1U) << 30U);
	else
		result = (form.by_element ? 0x2f00'8000U :
			0x0e00'c400U) |
			((form.selection >> 1U) << 30U) |
			((form.selection & 1U) << 22U);
	result |= ((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
	if (!form.by_element)
		return result | ((registers.second & 31U) << 16U);
	result |= (registers.second & 7U) << 16U;
	result |= (form.index & 1U) << 19U;
	result |= ((form.index >> 1U) & 1U) << 20U;
	result |= ((form.index >> 2U) & 1U) << 21U;
	result |= ((form.index >> 3U) & 1U) << 11U;
	return result;
}

template<typename Function>
void for_each_multiply_form(Function &&function)
{
	for (const unsigned width : {16U, 32U}) {
		const Form prototype{
			Family::multiply, width, false, 0U, 0U};
		for (unsigned selection = 0U;
		     selection < selection_count(prototype); ++selection) {
			function(Form{Family::multiply, width, false,
				selection, 0U});
			for (unsigned index = 0U;
			     index < index_count(prototype); ++index)
				function(Form{Family::multiply, width, true,
					selection, index});
		}
	}
}

template<typename Function>
void for_each_matrix_form(Function &&function)
{
	for (const unsigned width : {16U, 32U})
		function(Form{Family::matrix, width, false, 0U, 0U});
}

} // namespace arm64_fp8_multiply_matrix_engine_test
