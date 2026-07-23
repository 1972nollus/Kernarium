// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_fp_indexed_test_support.hpp"

namespace arm64_simd_fmulx_test {

namespace fp = arm64_simd_fp_indexed_test;

[[nodiscard]] constexpr std::uint32_t instruction(const fp::Shape &shape,
	unsigned destination, unsigned left, unsigned right)
{
	const std::uint32_t operation = shape.size == 0U ?
		(shape.scalar ? 0x5e40'1c00U : 0x0e40'1c00U) :
		(shape.scalar ? 0x5e20'dc00U : 0x0e20'dc00U);
	return operation | (!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
		(shape.size == 3U ? 0x0040'0000U : 0U) |
		((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

} // namespace arm64_simd_fmulx_test
