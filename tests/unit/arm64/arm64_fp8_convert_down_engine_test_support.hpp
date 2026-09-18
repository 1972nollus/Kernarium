// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_fp8_convert_down_engine_test {

namespace arm64 = uml::cpu::arm64;

struct Form final {
	unsigned source_width;
	bool upper;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

[[nodiscard]] constexpr std::uint32_t instruction(
	const Form &form, Registers registers = {0U, 1U, 2U}) noexcept
{
	return (form.source_width == 16U ?
			0x0e40'f400U : 0x0e00'f400U) |
		(form.upper ? 0x4000'0000U : 0U) |
		((registers.second & 31U) << 16) |
		((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

} // namespace arm64_fp8_convert_down_engine_test
