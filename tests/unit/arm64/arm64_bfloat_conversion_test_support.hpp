// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_floating_precision_conversion_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_bfloat_conversion_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Form : unsigned { scalar, lower, upper };

struct Registers final {
	unsigned destination;
	unsigned source;
};

inline constexpr std::array forms{Form::scalar, Form::lower, Form::upper};

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	Registers registers = {0U, 1U}) noexcept
{
	const std::uint32_t base = form == Form::scalar ? 0x1e63'4000U :
		form == Form::upper ? 0x4ea1'6800U : 0x0ea1'6800U;
	return base | (registers.destination & 31U) |
		((registers.source & 31U) << 5);
}

} // namespace arm64_bfloat_conversion_test
