// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>
#include <string_view>

namespace arm64_simd_indexed_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Encoding {
	indexed,
	vector_long,
	vector_rdm,
};

enum class Operation {
	mla,
	mls,
	mul,
	smlal,
	smlsl,
	smull,
	umlal,
	umlsl,
	umull,
	sqdmlal,
	sqdmlsl,
	sqdmull,
	sqdmulh,
	sqrdmulh,
	sqrdmlah,
	sqrdmlsh,
};

struct Family final {
	Operation operation;
	Encoding encoding;
};

struct Shape final {
	unsigned size;
	bool scalar;
	bool q;
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
	unsigned index{};
};

inline constexpr std::array all_families{
	Family{Operation::mla, Encoding::indexed},
	Family{Operation::mls, Encoding::indexed},
	Family{Operation::mul, Encoding::indexed},
	Family{Operation::smlal, Encoding::indexed},
	Family{Operation::smlsl, Encoding::indexed},
	Family{Operation::smull, Encoding::indexed},
	Family{Operation::smlal, Encoding::vector_long},
	Family{Operation::smlsl, Encoding::vector_long},
	Family{Operation::smull, Encoding::vector_long},
	Family{Operation::sqdmlal, Encoding::indexed},
	Family{Operation::sqdmlsl, Encoding::indexed},
	Family{Operation::sqdmlal, Encoding::vector_long},
	Family{Operation::sqdmlsl, Encoding::vector_long},
	Family{Operation::sqdmulh, Encoding::indexed},
	Family{Operation::sqdmull, Encoding::indexed},
	Family{Operation::sqdmull, Encoding::vector_long},
	Family{Operation::sqrdmlah, Encoding::indexed},
	Family{Operation::sqrdmlah, Encoding::vector_rdm},
	Family{Operation::sqrdmlsh, Encoding::indexed},
	Family{Operation::sqrdmlsh, Encoding::vector_rdm},
	Family{Operation::sqrdmulh, Encoding::indexed},
	Family{Operation::umlal, Encoding::indexed},
	Family{Operation::umlal, Encoding::vector_long},
	Family{Operation::umlsl, Encoding::indexed},
	Family{Operation::umlsl, Encoding::vector_long},
	Family{Operation::umull, Encoding::indexed},
	Family{Operation::umull, Encoding::vector_long},
};

inline constexpr std::array indexed_vector_shapes{
	Shape{1U, false, false}, Shape{1U, false, true},
	Shape{2U, false, false}, Shape{2U, false, true},
};

inline constexpr std::array vector_long_shapes{
	Shape{0U, false, false}, Shape{0U, false, true},
	Shape{1U, false, false}, Shape{1U, false, true},
	Shape{2U, false, false}, Shape{2U, false, true},
};

inline constexpr std::array saturating_shapes{
	Shape{1U, false, false}, Shape{1U, false, true},
	Shape{2U, false, false}, Shape{2U, false, true},
	Shape{1U, true, false}, Shape{2U, true, false},
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

[[nodiscard]] constexpr bool is_unsigned(Operation operation) noexcept
{
	return operation == Operation::umlal ||
		operation == Operation::umlsl ||
		operation == Operation::umull;
}

[[nodiscard]] constexpr bool is_same_width(Operation operation) noexcept
{
	return operation == Operation::mla ||
		operation == Operation::mls ||
		operation == Operation::mul;
}

[[nodiscard]] constexpr bool is_normal_long(Operation operation) noexcept
{
	return operation == Operation::smlal ||
		operation == Operation::smlsl ||
		operation == Operation::smull ||
		operation == Operation::umlal ||
		operation == Operation::umlsl ||
		operation == Operation::umull;
}

[[nodiscard]] constexpr bool is_rdm(Operation operation) noexcept
{
	return operation == Operation::sqrdmlah ||
		operation == Operation::sqrdmlsh;
}

[[nodiscard]] constexpr bool subtracts(Operation operation) noexcept
{
	return operation == Operation::mls ||
		operation == Operation::smlsl ||
		operation == Operation::umlsl ||
		operation == Operation::sqdmlsl ||
		operation == Operation::sqrdmlsh;
}

[[nodiscard]] constexpr bool indexed(const Family &family) noexcept
{
	return family.encoding == Encoding::indexed;
}

[[nodiscard]] constexpr unsigned indexed_operation(
	Operation operation) noexcept
{
	switch (operation) {
	case Operation::mla: return 0U;
	case Operation::mls: return 4U;
	case Operation::mul: return 8U;
	case Operation::smlal:
	case Operation::umlal: return 2U;
	case Operation::smlsl:
	case Operation::umlsl: return 6U;
	case Operation::smull:
	case Operation::umull: return 10U;
	case Operation::sqdmlal: return 3U;
	case Operation::sqdmlsl: return 7U;
	case Operation::sqdmull: return 11U;
	case Operation::sqdmulh: return 12U;
	case Operation::sqrdmulh:
	case Operation::sqrdmlah: return 13U;
	case Operation::sqrdmlsh: return 15U;
	}
	return 0U;
}

[[nodiscard]] constexpr bool indexed_unsigned_group(
	Operation operation) noexcept
{
	return operation == Operation::mla ||
		operation == Operation::mls ||
		is_unsigned(operation) ||
		is_rdm(operation);
}

[[nodiscard]] constexpr unsigned vector_long_operation(
	Operation operation) noexcept
{
	switch (operation) {
	case Operation::smlal:
	case Operation::umlal: return 8U;
	case Operation::sqdmlal: return 9U;
	case Operation::smlsl:
	case Operation::umlsl: return 10U;
	case Operation::sqdmlsl: return 11U;
	case Operation::smull:
	case Operation::umull: return 12U;
	case Operation::sqdmull: return 13U;
	default: return 0U;
	}
}

[[nodiscard]] constexpr std::uint32_t instruction(const Family &family,
	Shape shape, Registers registers) noexcept
{
	if (family.encoding == Encoding::vector_rdm)
		return (shape.scalar ? 0x7e00'8400U : 0x2e00'8400U) |
			(!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
			(subtracts(family.operation) ? 0x800U : 0U) |
			((shape.size & 3U) << 22) |
			((registers.right & 31U) << 16) |
			((registers.left & 31U) << 5) |
			(registers.destination & 31U);
	if (family.encoding == Encoding::vector_long)
		return (shape.scalar ? 0x5e20'0000U : 0x0e20'0000U) |
			(!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
			(is_unsigned(family.operation) ? 0x2000'0000U : 0U) |
			((shape.size & 3U) << 22) |
			(vector_long_operation(family.operation) << 12) |
			((registers.right & 31U) << 16) |
			((registers.left & 31U) << 5) |
			(registers.destination & 31U);

	std::uint32_t encoded =
		(shape.scalar ? 0x5f00'0000U : 0x0f00'0000U) |
		(!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
		(indexed_unsigned_group(family.operation) ?
			0x2000'0000U : 0U) |
		((shape.size & 3U) << 22) |
		(indexed_operation(family.operation) << 12) |
		((registers.right & 15U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
	if (shape.size == 1U) {
		encoded |= (registers.index & 1U) << 20;
		encoded |= (registers.index & 2U) << 20;
		encoded |= (registers.index & 4U) << 9;
	} else if (shape.size == 2U) {
		encoded |= ((registers.right >> 4) & 1U) << 20;
		encoded |= (registers.index & 1U) << 21;
		encoded |= (registers.index & 2U) << 10;
	}
	return encoded;
}

[[nodiscard]] constexpr std::span<const Shape>
allocated_shapes(const Family &family) noexcept
{
	if (is_same_width(family.operation) ||
	    (is_normal_long(family.operation) && indexed(family)))
		return indexed_vector_shapes;
	if (is_normal_long(family.operation))
		return vector_long_shapes;
	return saturating_shapes;
}

[[nodiscard]] constexpr unsigned lane_count(
	const Family &family, Shape shape) noexcept
{
	return indexed(family) ? (shape.size == 1U ? 8U : 4U) : 1U;
}

} // namespace arm64_simd_indexed_test
