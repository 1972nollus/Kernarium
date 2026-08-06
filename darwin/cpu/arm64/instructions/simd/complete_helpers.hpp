// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../../dispatcher.hpp"

#include "../../detail/floating_compare.hpp"
#include "../../detail/floating_indexed.hpp"
#include "../../detail/instruction_helpers.hpp"
#include "../../detail/integer_multiply.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#pragma STDC FENV_ACCESS ON

namespace uml::cpu::arm64 {

using detail::continue_after;
using detail::decode_little_endian;
using detail::encode_little_endian;

namespace simd_complete_detail {

using SignedWide = __int128;
using UnsignedWide = unsigned __int128;

constexpr std::uint64_t mask_for(unsigned bits)
{
	return bits == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << bits) - 1;
}

constexpr SignedWide signed_minimum(unsigned bits)
{
	return -static_cast<SignedWide>(UnsignedWide{1} << (bits - 1));
}

constexpr SignedWide signed_maximum(unsigned bits)
{
	return static_cast<SignedWide>((UnsignedWide{1} << (bits - 1)) - 1);
}

constexpr SignedWide as_signed(std::uint64_t value, unsigned bits)
{
	return bits == 64 ? static_cast<SignedWide>(static_cast<std::int64_t>(value)) :
		static_cast<SignedWide>(detail::sign_extend(value, bits));
}

constexpr SignedWide absolute_wide(SignedWide value)
{
	return value < 0 ? -value : value;
}

std::uint64_t saturate_signed(CpuState &cpu, SignedWide value, unsigned bits);
std::uint64_t saturate_unsigned(
	CpuState &cpu, SignedWide value, unsigned bits);
std::uint64_t rounded_shift_right_signed(
	std::uint64_t raw, unsigned bits, unsigned shift, bool rounding);
std::uint64_t rounded_shift_right_unsigned(
	std::uint64_t value, unsigned bits, unsigned shift, bool rounding);
std::uint64_t polynomial_multiply_byte(
	std::uint64_t left, std::uint64_t right);

constexpr bool valid_integer_two_register_misc(unsigned opcode,
	bool unsigned_group, bool scalar, bool q, unsigned size)
{
	/* These opcode slots are vector-only reverse/byte operations. Their
	 * scalar encodings are unallocated and must stop before register reads. */
	if (scalar && (opcode == 0U || opcode == 1U || opcode == 5U))
		return false;
	switch (opcode) {
	case 2:  // SADDLP/UADDLP
	case 6:  // SADALP/UADALP
	case 4:  // CLS/CLZ
		return !scalar && size < 3;
	case 3:  // SUQADD/USQADD
	case 7:  // SQABS/SQNEG
		return scalar || q || size < 3;
	case 11: // ABS/NEG
		return scalar ? size == 3 : q || size < 3;
	case 18: // XTN/SQXTUN
		return size < 3 && (unsigned_group || !scalar);
	case 19: // SHLL
		return size < 3 && unsigned_group && !scalar;
	case 20: // SQXTN/UQXTN
		return size < 3;
	default:
		return true;
	}
}

constexpr std::uint32_t unsigned_reciprocal_estimate(std::uint32_t operand)
{
	if ((operand & 0x8000'0000U) == 0)
		return 0xffff'ffffU;
	const std::uint32_t a = ((operand >> 23) * 2U) + 1U;
	const std::uint32_t b = (std::uint32_t{1} << 19) / a;
	return ((b + 1U) / 2U) << 23;
}

constexpr std::uint32_t unsigned_reciprocal_sqrt_estimate(
	std::uint32_t operand)
{
	if ((operand & 0xc000'0000U) == 0)
		return 0xffff'ffffU;
	std::uint32_t a = operand >> 23;
	if (a < 256U)
		a = a * 2U + 1U;
	else
		a = ((a >> 1) * 2U + 1U) * 2U;
	std::uint32_t b = 512U;
	while (static_cast<std::uint64_t>(a) * (b + 1U) * (b + 1U) <
	       (std::uint64_t{1} << 28))
		++b;
	return ((b + 1U) / 2U) << 23;
}

template<typename Floating>
Floating read_floating_lane(const VectorRegister &vector, std::size_t lane)
{
	using Integer = std::conditional_t<sizeof(Floating) == 8,
		std::uint64_t, std::conditional_t<sizeof(Floating) == 4,
		std::uint32_t, std::uint16_t>>;
	return std::bit_cast<Floating>(static_cast<Integer>(decode_little_endian(
		std::span{vector.bytes}.subspan(lane * sizeof(Floating),
			sizeof(Floating)))));
}

template<typename Floating>
void write_floating_lane(VectorRegister &vector, std::size_t lane, Floating value)
{
	using Integer = std::conditional_t<sizeof(Floating) == 8,
		std::uint64_t, std::conditional_t<sizeof(Floating) == 4,
		std::uint32_t, std::uint16_t>>;
	encode_little_endian(std::bit_cast<Integer>(value),
		std::span{vector.bytes}.subspan(lane * sizeof(Floating),
			sizeof(Floating)));
}

template<typename Floating>
Floating round_integral(Floating value, unsigned mode)
{
	switch (mode) {
	case 0: return std::nearbyint(value);
	case 1: return std::ceil(value);
	case 2: return std::floor(value);
	case 4: return std::round(value);
	default: return std::trunc(value);
	}
}

std::uint64_t floating_to_integer(
	long double value, bool unsigned_result,
	unsigned bits, unsigned rounding);

template<typename Floating>
Floating reciprocal_exponent(Floating value)
{
	if (!std::isfinite(value) || value == 0)
		return static_cast<Floating>(1) / value;
	int exponent;
	std::frexp(std::fabs(value), &exponent);
	return std::copysign(std::ldexp(static_cast<Floating>(1), 1 - exponent),
		value);
}

template<typename Floating>
Floating round_to_odd(long double value)
{
	Floating result = static_cast<Floating>(value);
	if (static_cast<long double>(result) == value || !std::isfinite(result))
		return result;
	if ((value > 0 && static_cast<long double>(result) > value) ||
	    (value < 0 && static_cast<long double>(result) < value))
		result = std::nextafter(result, static_cast<Floating>(0));
	using Integer = std::conditional_t<sizeof(Floating) == 8,
		std::uint64_t, std::uint32_t>;
	auto bits = std::bit_cast<Integer>(result);
	bits |= 1U;
	return std::bit_cast<Floating>(bits);
}

template<typename Floating>
Floating multiply_extended(Floating left, Floating right)
{
	using Integer = std::conditional_t<sizeof(Floating) == 8,
		std::uint64_t, std::conditional_t<sizeof(Floating) == 4,
		std::uint32_t, std::uint16_t>>;
	constexpr Integer sign = Integer{1} << (sizeof(Floating) * 8U - 1U);
	constexpr Integer infinity = sizeof(Floating) == 8 ?
		static_cast<Integer>(0x7ff0'0000'0000'0000ULL) :
		sizeof(Floating) == 4 ? static_cast<Integer>(0x7f80'0000U) :
		static_cast<Integer>(0x7c00U);
	constexpr Integer two = sizeof(Floating) == 8 ?
		static_cast<Integer>(0x4000'0000'0000'0000ULL) :
		sizeof(Floating) == 4 ? static_cast<Integer>(0x4000'0000U) :
		static_cast<Integer>(0x4000U);
	const auto left_bits = std::bit_cast<Integer>(left);
	const auto right_bits = std::bit_cast<Integer>(right);
	const auto left_magnitude = left_bits & ~sign;
	const auto right_magnitude = right_bits & ~sign;
	if ((left_magnitude == infinity && right_magnitude == 0) ||
	    (left_magnitude == 0 && right_magnitude == infinity))
		return std::bit_cast<Floating>(static_cast<Integer>(
			two | ((left_bits ^ right_bits) & sign)));
	return left * right;
}

template<typename Floating>
Floating fused_multiply_add(Floating left, Floating right,
	Floating accumulator, bool subtract)
{
	Floating result;
#define UML_INDEXED_FUSED_ASM(suffix, operation) \
	__asm__ volatile(#operation " %" #suffix "0, %" #suffix "1, %" #suffix \
		"2, %" #suffix "3" : "=w"(result) : "w"(left), "w"(right), \
		"w"(accumulator))
	if constexpr (sizeof(Floating) == 2) {
		if (subtract)
			UML_INDEXED_FUSED_ASM(h, fmsub);
		else
			UML_INDEXED_FUSED_ASM(h, fmadd);
	} else if constexpr (sizeof(Floating) == 4) {
		if (subtract)
			UML_INDEXED_FUSED_ASM(s, fmsub);
		else
			UML_INDEXED_FUSED_ASM(s, fmadd);
	} else {
		if (subtract)
			UML_INDEXED_FUSED_ASM(d, fmsub);
		else
			UML_INDEXED_FUSED_ASM(d, fmadd);
	}
#undef UML_INDEXED_FUSED_ASM
	return result;
}

} // namespace simd_complete_detail


} // namespace uml::cpu::arm64
