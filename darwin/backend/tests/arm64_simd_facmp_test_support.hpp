// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_floating_compare_test_support.hpp"

namespace arm64_simd_facmp_test {

namespace base = arm64_simd_test;
namespace compare = arm64_simd_floating_compare_test;
namespace scalar = arm64_simd_fcmp_test;

using Shape = compare::Shape;
inline constexpr auto shapes = compare::shapes;

enum class Operation : unsigned { greater_equal, greater };

[[nodiscard]] constexpr std::uint32_t instruction(Operation operation,
	const Shape &shape, unsigned destination, unsigned left, unsigned right)
{
	const bool half = shape.width == 16U;
	std::uint32_t encoded = half ?
		(shape.scalar ? 0x7e40'2c00U : 0x2e40'2c00U) :
		(shape.scalar ? 0x7e20'ec00U : 0x2e20'ec00U);
	if (!shape.scalar && shape.q)
		encoded |= 0x4000'0000U;
	if (!half && shape.width == 64U)
		encoded |= 0x0040'0000U;
	if (operation == Operation::greater)
		encoded |= 0x0080'0000U;
	return encoded | ((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

inline void require_result(std::uint32_t encoded, const Shape &shape,
	Operation operation, unsigned destination, unsigned right,
	std::span<const bool> matches, std::uint32_t exceptions,
	const uml::backend::arm64::CpuState &initial, base::Harness &harness)
{
	compare::require_result(encoded, shape,
		operation == Operation::greater_equal ? compare::Operation::greater_equal :
			compare::Operation::greater,
		false, destination, right, matches, exceptions, initial, harness);
}

} // namespace arm64_simd_facmp_test
