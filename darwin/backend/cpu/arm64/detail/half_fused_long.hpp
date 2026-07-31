// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "bfloat_multiply.hpp"
#include "floating_exception.hpp"

#include <bit>
#include <cstdint>

namespace uml::backend::arm64::detail {

struct HalfFusedLong final {
	bool recognized{};
	bool by_element{};
	bool subtract{};
	bool upper{};
	bool wide{};
	unsigned index{};
};

[[nodiscard]] constexpr HalfFusedLong decode_half_fused_long(
	std::uint32_t instruction) noexcept
{
	const bool wide = (instruction & 0x4000'0000U) != 0U;
	const auto vector = instruction & 0xbfe0'fc00U;
	if (vector == 0x0e20'ec00U || vector == 0x2e20'cc00U ||
	    vector == 0x0ea0'ec00U || vector == 0x2ea0'cc00U)
		return {true, false, (instruction & 0x0080'0000U) != 0U,
			(instruction & 0x2000'0000U) != 0U, wide, 0U};
	const auto indexed = instruction & 0xbfc0'f400U;
	if (indexed == 0x0f80'0000U || indexed == 0x2f80'8000U ||
	    indexed == 0x0f80'4000U || indexed == 0x2f80'c000U) {
		const unsigned index = ((instruction >> 20) & 3U) |
			((instruction >> 9) & 4U);
		return {true, true, (instruction & 0x0000'4000U) != 0U,
			(instruction & 0x2000'0000U) != 0U, wide, index};
	}
	return {};
}

[[nodiscard]] constexpr bool reserved_half_fused_long(
	std::uint32_t instruction) noexcept
{
	const auto vector_without_size =
		instruction & ~0x0040'0000U;
	const auto vector = decode_half_fused_long(vector_without_size);
	if ((instruction & 0x0040'0000U) != 0U &&
	    vector.recognized && !vector.by_element)
		return true;
	const auto indexed_without_opcode =
		instruction & ~0x0000'0400U;
	const auto indexed =
		decode_half_fused_long(indexed_without_opcode);
	return (instruction & 0x0000'0400U) != 0U &&
		indexed.recognized && indexed.by_element;
}

struct HalfFusedLongResult final {
	std::uint32_t bits{};
	std::uint32_t exceptions{};
};

namespace half_fused_long_detail {

[[nodiscard]] constexpr bool half_nan(std::uint16_t bits) noexcept
{
	return (bits & 0x7c00U) == 0x7c00U && (bits & 0x03ffU) != 0U;
}

[[nodiscard]] constexpr std::uint32_t expand_half(std::uint16_t bits,
	std::uint32_t fpcr) noexcept
{
	const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000U) << 16;
	const unsigned exponent = (bits >> 10) & 31U;
	const unsigned fraction = bits & 0x03ffU;
	if (exponent == 0U) {
		if (fraction == 0U || (fpcr & (1U << 19)) != 0U)
			return sign;
		const unsigned high = std::bit_width(fraction) - 1U;
		const unsigned float_exponent = static_cast<unsigned>(-24 +
			static_cast<int>(high) + 127);
		const std::uint32_t significand =
			static_cast<std::uint32_t>(fraction) << (23U - high);
		return sign | (float_exponent << 23) |
			(significand & 0x7f'ffffU);
	}
	if (exponent == 31U)
		return sign | 0x7f80'0000U |
			(static_cast<std::uint32_t>(fraction) << 13);
	return sign | ((exponent + 112U) << 23) |
		(static_cast<std::uint32_t>(fraction) << 13);
}

[[nodiscard]] inline HalfFusedLongResult captured_fmadd(
	std::uint32_t addend, std::uint32_t left, std::uint32_t right,
	std::uint32_t fpcr) noexcept
{
	std::uint64_t saved_fpcr;
	std::uint64_t saved_fpsr;
	__asm__ volatile("mrs %0, fpcr" : "=r"(saved_fpcr) : : "memory");
	__asm__ volatile("mrs %0, fpsr" : "=r"(saved_fpsr) : : "memory");
	const std::uint64_t local_fpcr = host_fpcr(fpcr);
	const std::uint64_t clear = 0U;
	__asm__ volatile("msr fpcr, %0" : : "r"(local_fpcr) : "memory");
	__asm__ volatile("msr fpsr, %0" : : "r"(clear) : "memory");
	const auto result = bfloat_multiply_detail::fmadd(addend, left, right);
	std::uint64_t exceptions;
	__asm__ volatile("mrs %0, fpsr" : "=r"(exceptions) : : "memory");
	__asm__ volatile("msr fpcr, %0" : : "r"(saved_fpcr) : "memory");
	__asm__ volatile("msr fpsr, %0" : : "r"(saved_fpsr) : "memory");
	return {result, static_cast<std::uint32_t>(exceptions) & 0xf800'009fU};
}

[[nodiscard]] constexpr bool subnormal_single(std::uint32_t bits) noexcept
{
	return (bits & 0x7f80'0000U) == 0U &&
		(bits & 0x007f'ffffU) != 0U;
}

} // namespace half_fused_long_detail

[[nodiscard]] inline HalfFusedLongResult half_fused_multiply_add(
	std::uint32_t addend, std::uint16_t left_half, std::uint16_t right_half,
	bool subtract, std::uint32_t fpcr) noexcept
{
	using namespace bfloat_multiply_detail;
	using namespace half_fused_long_detail;
	if (subtract && !((fpcr & 2U) != 0U && half_nan(left_half)))
		left_half ^= 0x8000U;
	std::uint32_t left = expand_half(left_half, fpcr);
	std::uint32_t right = expand_half(right_half, fpcr);
	const bool alternative = (fpcr & 2U) != 0U;
	if (!alternative) {
		if ((fpcr & 1U) != 0U && (fpcr & (1U << 24)) == 0U) {
			addend = flush_subnormal(addend);
			return captured_fmadd(addend, left, right, fpcr & ~1U);
		}
		return captured_fmadd(addend, left, right, fpcr);
	}

	const bool addend_denormal = subnormal_single(addend) &&
		(fpcr & 1U) == 0U;
	if ((fpcr & 1U) != 0U)
		addend = flush_subnormal(addend);
	const bool addend_nan = is_nan(addend);
	const bool left_nan = is_nan(left);
	const bool right_nan = is_nan(right);
	const bool any_signaling = is_signaling_nan(addend) ||
		is_signaling_nan(left) || is_signaling_nan(right);
	std::uint32_t selected_nan{};
	if (left_nan && (addend_nan || right_nan))
		selected_nan = left;
	else if (right_nan && addend_nan)
		selected_nan = right;
	else if (is_signaling_nan(addend))
		selected_nan = addend;
	else if (is_signaling_nan(left))
		selected_nan = left;
	else if (is_signaling_nan(right))
		selected_nan = right;
	else if (addend_nan)
		selected_nan = addend;
	else if (left_nan)
		selected_nan = left;
	else if (right_nan)
		selected_nan = right;
	if (selected_nan != 0U)
		return {process_alternative_nan(selected_nan,
			(fpcr & (1U << 25)) != 0U), any_signaling ? 1U : 0U};

	const bool left_infinity = is_infinity(left);
	const bool right_infinity = is_infinity(right);
	const bool left_zero = is_zero(left);
	const bool right_zero = is_zero(right);
	const bool addend_infinity = is_infinity(addend);
	const bool product_infinity = left_infinity || right_infinity;
	const bool product_zero = left_zero || right_zero;
	const bool product_sign = ((left ^ right) >> 31) != 0U;
	const bool addend_sign = (addend >> 31) != 0U;
	if ((left_infinity && right_zero) || (left_zero && right_infinity) ||
	    (addend_infinity && product_infinity &&
	     addend_sign != product_sign))
		return {0xffc0'0000U, 1U};
	if (addend_infinity)
		return {addend, 0U};
	if (product_infinity)
		return {(static_cast<std::uint32_t>(product_sign) << 31) |
			0x7f80'0000U, 0U};
	if (is_zero(addend) && product_zero && addend_sign == product_sign)
		return {static_cast<std::uint32_t>(addend_sign) << 31, 0U};

	const std::uint32_t local_fpcr = fpcr & ~(2U | 1U | (1U << 24));
	auto result = captured_fmadd(addend, left, right, local_fpcr);
	if (addend_denormal)
		result.exceptions |= 1U << 7;
	const bool result_subnormal = subnormal_single(result.bits);
	const bool result_zero = (result.bits & 0x7fff'ffffU) == 0U;
	if (!result_subnormal)
		result.exceptions &= ~(1U << 3);
	if (result_zero)
		result.exceptions &= ~(1U << 3);
	if ((fpcr & (1U << 24)) != 0U && result_subnormal) {
		result.bits &= 0x8000'0000U;
		result.exceptions |= (1U << 3) | (1U << 4);
	}
	return result;
}

} // namespace uml::backend::arm64::detail
