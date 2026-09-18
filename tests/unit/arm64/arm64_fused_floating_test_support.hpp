// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_basic_floating_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace arm64_fused_floating_test {

namespace basic = arm64_basic_floating_test;
namespace common = arm64_simd_test;
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Form final {
	bool scalar;
	bool negate;
	bool subtract;
	unsigned width;
	unsigned vector_bytes;
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
	unsigned addend;
};

inline constexpr std::array register_cases{
	Registers{0, 1, 2, 3}, Registers{1, 1, 2, 3},
	Registers{2, 1, 2, 3}, Registers{3, 1, 2, 3},
	Registers{1, 1, 1, 1}, Registers{31, 30, 29, 28},
};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const bool negate : {false, true})
		for (const bool subtract : {false, true})
			for (const auto width : basic::widths)
				function(Form{true, negate, subtract, width, 16U});
	for (const bool subtract : {false, true})
		for (const auto shape : basic::vector_shapes)
			function(Form{false, false, subtract, shape[0], shape[1]});
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2, 3}) noexcept
{
	std::uint32_t value;
	if (form.scalar) {
		value = 0x1f00'0000U |
			(basic::scalar_type(form.width) << 22) |
			(form.negate ? 0x0020'0000U : 0U) |
			(form.subtract ? 0x0000'8000U : 0U) |
			((registers.addend & 31U) << 10);
	} else {
		const bool q = form.vector_bytes == 16U;
		if (form.width == 16U)
			value = 0x0e40'0c00U;
		else
			value = 0x0e20'cc00U |
				(form.width == 64U ? 0x0040'0000U : 0U);
		value |= q ? 0x4000'0000U : 0U;
		value |= form.subtract ? 0x0080'0000U : 0U;
	}
	return value | ((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) | (registers.destination & 31U);
}

} // namespace arm64_fused_floating_test
