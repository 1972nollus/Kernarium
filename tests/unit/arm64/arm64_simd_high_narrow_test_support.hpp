// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string_view>

namespace arm64_simd_high_narrow_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	add,
	rounded_add,
	subtract,
	rounded_subtract,
};

inline constexpr std::array operations{
	Operation::add,
	Operation::rounded_add,
	Operation::subtract,
	Operation::rounded_subtract,
};

struct Shape final {
	unsigned size;
	bool upper;
};

inline constexpr std::array shapes{
	Shape{0U, false}, Shape{1U, false}, Shape{2U, false},
	Shape{0U, true}, Shape{1U, true}, Shape{2U, true},
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

[[noreturn]] inline void fail(const char *message,
	const std::source_location location = std::source_location::current())
{
	std::fprintf(stderr, "%s at %s:%u\n", message,
		location.file_name(), location.line());
	std::abort();
}

inline void require(bool condition, const char *message = "require failed",
	const std::source_location location = std::source_location::current())
{
	if (!condition)
		fail(message, location);
}

[[nodiscard]] constexpr bool rounding(Operation operation)
{
	return operation == Operation::rounded_add ||
		operation == Operation::rounded_subtract;
}

[[nodiscard]] constexpr bool subtracting(Operation operation)
{
	return operation == Operation::subtract ||
		operation == Operation::rounded_subtract;
}

[[nodiscard]] constexpr std::uint32_t instruction(Operation operation,
	Shape shape, Registers registers)
{
	return 0x0e20'4000U | (shape.upper ? 0x4000'0000U : 0U) |
		(rounding(operation) ? 0x2000'0000U : 0U) |
		((shape.size & 3U) << 22) |
		(subtracting(operation) ? 0x2000U : 0U) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

} // namespace arm64_simd_high_narrow_test
