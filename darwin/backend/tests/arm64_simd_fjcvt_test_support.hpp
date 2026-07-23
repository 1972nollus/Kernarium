// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_fcvtzi_test_support.hpp"

namespace arm64_simd_fjcvt_test {

namespace base = arm64_simd_test;
namespace move = arm64_simd_fmov_test;

[[nodiscard]] constexpr std::uint32_t instruction(
	unsigned destination, unsigned source)
{
	return 0x1e7e'0000U | ((source & 31U) << 5) | (destination & 31U);
}

inline void require_result(unsigned destination, std::uint32_t value,
	std::uint32_t nzcv, std::uint32_t exceptions,
	const uml::backend::arm64::CpuState &initial, base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	if (destination != 31U)
		expected.registers[destination] = value;
	expected.nzcv = nzcv;
	expected.fpsr |= exceptions;
	const auto encoded = instruction(destination, 29U);
	const auto interpreted = harness.run_interpreter(encoded, initial);
	if (interpreted.nzcv != expected.nzcv)
		std::fprintf(stderr,
			"FJCVTZS interpreter NZCV: expected=%08x actual=%08x\n",
			expected.nzcv, interpreted.nzcv);
	base::require_architecture_equal(expected, interpreted);
	harness.require_differential(encoded, initial);
}

inline void set_source(uml::backend::arm64::CpuState &cpu,
	std::uint64_t bits)
{
	move::write_bits(cpu.vectors[29].bytes, 0U, 8U, bits);
}

} // namespace arm64_simd_fjcvt_test
