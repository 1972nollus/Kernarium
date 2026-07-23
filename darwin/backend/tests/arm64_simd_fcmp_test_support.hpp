// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_fcsel_test_support.hpp"

namespace arm64_simd_fcmp_test {

namespace base = arm64_simd_test;
namespace move = arm64_simd_fmov_test;

[[nodiscard]] constexpr std::uint32_t direct_instruction(unsigned bytes,
	bool signaling, bool zero, unsigned left, unsigned right = 0U)
{
	return 0x1e20'2000U | (move::type_for_bytes(bytes) << 22) |
		((right & 31U) << 16) | ((left & 31U) << 5) |
		(signaling ? 16U : 0U) | (zero ? 8U : 0U);
}

[[nodiscard]] constexpr std::uint32_t conditional_instruction(unsigned bytes,
	bool signaling, unsigned condition, unsigned fallback, unsigned left,
	unsigned right)
{
	return 0x1e20'0400U | (move::type_for_bytes(bytes) << 22) |
		((right & 31U) << 16) | ((condition & 15U) << 12) |
		((left & 31U) << 5) | (signaling ? 16U : 0U) |
		(fallback & 15U);
}

[[nodiscard]] constexpr std::uint64_t positive_zero(unsigned)
{
	return 0U;
}

[[nodiscard]] constexpr std::uint64_t negative_zero(unsigned bytes)
{
	return std::uint64_t{1} << (bytes * 8U - 1U);
}

[[nodiscard]] constexpr std::uint64_t positive_one(unsigned bytes)
{
	return bytes == 2U ? 0x3c00U :
		bytes == 4U ? 0x3f80'0000U : 0x3ff0'0000'0000'0000ULL;
}

[[nodiscard]] constexpr std::uint64_t positive_two(unsigned bytes)
{
	return bytes == 2U ? 0x4000U :
		bytes == 4U ? 0x4000'0000U : 0x4000'0000'0000'0000ULL;
}

[[nodiscard]] constexpr std::uint64_t negative_one(unsigned bytes)
{
	return positive_one(bytes) | negative_zero(bytes);
}

[[nodiscard]] constexpr std::uint64_t negative_two(unsigned bytes)
{
	return positive_two(bytes) | negative_zero(bytes);
}

[[nodiscard]] constexpr std::uint64_t positive_infinity(unsigned bytes)
{
	return bytes == 2U ? 0x7c00U :
		bytes == 4U ? 0x7f80'0000U : 0x7ff0'0000'0000'0000ULL;
}

[[nodiscard]] constexpr std::uint64_t negative_infinity(unsigned bytes)
{
	return positive_infinity(bytes) | negative_zero(bytes);
}

[[nodiscard]] constexpr std::uint64_t quiet_nan(unsigned bytes)
{
	return bytes == 2U ? 0x7e51U :
		bytes == 4U ? 0x7fc1'2345U : 0x7ff8'1234'5678'9abcULL;
}

[[nodiscard]] constexpr std::uint64_t signaling_nan(unsigned bytes)
{
	return bytes == 2U ? 0x7d51U :
		bytes == 4U ? 0x7f81'2345U : 0x7ff0'1234'5678'9abcULL;
}

[[nodiscard]] constexpr std::uint64_t minimum_subnormal(unsigned bytes,
	bool negative = false)
{
	return 1U | (negative ? negative_zero(bytes) : 0U);
}

inline void put(uml::backend::arm64::CpuState &cpu, unsigned reg,
	unsigned bytes, std::uint64_t bits)
{
	move::write_bits(cpu.vectors[reg].bytes, 0U, bytes, bits);
}

inline void require_result(std::uint32_t encoded, std::uint32_t nzcv,
	std::uint32_t exceptions, const uml::backend::arm64::CpuState &initial,
	base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	expected.nzcv = nzcv;
	expected.fpsr |= exceptions;
	base::require_architecture_equal(expected,
		harness.run_interpreter(encoded, initial));
	harness.require_differential(encoded, initial);
}

} // namespace arm64_simd_fcmp_test
