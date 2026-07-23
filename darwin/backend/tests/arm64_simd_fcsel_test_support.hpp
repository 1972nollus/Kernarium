// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_fmov_test_support.hpp"

namespace arm64_simd_fcsel_test {

namespace base = arm64_simd_test;
namespace move = arm64_simd_fmov_test;

[[nodiscard]] constexpr std::uint32_t instruction(unsigned bytes,
	unsigned condition, unsigned destination, unsigned first, unsigned second)
{
	return 0x1e20'0c00U | (move::type_for_bytes(bytes) << 22) |
		((second & 31U) << 16) | ((condition & 15U) << 12) |
		((first & 31U) << 5) | (destination & 31U);
}

inline void require_result(std::uint32_t encoded, unsigned bytes,
	unsigned destination, unsigned first, unsigned second,
	const uml::backend::arm64::CpuState &initial, base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	const unsigned condition = (encoded >> 12) & 15U;
	const unsigned selected =
		uml::backend::arm64::detail::condition_holds(initial.nzcv, condition) ?
			first : second;
	expected.vectors[destination].bytes = {};
	move::write_bits(expected.vectors[destination].bytes, 0U, bytes,
		move::read_bits(initial.vectors[selected].bytes, 0U, bytes));
	base::require_architecture_equal(expected,
		harness.run_interpreter(encoded, initial));
	harness.require_differential(encoded, initial);
}

} // namespace arm64_simd_fcsel_test
