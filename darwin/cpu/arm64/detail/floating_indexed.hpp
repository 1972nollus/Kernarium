// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "floating_bits.hpp"
#include "floating_exception.hpp"

#include <bit>
#include <cstdint>
#include <type_traits>

namespace uml::cpu::arm64::detail {

enum class IndexedFloatingOperation : unsigned {
	add,
	subtract,
	multiply,
	multiply_extended,
};

template<typename Integer>
struct IndexedFloatingResult final {
	Integer bits{};
	std::uint32_t exceptions{};
};

namespace indexed_floating_detail {

using namespace floating_bits;

template<typename Integer>
[[nodiscard]] constexpr IndexedFloatingResult<Integer> process_nan(
	Integer selected, bool any_signaling, std::uint32_t fpcr) noexcept
{
	return {floating_bits::process_nan(selected, fpcr),
		any_signaling ? 1U : 0U};
}

template<typename Integer>
[[nodiscard]] constexpr bool select_binary_nan(Integer left, Integer right,
	Integer &selected, bool &any_signaling) noexcept
{
	const bool left_nan = is_nan(left);
	const bool right_nan = is_nan(right);
	if (!left_nan && !right_nan)
		return false;
	any_signaling = is_signaling_nan(left) || is_signaling_nan(right);
	if (left_nan && right_nan)
		selected = left;
	else if (is_signaling_nan(left))
		selected = left;
	else if (is_signaling_nan(right))
		selected = right;
	else
		selected = left_nan ? left : right;
	return true;
}

template<typename Integer>
[[nodiscard]] constexpr bool select_fused_nan(Integer addend, Integer left,
	Integer right, Integer &selected, bool &any_signaling) noexcept
{
	const bool addend_nan = is_nan(addend);
	const bool left_nan = is_nan(left);
	const bool right_nan = is_nan(right);
	if (!addend_nan && !left_nan && !right_nan)
		return false;
	any_signaling = is_signaling_nan(addend) ||
		is_signaling_nan(left) || is_signaling_nan(right);
	/*
	 * This is the FPProcessNaNs3 alternative ordering.  For FMLS the
	 * original left operand is supplied: FPNeg preserves a NaN's sign when
	 * alternative handling is active.
	 */
	if (left_nan && (addend_nan || right_nan))
		selected = left;
	else if (right_nan && addend_nan)
		selected = right;
	else if (is_signaling_nan(addend))
		selected = addend;
	else if (is_signaling_nan(left))
		selected = left;
	else if (is_signaling_nan(right))
		selected = right;
	else if (addend_nan)
		selected = addend;
	else if (left_nan)
		selected = left;
	else
		selected = right;
	return true;
}

template<typename Floating>
[[nodiscard]] inline Floating native_operation(Floating addend,
	Floating left, Floating right,
	IndexedFloatingOperation operation) noexcept
{
	Floating result;
#define UML_INDEXED_FLOATING_ASM(suffix, instruction) \
	__asm__ volatile(#instruction " %" #suffix "0, %" #suffix "1, %" \
		#suffix "2, %" #suffix "3" : "=w"(result) : "w"(left), \
		"w"(right), "w"(addend))
#define UML_INDEXED_MULTIPLY_ASM(suffix, instruction) \
	__asm__ volatile(#instruction " %" #suffix "0, %" #suffix "1, %" \
		#suffix "2" : "=w"(result) : "w"(left), "w"(right))
#define UML_INDEXED_FLOATING_WIDTH(suffix) \
	do { \
		switch (operation) { \
		case IndexedFloatingOperation::add: \
			UML_INDEXED_FLOATING_ASM(suffix, fmadd); \
			break; \
		case IndexedFloatingOperation::subtract: \
			UML_INDEXED_FLOATING_ASM(suffix, fmsub); \
			break; \
		case IndexedFloatingOperation::multiply: \
			UML_INDEXED_MULTIPLY_ASM(suffix, fmul); \
			break; \
		case IndexedFloatingOperation::multiply_extended: \
			UML_INDEXED_MULTIPLY_ASM(suffix, fmulx); \
			break; \
		} \
	} while (false)
	if constexpr (sizeof(Floating) == 2U)
		UML_INDEXED_FLOATING_WIDTH(h);
	else if constexpr (sizeof(Floating) == 4U)
		UML_INDEXED_FLOATING_WIDTH(s);
	else
		UML_INDEXED_FLOATING_WIDTH(d);
#undef UML_INDEXED_FLOATING_WIDTH
#undef UML_INDEXED_MULTIPLY_ASM
#undef UML_INDEXED_FLOATING_ASM
	return result;
}

template<typename Floating, typename Integer>
[[nodiscard]] inline IndexedFloatingResult<Integer> captured_operation(
	Integer addend, Integer left, Integer right,
	IndexedFloatingOperation operation, std::uint32_t fpcr) noexcept
{
	const auto captured = run_with_local_fpcr(host_fpcr(fpcr), [&] {
		return native_operation(std::bit_cast<Floating>(addend),
			std::bit_cast<Floating>(left),
			std::bit_cast<Floating>(right), operation);
	});
	return {std::bit_cast<Integer>(captured.value), captured.fpsr};
}

} // namespace indexed_floating_detail

/*
 * Execute one lane whose AFP controls need software handling.  Hosts which
 * implement the feature use the native instruction directly; this path keeps
 * the checked gadgets exact on older hosts.
 */
template<typename Floating>
[[nodiscard]] inline IndexedFloatingResult<
	std::conditional_t<sizeof(Floating) == 2U, std::uint16_t,
	std::conditional_t<sizeof(Floating) == 4U, std::uint32_t, std::uint64_t>>>
indexed_floating_software(Floating addend_value, Floating left_value,
	Floating right_value, IndexedFloatingOperation operation,
	std::uint32_t fpcr) noexcept
{
	using Integer = std::conditional_t<sizeof(Floating) == 2U,
		std::uint16_t, std::conditional_t<sizeof(Floating) == 4U,
			std::uint32_t, std::uint64_t>>;
	using namespace indexed_floating_detail;
	Integer addend = std::bit_cast<Integer>(addend_value);
	Integer left = std::bit_cast<Integer>(left_value);
	Integer right = std::bit_cast<Integer>(right_value);

	const bool half = sizeof(Floating) == 2U;
	const bool alternative = (fpcr & 2U) != 0U;
	if (!alternative) {
		/* FIZ is ignored for binary16 and silently flushes binary32/64
		 * inputs even when AH is clear. */
		if (!half && (fpcr & 1U) != 0U) {
			addend = flush_subnormal(addend);
			left = flush_subnormal(left);
			right = flush_subnormal(right);
		}
		return captured_operation<Floating, Integer>(addend, left, right,
			operation, fpcr & ~1U);
	}
	const bool flush_inputs = half ?
		(fpcr & (1U << 19)) != 0U : (fpcr & 1U) != 0U;
	const bool flush_output = half ?
		(fpcr & (1U << 19)) != 0U : (fpcr & (1U << 24)) != 0U;
	if (flush_inputs) {
		addend = flush_subnormal(addend);
		left = flush_subnormal(left);
		right = flush_subnormal(right);
	}

	Integer selected{};
	bool any_signaling{};
	const bool fused = operation == IndexedFloatingOperation::add ||
		operation == IndexedFloatingOperation::subtract;
	const bool has_nan = fused ?
		select_fused_nan(addend, left, right, selected, any_signaling) :
		select_binary_nan(left, right, selected, any_signaling);
	if (has_nan)
		return process_nan(selected, any_signaling, fpcr);

	const bool left_infinity = is_infinity(left);
	const bool right_infinity = is_infinity(right);
	const bool left_zero = is_zero(left);
	const bool right_zero = is_zero(right);
	if (operation != IndexedFloatingOperation::multiply_extended &&
	    ((left_infinity && right_zero) ||
	     (left_zero && right_infinity)))
		return {default_nan<Integer>(fpcr), 1U};
	if (fused) {
		const bool addend_infinity = is_infinity(addend);
		const bool product_infinity = left_infinity || right_infinity;
		const bool product_sign =
			(((left ^ right) & sign_bit<Integer>) != 0U) ^
			(operation == IndexedFloatingOperation::subtract);
		const bool addend_sign = (addend & sign_bit<Integer>) != 0U;
		if (addend_infinity && product_infinity &&
		    addend_sign != product_sign)
			return {default_nan<Integer>(fpcr), 1U};
	}

	std::uint32_t local_fpcr = fpcr & ~(2U | 1U);
	if (half)
		local_fpcr &= ~(1U << 19);
	else
		local_fpcr &= ~(1U << 24);
	auto result = captured_operation<Floating, Integer>(addend, left, right,
		operation, local_fpcr);
	/*
	 * AH detects tininess after rounding.  Clear an ordinary host's
	 * before-rounding underflow indication when rounding produced a normal
	 * value, then apply architectural output flushing to a rounded
	 * subnormal.
	 */
	if (!is_subnormal(result.bits) && !is_zero(result.bits))
		result.exceptions &= ~(1U << 3);
	if (flush_output && is_subnormal(result.bits)) {
		result.bits &= sign_bit<Integer>;
		result.exceptions |= (1U << 3) | (1U << 4);
	}
	return result;
}

} // namespace uml::cpu::arm64::detail
