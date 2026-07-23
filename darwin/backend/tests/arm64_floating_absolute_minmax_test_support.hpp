// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_basic_floating_test_support.hpp"
#include "arm64_floating_complex_test_support.hpp"
#include "cpu/arm64/detail/floating_horizontal.hpp"

namespace arm64_floating_absolute_minmax_test {

namespace basic = arm64_basic_floating_test;
namespace common = arm64_simd_test;
namespace complex = arm64_floating_complex_test;
namespace detail = uml::backend::arm64::detail;

using Operation = detail::FloatingHorizontalOperation;
using Registers = complex::Registers;
inline constexpr auto register_cases = complex::register_cases;

struct Form final {
	Operation operation;
	unsigned width;
	unsigned vector_bytes;
};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto operation : {Operation::absolute_maximum,
		Operation::absolute_minimum})
		for (const auto shape : basic::vector_shapes)
			function(Form{operation, shape[0], shape[1]});
}

[[nodiscard]] constexpr unsigned shape(unsigned width,
	unsigned vector_bytes) noexcept
{
	return width == 16U ? (vector_bytes == 16U ? 1U : 0U) :
		width == 32U ? (vector_bytes == 16U ? 3U : 2U) : 4U;
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	constexpr std::array maximum{0x0ec0'1c00U, 0x4ec0'1c00U,
		0x0ea0'dc00U, 0x4ea0'dc00U, 0x4ee0'dc00U};
	constexpr std::array minimum{0x2ec0'1c00U, 0x6ec0'1c00U,
		0x2ea0'dc00U, 0x6ea0'dc00U, 0x6ee0'dc00U};
	const auto &table = form.operation == Operation::absolute_maximum ?
		maximum : minimum;
	return table[shape(form.width, form.vector_bytes)] |
		(registers.destination & 31U) |
		((registers.first & 31U) << 5) |
		((registers.second & 31U) << 16);
}

} // namespace arm64_floating_absolute_minmax_test
