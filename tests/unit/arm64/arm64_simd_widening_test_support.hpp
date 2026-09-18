// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string_view>

namespace arm64_simd_widening_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	add_long = 0U,
	add_wide = 1U,
	subtract_long = 2U,
	subtract_wide = 3U,
	absolute_accumulate = 5U,
	absolute_long = 7U,
};

inline constexpr std::array operations{
	Operation::add_long,
	Operation::add_wide,
	Operation::subtract_long,
	Operation::subtract_wide,
	Operation::absolute_accumulate,
	Operation::absolute_long,
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

[[nodiscard]] constexpr unsigned encoding(Operation operation)
{
	return static_cast<unsigned>(operation);
}

[[nodiscard]] constexpr std::uint32_t instruction(Operation operation,
	bool unsigned_values, Shape shape, Registers registers)
{
	return 0x0e20'0000U |
		(shape.upper ? 0x4000'0000U : 0U) |
		(unsigned_values ? 0x2000'0000U : 0U) |
		((shape.size & 3U) << 22) |
		(encoding(operation) << 12) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

/*
 * The scalar three-different allocation shares the operation field, but none
 * of the twelve vector instructions audited by this suite has a scalar form.
 */
[[nodiscard]] constexpr std::uint32_t scalar_instruction(
	Operation operation, bool unsigned_values, unsigned size,
	Registers registers)
{
	return 0x5e20'0000U |
		(unsigned_values ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) |
		(encoding(operation) << 12) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

} // namespace arm64_simd_widening_test
