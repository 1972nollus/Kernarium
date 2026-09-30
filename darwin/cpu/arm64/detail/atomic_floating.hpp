// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "instruction_primitives.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>
#include <type_traits>

namespace uml::cpu::arm64::detail {

enum class AtomicFloatingOperation : unsigned {
	add,
	maximum,
	maximum_number,
	minimum,
	minimum_number,
};

struct AtomicFloating final {
	bool recognized{};
	bool valid{};
	bool bfloat{};
	bool return_old{};
	bool acquire{};
	bool release{};
	unsigned width{};
	AtomicFloatingOperation operation{};
};

[[nodiscard]] constexpr AtomicFloating decode_atomic_floating(
	std::uint32_t instruction) noexcept
{
	/* FEAT_LSFE occupies the scalar-atomic class with VR=1. Route the whole
	 * allocation here so malformed store-only encodings are rejected before
	 * an address, SIMD register, or memory location is observed.
	 */
	if ((instruction & 0x3f20'0c00U) != 0x3c20'0000U)
		return {};
	const unsigned size = instruction >> 30;
	const bool return_old = (instruction & 0x8000U) == 0U;
	const bool acquire = (instruction & 0x0080'0000U) != 0U;
	const bool release = (instruction & 0x0040'0000U) != 0U;
	const unsigned opcode = (instruction >> 12) & 7U;
	AtomicFloatingOperation operation{};
	bool operation_valid = true;
	switch (opcode) {
	case 0: operation = AtomicFloatingOperation::add; break;
	case 4: operation = AtomicFloatingOperation::maximum; break;
	case 6: operation = AtomicFloatingOperation::maximum_number; break;
	case 5: operation = AtomicFloatingOperation::minimum; break;
	case 7: operation = AtomicFloatingOperation::minimum_number; break;
	default: operation_valid = false; break;
	}
	const bool store_valid = return_old ||
		(!acquire && (instruction & 31U) == 31U);
	return {true, operation_valid && store_valid, size == 0U, return_old,
		acquire, release, size <= 1U ? 16U : size == 2U ? 32U : 64U,
		operation};
}

[[nodiscard]] constexpr std::uintptr_t atomic_floating_operand(
	std::uint32_t instruction) noexcept
{
	return ((instruction >> 16) & 31U) |
		(static_cast<std::uintptr_t>(instruction & 31U) << 8) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 16);
}

namespace atomic_floating_detail {

enum class BFloatClass : unsigned {
	zero,
	finite,
	infinity,
	quiet_nan,
	signaling_nan,
};

struct BFloatValue final {
	BFloatClass type{};
	bool sign{};
	std::uint64_t significand{};
	int exponent{};
	std::uint16_t bits{};
};

[[nodiscard]] constexpr std::uint32_t local_fpcr(
	std::uint32_t fpcr) noexcept
{
	/* MemAtomicFP disables every FP trap and forces DN. It also clears AH and
	 * ORs FIZ into FZ, but the virtual CPU omits FEAT_AFP, so AH and FIZ are
	 * RES0. The caller separately preserves FPSR so none of the operation's
	 * cumulative flags escape.
	 */
	return (fpcr & ~0x0000'9f00U) | (1U << 25);
}

[[nodiscard]] constexpr std::uint16_t flush_bfloat_input(
	std::uint16_t bits, std::uint32_t fpcr) noexcept
{
	return (bits & 0x7f80U) == 0U && (bits & 0x7fU) != 0U &&
		(fpcr & (1U << 24)) != 0U ? bits & 0x8000U : bits;
}

[[nodiscard]] constexpr BFloatValue unpack_bfloat(
	std::uint16_t input, std::uint32_t fpcr) noexcept
{
	const auto bits = flush_bfloat_input(input, fpcr);
	const bool sign = (bits & 0x8000U) != 0U;
	const unsigned exponent = (bits >> 7) & 0xffU;
	const unsigned fraction = bits & 0x7fU;
	if (exponent == 0U)
		return {fraction == 0U ? BFloatClass::zero : BFloatClass::finite,
			sign, fraction, -133, bits};
	if (exponent == 0xffU) {
		if (fraction == 0U)
			return {BFloatClass::infinity, sign, 0U, 0, bits};
		return {(fraction & 0x40U) != 0U ? BFloatClass::quiet_nan :
			BFloatClass::signaling_nan, sign, 0U, 0, bits};
	}
	return {BFloatClass::finite, sign, 0x80U | fraction,
		static_cast<int>(exponent) - 127 - 7, bits};
}

[[nodiscard]] constexpr bool is_nan(BFloatClass type) noexcept
{
	return type == BFloatClass::quiet_nan ||
		type == BFloatClass::signaling_nan;
}

[[nodiscard]] constexpr unsigned bit_width128(
	unsigned __int128 value) noexcept
{
	const auto high = static_cast<std::uint64_t>(value >> 64);
	return high != 0U ? 64U + std::bit_width(high) :
		std::bit_width(static_cast<std::uint64_t>(value));
}

[[nodiscard]] constexpr unsigned __int128 shift_right_jam(
	unsigned __int128 value, unsigned shift) noexcept
{
	if (shift == 0U)
		return value;
	if (shift >= 128U)
		return value != 0U ? 1U : 0U;
	const auto mask = (static_cast<unsigned __int128>(1U) << shift) - 1U;
	return (value >> shift) | static_cast<unsigned>(
		(value & mask) != 0U);
}

[[nodiscard]] constexpr unsigned __int128 round_shift(
	unsigned __int128 value, unsigned shift, unsigned rounding,
	bool sign) noexcept
{
	if (shift == 0U)
		return value;
	unsigned __int128 truncated{};
	unsigned __int128 remainder{};
	if (shift < 128U) {
		truncated = value >> shift;
		remainder = value &
			((static_cast<unsigned __int128>(1U) << shift) - 1U);
	} else {
		remainder = value;
	}
	if (remainder == 0U)
		return truncated;
	bool increment = false;
	if (rounding == 0U && shift <= 128U) {
		const auto halfway = static_cast<unsigned __int128>(1U) <<
			(shift - 1U);
		increment = remainder > halfway ||
			(remainder == halfway && (truncated & 1U) != 0U);
	} else if (rounding == 1U) {
		increment = !sign;
	} else if (rounding == 2U) {
		increment = sign;
	}
	return truncated + static_cast<unsigned>(increment);
}

[[nodiscard]] constexpr std::uint16_t round_bfloat(
	unsigned __int128 magnitude, int unit_exponent, bool sign,
	std::uint32_t fpcr) noexcept
{
	const unsigned high = bit_width128(magnitude) - 1U;
	int value_exponent = unit_exponent + static_cast<int>(high);
	const bool normal = value_exponent >= -126;
	const int signed_shift = normal ? static_cast<int>(high) - 7 :
		-133 - unit_exponent;
	unsigned __int128 unrounded = magnitude;
	unsigned shift = 0;
	if (signed_shift < 0)
		unrounded <<= static_cast<unsigned>(-signed_shift);
	else
		shift = static_cast<unsigned>(signed_shift);
	const unsigned rounding = (fpcr >> 22) & 3U;
	auto significand = static_cast<std::uint64_t>(round_shift(
		unrounded, shift, rounding, sign));
	unsigned exponent_field = 0;
	if (normal) {
		exponent_field = static_cast<unsigned>(value_exponent + 127);
		if (significand >= 0x100U) {
			significand >>= 1U;
			++exponent_field;
		}
	} else if (significand >= 0x80U) {
		exponent_field = 1U;
		significand = 0U;
	}
	if (exponent_field >= 0xffU) {
		const bool infinity = rounding == 0U ||
			(rounding == 1U && !sign) || (rounding == 2U && sign);
		return static_cast<std::uint16_t>((sign ? 0x8000U : 0U) |
			(infinity ? 0x7f80U : 0x7f7fU));
	}
	const auto result = static_cast<std::uint16_t>(
		(sign ? 0x8000U : 0U) | (exponent_field << 7) |
		(significand & 0x7fU));
	if ((fpcr & (1U << 24)) != 0U && exponent_field == 0U &&
	    (result & 0x7fU) != 0U)
		return static_cast<std::uint16_t>(sign ? 0x8000U : 0U);
	return result;
}

[[nodiscard]] constexpr std::uint16_t add_bfloat(
	std::uint16_t left_bits, std::uint16_t right_bits,
	std::uint32_t fpcr) noexcept
{
	fpcr = local_fpcr(fpcr);
	const auto left = unpack_bfloat(left_bits, fpcr);
	const auto right = unpack_bfloat(right_bits, fpcr);
	if (is_nan(left.type) || is_nan(right.type) ||
	    (left.type == BFloatClass::infinity &&
	     right.type == BFloatClass::infinity && left.sign != right.sign))
		return 0x7fc0U;
	if (left.type == BFloatClass::infinity)
		return left.bits;
	if (right.type == BFloatClass::infinity)
		return right.bits;
	if (left.type == BFloatClass::zero &&
	    right.type == BFloatClass::zero && left.sign == right.sign)
		return static_cast<std::uint16_t>(left.sign ? 0x8000U : 0U);

	constexpr unsigned target = 120U;
	const int left_top = left.type == BFloatClass::zero ? -10000 :
		left.exponent + static_cast<int>(std::bit_width(left.significand)) - 1;
	const int right_top = right.type == BFloatClass::zero ? -10000 :
		right.exponent + static_cast<int>(std::bit_width(right.significand)) - 1;
	const int top = std::max(left_top, right_top);
	const auto align = [top](const BFloatValue &value) constexpr {
		if (value.type == BFloatClass::zero)
			return static_cast<unsigned __int128>(0U);
		const unsigned high = std::bit_width(value.significand) - 1U;
		auto aligned = static_cast<unsigned __int128>(value.significand) <<
			(target - high);
		const unsigned difference = static_cast<unsigned>(top -
			(value.exponent + static_cast<int>(high)));
		return shift_right_jam(aligned, difference);
	};
	const auto left_magnitude = align(left);
	const auto right_magnitude = align(right);
	unsigned __int128 magnitude{};
	bool sign{};
	if (left.sign == right.sign) {
		magnitude = left_magnitude + right_magnitude;
		sign = left.sign;
	} else if (left_magnitude >= right_magnitude) {
		magnitude = left_magnitude - right_magnitude;
		sign = left.sign;
	} else {
		magnitude = right_magnitude - left_magnitude;
		sign = right.sign;
	}
	if (magnitude == 0U)
		return static_cast<std::uint16_t>(((fpcr >> 22) & 3U) == 2U ?
			0x8000U : 0U);
	return round_bfloat(magnitude, top - static_cast<int>(target), sign,
		fpcr);
}

[[nodiscard]] constexpr bool bfloat_less(const BFloatValue &left,
	const BFloatValue &right) noexcept
{
	const auto left_magnitude = left.bits & 0x7fffU;
	const auto right_magnitude = right.bits & 0x7fffU;
	if ((left_magnitude | right_magnitude) == 0U || left.bits == right.bits)
		return false;
	if (left.sign != right.sign)
		return left.sign;
	return left.sign ? left_magnitude > right_magnitude :
		left_magnitude < right_magnitude;
}

template<AtomicFloatingOperation Operation>
[[nodiscard]] constexpr std::uint16_t minmax_bfloat(
	std::uint16_t left_bits, std::uint16_t right_bits,
	std::uint32_t fpcr) noexcept
{
	fpcr = local_fpcr(fpcr);
	auto left = unpack_bfloat(left_bits, fpcr);
	auto right = unpack_bfloat(right_bits, fpcr);
	constexpr bool number = Operation == AtomicFloatingOperation::maximum_number ||
		Operation == AtomicFloatingOperation::minimum_number;
	constexpr bool maximum = Operation == AtomicFloatingOperation::maximum ||
		Operation == AtomicFloatingOperation::maximum_number;
	if constexpr (number) {
		const auto replacement = static_cast<std::uint16_t>(
			maximum ? 0xff80U : 0x7f80U);
		if (left.type == BFloatClass::quiet_nan &&
		    right.type != BFloatClass::quiet_nan)
			left = unpack_bfloat(replacement, fpcr);
		else if (left.type != BFloatClass::quiet_nan &&
			 right.type == BFloatClass::quiet_nan)
			right = unpack_bfloat(replacement, fpcr);
	}
	if (is_nan(left.type) || is_nan(right.type))
		return 0x7fc0U;
	const auto left_magnitude = left.bits & 0x7fffU;
	const auto right_magnitude = right.bits & 0x7fffU;
	if ((left_magnitude | right_magnitude) == 0U)
		return static_cast<std::uint16_t>(maximum ?
			(left.sign && right.sign ? 0x8000U : 0U) :
			(left.sign || right.sign ? 0x8000U : 0U));
	const bool less = bfloat_less(left, right);
	if constexpr (maximum)
		return less ? right.bits : left.bits;
	else
		return less ? left.bits : right.bits;
}

template<typename Floating, AtomicFloatingOperation Operation>
[[nodiscard]] Floating native_operation(Floating left, Floating right) noexcept
{
	Floating result{};
#define UML_ATOMIC_FLOATING_ASM(suffix, instruction) \
	__asm__ volatile(#instruction " %" #suffix "0, %" #suffix "1, %" \
		#suffix "2" : "=w"(result) : "w"(left), "w"(right))
#define UML_ATOMIC_FLOATING_SELECT(suffix) \
	if constexpr (Operation == AtomicFloatingOperation::add) \
		UML_ATOMIC_FLOATING_ASM(suffix, fadd); \
	else if constexpr (Operation == AtomicFloatingOperation::maximum) \
		UML_ATOMIC_FLOATING_ASM(suffix, fmax); \
	else if constexpr (Operation == AtomicFloatingOperation::maximum_number) \
		UML_ATOMIC_FLOATING_ASM(suffix, fmaxnm); \
	else if constexpr (Operation == AtomicFloatingOperation::minimum) \
		UML_ATOMIC_FLOATING_ASM(suffix, fmin); \
	else \
		UML_ATOMIC_FLOATING_ASM(suffix, fminnm)
	if constexpr (sizeof(Floating) == 2)
		UML_ATOMIC_FLOATING_SELECT(h);
	else if constexpr (sizeof(Floating) == 4)
		UML_ATOMIC_FLOATING_SELECT(s);
	else
		UML_ATOMIC_FLOATING_SELECT(d);
#undef UML_ATOMIC_FLOATING_SELECT
#undef UML_ATOMIC_FLOATING_ASM
	return result;
}

template<typename Floating, AtomicFloatingOperation Operation>
[[nodiscard]] auto controlled_native(std::uint64_t left_bits,
	std::uint64_t right_bits, std::uint32_t fpcr) noexcept
{
	using Integer = std::conditional_t<sizeof(Floating) == 2, std::uint16_t,
		std::conditional_t<sizeof(Floating) == 4, std::uint32_t,
			std::uint64_t>>;
	std::uint64_t saved_fpcr{};
	std::uint64_t saved_fpsr{};
	__asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr" :
		"=r"(saved_fpcr), "=r"(saved_fpsr) : : "memory");
	const std::uint64_t selected_fpcr = local_fpcr(fpcr);
	__asm__ volatile("msr fpcr, %0\n\tmsr fpsr, xzr" : :
		"r"(selected_fpcr) : "memory");
	const auto left = std::bit_cast<Floating>(static_cast<Integer>(left_bits));
	const auto right = std::bit_cast<Floating>(static_cast<Integer>(right_bits));
	const auto result = native_operation<Floating, Operation>(left, right);
	__asm__ volatile("msr fpcr, %0\n\tmsr fpsr, %1" : :
		"r"(saved_fpcr), "r"(saved_fpsr) : "memory");
	return std::bit_cast<Integer>(result);
}

} // namespace atomic_floating_detail

template<AtomicFloatingOperation Operation>
[[nodiscard]] inline std::uint64_t atomic_floating_result(
	std::uint64_t observed, std::uint64_t operand, unsigned width,
	bool bfloat, std::uint32_t fpcr) noexcept
{
	using namespace atomic_floating_detail;
	if (bfloat) {
		const auto left = static_cast<std::uint16_t>(observed);
		const auto right = static_cast<std::uint16_t>(operand);
		if constexpr (Operation == AtomicFloatingOperation::add)
			return add_bfloat(left, right, fpcr);
		else
			return minmax_bfloat<Operation>(left, right, fpcr);
	}
	if (width == 16U)
		return controlled_native<_Float16, Operation>(observed, operand, fpcr);
	if (width == 32U)
		return controlled_native<float, Operation>(observed, operand, fpcr);
	return controlled_native<double, Operation>(observed, operand, fpcr);
}

[[nodiscard]] inline std::uint64_t atomic_floating_result(
	AtomicFloatingOperation operation, std::uint64_t observed,
	std::uint64_t operand, unsigned width, bool bfloat,
	std::uint32_t fpcr) noexcept
{
	switch (operation) {
	case AtomicFloatingOperation::add:
		return atomic_floating_result<AtomicFloatingOperation::add>(
			observed, operand, width, bfloat, fpcr);
	case AtomicFloatingOperation::maximum:
		return atomic_floating_result<AtomicFloatingOperation::maximum>(
			observed, operand, width, bfloat, fpcr);
	case AtomicFloatingOperation::maximum_number:
		return atomic_floating_result<AtomicFloatingOperation::maximum_number>(
			observed, operand, width, bfloat, fpcr);
	case AtomicFloatingOperation::minimum:
		return atomic_floating_result<AtomicFloatingOperation::minimum>(
			observed, operand, width, bfloat, fpcr);
	case AtomicFloatingOperation::minimum_number:
		return atomic_floating_result<AtomicFloatingOperation::minimum_number>(
			observed, operand, width, bfloat, fpcr);
	}
	/* Decoders construct only the five enumerators above. Preserve memory if a
	 * corrupted internal descriptor ever crosses this helper boundary. */
	return observed;
}

} // namespace uml::cpu::arm64::detail
