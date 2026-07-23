// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_fcvt_precision_test_support.hpp"

#include <array>
#include <span>

namespace arm64_simd_fcvt_vector_test {

namespace base = arm64_simd_test;
namespace scalar = arm64_simd_fcvt_precision_test;
namespace move = arm64_simd_fmov_test;

enum class Operation { long_conversion, narrow };

struct Shape final {
	Operation operation;
	unsigned source_width;
	unsigned destination_width;
};

inline constexpr std::array shapes{
	Shape{Operation::long_conversion, 16U, 32U},
	Shape{Operation::long_conversion, 32U, 64U},
	Shape{Operation::narrow, 32U, 16U},
	Shape{Operation::narrow, 64U, 32U},
};

[[nodiscard]] constexpr std::uint32_t instruction(const Shape &shape,
	bool upper, unsigned destination, unsigned source)
{
	std::uint32_t encoded = shape.operation == Operation::long_conversion ?
		0x0e21'7800U : 0x0e21'6800U;
	if (shape.source_width == 32U &&
	    shape.operation == Operation::long_conversion)
		encoded |= 0x0040'0000U;
	if (shape.source_width == 64U)
		encoded |= 0x0040'0000U;
	if (upper)
		encoded |= 0x4000'0000U;
	return encoded | ((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr unsigned lanes(const Shape &shape)
{
	return (shape.operation == Operation::narrow ? 128U : 64U) /
		shape.source_width;
}

inline void set_source_lane(uml::backend::arm64::CpuState &cpu,
	unsigned source, const Shape &shape, bool upper, unsigned lane,
	std::uint64_t bits)
{
	const unsigned base = shape.operation == Operation::long_conversion && upper ?
		8U : 0U;
	move::write_bits(cpu.vectors[source].bytes,
		base + lane * shape.source_width / 8U, shape.source_width / 8U, bits);
}

inline void require_result(std::uint32_t encoded, const Shape &shape,
	bool upper, unsigned destination, std::span<const std::uint64_t> outputs,
	std::uint32_t exceptions, const uml::backend::arm64::CpuState &initial,
	base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	auto result = shape.operation == Operation::narrow && upper ?
		initial.vectors[destination].bytes : std::array<std::byte, 16>{};
	const unsigned base = shape.operation == Operation::narrow && upper ? 8U : 0U;
	for (unsigned lane = 0; lane < outputs.size(); ++lane)
		move::write_bits(result,
			base + lane * shape.destination_width / 8U,
			shape.destination_width / 8U, outputs[lane]);
	expected.vectors[destination].bytes = result;
	expected.fpsr |= exceptions;
	base::require_architecture_equal(expected,
		harness.run_interpreter(encoded, initial));
	harness.require_differential(encoded, initial);
}

} // namespace arm64_simd_fcvt_vector_test
