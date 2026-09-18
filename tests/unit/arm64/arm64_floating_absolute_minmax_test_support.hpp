// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_floating_horizontal_test_support.hpp"

namespace arm64_floating_absolute_minmax_test {

namespace horizontal = arm64_floating_horizontal_test;
namespace basic = horizontal::basic;
namespace common = horizontal::common;
namespace arm64 = horizontal::arm64;
namespace engine_tests = horizontal::engine_tests;

using Operation = horizontal::Operation;
using Registers = horizontal::Registers;
inline constexpr auto register_cases = horizontal::register_cases;

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

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	constexpr std::array maximum{0x0ec0'1c00U, 0x4ec0'1c00U,
		0x0ea0'dc00U, 0x4ea0'dc00U, 0x4ee0'dc00U};
	constexpr std::array minimum{0x2ec0'1c00U, 0x6ec0'1c00U,
		0x2ea0'dc00U, 0x6ea0'dc00U, 0x6ee0'dc00U};
	const auto &table = form.operation == Operation::absolute_maximum ?
		maximum : minimum;
	return table[horizontal::vector_shape(
		form.width, form.vector_bytes)] |
		(registers.destination & 31U) |
		((registers.first & 31U) << 5) |
		((registers.second & 31U) << 16);
}

} // namespace arm64_floating_absolute_minmax_test
