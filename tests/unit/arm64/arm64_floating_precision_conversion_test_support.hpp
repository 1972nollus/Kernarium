// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_floating_precision_conversion_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : std::uint8_t {
	widen,
	narrow,
	round_odd,
};

struct Form final {
	Operation operation;
	unsigned source_width;
	unsigned destination_width;
	bool upper;
	bool scalar;
};

struct Registers final {
	unsigned destination;
	unsigned source;
};

[[nodiscard]] constexpr std::uint32_t instruction(
	const Form &form, Registers registers = {0U, 1U}) noexcept
{
	const auto fields = ((registers.source & 31U) << 5) |
		(registers.destination & 31U);
	if (form.operation == Operation::round_odd)
		return (form.scalar ? 0x7e61'6800U : 0x2e61'6800U) |
			(!form.scalar && form.upper ? 0x4000'0000U : 0U) | fields;
	auto encoded = form.operation == Operation::widen ?
		0x0e21'7800U : 0x0e21'6800U;
	if ((form.operation == Operation::widen &&
	     form.source_width == 32U) ||
	    (form.operation == Operation::narrow &&
	     form.source_width == 64U))
		encoded |= 0x0040'0000U;
	if (form.upper)
		encoded |= 0x4000'0000U;
	return encoded | fields;
}

} // namespace arm64_floating_precision_conversion_test
