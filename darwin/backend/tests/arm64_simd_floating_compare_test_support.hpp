// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_fcmp_test_support.hpp"

#include <array>
#include <span>

namespace arm64_simd_floating_compare_test {

namespace base = arm64_simd_test;
namespace move = arm64_simd_fmov_test;
namespace scalar = arm64_simd_fcmp_test;

enum class Operation : unsigned {
	equal,
	greater_equal,
	greater,
	less_equal,
	less,
};

struct Shape final {
	unsigned width;
	unsigned vector_bytes;
	bool scalar;
	bool q;
};

inline constexpr std::array shapes{
	Shape{16U, 2U, true, false}, Shape{32U, 4U, true, false},
	Shape{64U, 8U, true, false}, Shape{16U, 8U, false, false},
	Shape{16U, 16U, false, true}, Shape{32U, 8U, false, false},
	Shape{32U, 16U, false, true}, Shape{64U, 16U, false, true},
};

[[nodiscard]] constexpr std::uint32_t register_instruction(Operation operation,
	const Shape &shape, unsigned destination, unsigned left, unsigned right)
{
	const bool half = shape.width == 16U;
	std::uint32_t encoded = half ?
		(shape.scalar ? 0x5e40'2400U : 0x0e40'2400U) :
		(shape.scalar ? 0x5e20'e400U : 0x0e20'e400U);
	if (!shape.scalar && shape.q)
		encoded |= 0x4000'0000U;
	if (!half && shape.width == 64U)
		encoded |= 0x0040'0000U;
	if (operation != Operation::equal)
		encoded |= 0x2000'0000U;
	if (operation == Operation::greater)
		encoded |= 0x0080'0000U;
	return encoded | ((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t zero_instruction(Operation operation,
	const Shape &shape, unsigned destination, unsigned source)
{
	const bool half = shape.width == 16U;
	std::uint32_t encoded = half ?
		(shape.scalar ? 0x5ef8'd800U : 0x0ef8'd800U) :
		(shape.scalar ? 0x5ea0'd800U : 0x0ea0'd800U);
	if (!shape.scalar && shape.q)
		encoded |= 0x4000'0000U;
	if (!half && shape.width == 64U)
		encoded |= 0x0040'0000U;
	switch (operation) {
	case Operation::equal: break;
	case Operation::greater_equal: encoded = (encoded - 0x1000U) |
		0x2000'0000U; break;
	case Operation::greater: encoded -= 0x1000U; break;
	case Operation::less_equal: encoded |= 0x2000'0000U; break;
	case Operation::less: encoded += 0x1000U; break;
	}
	return encoded | ((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint64_t lane_mask(unsigned width)
{
	return width == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << width) - 1U;
}

inline void write_lane(std::array<std::byte, 16> &bytes, const Shape &shape,
	unsigned lane, std::uint64_t bits)
{
	move::write_bits(bytes, lane * shape.width / 8U, shape.width / 8U, bits);
}

inline void require_result(std::uint32_t encoded, const Shape &shape,
	Operation operation, bool zero, unsigned destination, unsigned right,
	std::span<const bool> matches, std::uint32_t exceptions,
	const uml::backend::arm64::CpuState &initial, base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	std::array<std::byte, 16> result{};
	if (shape.scalar && !zero && (initial.fpcr & 4U) != 0U)
		result = initial.vectors[right].bytes;
	for (unsigned lane = 0; lane < matches.size(); ++lane)
		write_lane(result, shape, lane,
			matches[lane] ? lane_mask(shape.width) : 0U);
	expected.vectors[destination].bytes = result;
	expected.fpsr |= exceptions;
	base::require_architecture_equal(expected,
		harness.run_interpreter(encoded, initial));
	harness.require_differential(encoded, initial);
	(void)operation;
}

[[nodiscard]] constexpr bool ordered_match(Operation operation,
	int relation) noexcept
{
	switch (operation) {
	case Operation::equal: return relation == 0;
	case Operation::greater_equal: return relation >= 0;
	case Operation::greater: return relation > 0;
	case Operation::less_equal: return relation <= 0;
	case Operation::less: return relation < 0;
	}
	return false;
}

} // namespace arm64_simd_floating_compare_test
