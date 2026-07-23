// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_fcvt_precision_test_support.hpp"

#include <array>
#include <span>

namespace arm64_simd_fcvtxn_test {

namespace base = arm64_simd_test;
namespace move = arm64_simd_fmov_test;

enum class Form { scalar, vector_lower, vector_upper };

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	unsigned destination, unsigned source)
{
	const auto base = form == Form::scalar ? 0x7e61'6800U : 0x2e61'6800U;
	return base | (form == Form::vector_upper ? 0x4000'0000U : 0U) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr unsigned lanes(Form form)
{
	return form == Form::scalar ? 1U : 2U;
}

inline void set_source_lane(uml::backend::arm64::CpuState &cpu,
	unsigned source, unsigned lane, std::uint64_t bits)
{
	move::write_bits(cpu.vectors[source].bytes, lane * 8U, 8U, bits);
}

inline void require_result(std::uint32_t encoded, Form form,
	unsigned destination, std::span<const std::uint32_t> outputs,
	std::uint32_t exceptions, const uml::backend::arm64::CpuState &initial,
	base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	auto result = form == Form::vector_upper ||
		(form == Form::scalar && (initial.fpcr & 4U) != 0U) ?
		initial.vectors[destination].bytes : std::array<std::byte, 16>{};
	const unsigned base_offset = form == Form::vector_upper ? 8U : 0U;
	for (unsigned lane = 0; lane < outputs.size(); ++lane)
		move::write_bits(result, base_offset + lane * 4U, 4U, outputs[lane]);
	expected.vectors[destination].bytes = result;
	expected.fpsr |= exceptions;
	base::require_architecture_equal(expected,
		harness.run_interpreter(encoded, initial));
	harness.require_differential(encoded, initial);
}

} // namespace arm64_simd_fcvtxn_test
