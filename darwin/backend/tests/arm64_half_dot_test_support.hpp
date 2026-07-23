// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_half_matrix_test_support.hpp"
#include "cpu/arm64/detail/half_dot.hpp"

namespace arm64_half_dot_test {

namespace common = arm64_simd_test;
namespace matrix = arm64_half_matrix_test;

using Registers = matrix::Registers;
inline constexpr auto register_cases = matrix::register_cases;

struct Form final {
	bool by_element;
	bool wide;
	unsigned index;
};

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	Registers registers = {0, 1, 2}) noexcept
{
	const std::uint32_t base = form.by_element ? 0x0f40'9000U :
		0x0e80'fc00U;
	const std::uint32_t index = form.by_element ?
		((form.index & 1U) << 21) | ((form.index & 2U) << 10) : 0U;
	return base | (static_cast<std::uint32_t>(form.wide) << 30) | index |
		((registers.second & 31U) << 16) |
		((registers.first & 31U) << 5) | (registers.destination & 31U);
}

} // namespace arm64_half_dot_test
