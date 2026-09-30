// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "floating_bits.hpp"
#include "floating_exception.hpp"

#include <bit>
#include <cstdint>

namespace uml::cpu::arm64::detail {

struct BFloatMultiplyLong final {
	bool recognized{};
	bool by_element{};
	bool top{};
	unsigned index{};
};

[[nodiscard]] constexpr BFloatMultiplyLong decode_bfloat_multiply_long(
	std::uint32_t instruction) noexcept
{
	if ((instruction & 0xbfe0'fc00U) == 0x2ec0'fc00U)
		return {true, false, (instruction & 0x4000'0000U) != 0U, 0U};
	if ((instruction & 0xbfc0'f400U) == 0x0fc0'f000U) {
		const unsigned index = ((instruction >> 20) & 3U) |
			((instruction >> 9) & 4U);
		return {true, true, (instruction & 0x4000'0000U) != 0U, index};
	}
	return {};
}

namespace bfloat_multiply_detail {

using namespace floating_bits;

[[nodiscard]] inline std::uint32_t fmadd(std::uint32_t addend,
	std::uint32_t left, std::uint32_t right) noexcept
{
	const float addend_value = std::bit_cast<float>(addend);
	const float left_value = std::bit_cast<float>(left);
	const float right_value = std::bit_cast<float>(right);
	float result;
	__asm__ volatile("fmadd %s0, %s1, %s2, %s3" : "=w"(result) :
		"w"(left_value), "w"(right_value), "w"(addend_value));
	return std::bit_cast<std::uint32_t>(result);
}

[[nodiscard]] inline std::uint32_t controlled_fmadd(std::uint32_t addend,
	std::uint32_t left, std::uint32_t right, std::uint32_t fpcr,
	bool suppress_exceptions) noexcept
{
	std::uint64_t saved_fpcr;
	std::uint64_t saved_fpsr{};
	__asm__ volatile("mrs %0, fpcr" : "=r"(saved_fpcr) : : "memory");
	if (suppress_exceptions)
		__asm__ volatile("mrs %0, fpsr" : "=r"(saved_fpsr) : : "memory");
	const std::uint64_t local_fpcr = host_fpcr(fpcr);
	__asm__ volatile("msr fpcr, %0" : : "r"(local_fpcr) : "memory");
	if (suppress_exceptions) {
		const std::uint64_t clear = 0U;
		__asm__ volatile("msr fpsr, %0" : : "r"(clear) : "memory");
	}
	const auto result = fmadd(addend, left, right);
	__asm__ volatile("msr fpcr, %0" : : "r"(saved_fpcr) : "memory");
	if (suppress_exceptions)
		__asm__ volatile("msr fpsr, %0" : : "r"(saved_fpsr) : "memory");
	return result;
}

} // namespace bfloat_multiply_detail

/* BFMulAddH expands both BFloat16 multiplicands exactly to binary32.  The
 * ordinary path therefore maps exactly to scalar FMADD under the guest FPCR.
 * AH is handled explicitly so the reference/software path remains correct on
 * hosts without FEAT_AFP: alternative NaN priority and special results are
 * resolved in integer bits, while the finite operation runs with forced RNE,
 * flushed inputs/output, and its exceptions suppressed. */
[[nodiscard]] inline std::uint32_t bfloat_multiply_add(
	std::uint32_t addend, std::uint16_t left, std::uint16_t right,
	std::uint32_t fpcr) noexcept
{
	using namespace bfloat_multiply_detail;
	std::uint32_t left32 = static_cast<std::uint32_t>(left) << 16;
	std::uint32_t right32 = static_cast<std::uint32_t>(right) << 16;
	const bool alternative = (fpcr & 2U) != 0U;
	if (!alternative) {
		/* FIZ is an AFP control. Emulate it before FMADD on older hosts; FZ
		 * takes precedence and must still report IDC for flushed inputs. */
		if ((fpcr & 1U) != 0U && (fpcr & (1U << 24)) == 0U) {
			addend = flush_subnormal(addend);
			left32 = flush_subnormal(left32);
			right32 = flush_subnormal(right32);
			return controlled_fmadd(addend, left32, right32, fpcr & ~1U,
				false);
		}
		return fmadd(addend, left32, right32);
	}

	addend = flush_subnormal(addend);
	left32 = flush_subnormal(left32);
	right32 = flush_subnormal(right32);
	const bool addend_nan = is_nan(addend);
	const bool left_nan = is_nan(left32);
	const bool right_nan = is_nan(right32);
	std::uint32_t selected_nan{};
	if (left_nan && (addend_nan || right_nan))
		selected_nan = left32;
	else if (right_nan && addend_nan)
		selected_nan = right32;
	else if (is_signaling_nan(addend))
		selected_nan = addend;
	else if (is_signaling_nan(left32))
		selected_nan = left32;
	else if (is_signaling_nan(right32))
		selected_nan = right32;
	else if (addend_nan)
		selected_nan = addend;
	else if (left_nan)
		selected_nan = left32;
	else if (right_nan)
		selected_nan = right32;
	if (selected_nan != 0U)
		return process_nan(selected_nan, fpcr);

	const bool left_infinity = is_infinity(left32);
	const bool right_infinity = is_infinity(right32);
	const bool left_zero = is_zero(left32);
	const bool right_zero = is_zero(right32);
	const bool addend_infinity = is_infinity(addend);
	const bool product_infinity = left_infinity || right_infinity;
	const bool product_zero = left_zero || right_zero;
	const bool product_sign = ((left32 ^ right32) >> 31) != 0U;
	const bool addend_sign = (addend >> 31) != 0U;
	if ((left_infinity && right_zero) || (left_zero && right_infinity) ||
	    (addend_infinity && product_infinity &&
	     addend_sign != product_sign))
		return 0xffc0'0000U;
	if (addend_infinity)
		return addend;
	if (product_infinity)
		return (static_cast<std::uint32_t>(product_sign) << 31) |
			0x7f80'0000U;
	if (is_zero(addend) && product_zero && addend_sign == product_sign)
		return static_cast<std::uint32_t>(addend_sign) << 31;

	const std::uint32_t local_fpcr =
		(fpcr & ~((3U << 22) | 2U | 1U)) | (1U << 24);
	return controlled_fmadd(addend, left32, right32, local_fpcr, true);
}

} // namespace uml::cpu::arm64::detail
