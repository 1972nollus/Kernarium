// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu::arm64::detail {

enum class FloatingHorizontalOperation : unsigned {
	absolute_difference,
	add,
	maximum,
	maximum_number,
	minimum,
	minimum_number,
	absolute_maximum,
	absolute_minimum,
};

template<typename Integer>
struct FloatingAbsoluteMinMaxResult final {
	Integer bits;
	std::uint32_t exceptions;
};

template<typename Integer>
[[nodiscard]] constexpr FloatingAbsoluteMinMaxResult<Integer>
floating_absolute_minmax(Integer first, Integer second, bool maximum,
	bool default_nan) noexcept
{
	constexpr unsigned bits = sizeof(Integer) * 8U;
	constexpr unsigned fraction_bits = sizeof(Integer) == 2 ? 10U :
		sizeof(Integer) == 4 ? 23U : 52U;
	constexpr Integer fraction_mask =
		static_cast<Integer>((Integer{1} << fraction_bits) - 1U);
	constexpr Integer exponent_mask = static_cast<Integer>(
		((Integer{1} << (bits - fraction_bits - 1U)) - 1U) <<
		fraction_bits);
	constexpr Integer quiet_bit = Integer{1} << (fraction_bits - 1U);
	constexpr Integer magnitude_mask = static_cast<Integer>(
		(Integer{1} << (bits - 1U)) - 1U);
	const auto is_nan = [](Integer value) {
		return (value & exponent_mask) == exponent_mask &&
			(value & fraction_mask) != 0U;
	};
	const auto is_signaling = [&](Integer value) {
		return is_nan(value) && (value & quiet_bit) == 0U;
	};
	const bool first_signaling = is_signaling(first);
	const bool second_signaling = is_signaling(second);
	Integer selected{};
	if (first_signaling)
		selected = first;
	else if (second_signaling)
		selected = second;
	else if (is_nan(first))
		selected = first;
	else if (is_nan(second))
		selected = second;
	else {
		const Integer first_absolute = first & magnitude_mask;
		const Integer second_absolute = second & magnitude_mask;
		return {maximum ?
			(first_absolute > second_absolute ? first_absolute :
				second_absolute) :
			(first_absolute < second_absolute ? first_absolute :
				second_absolute), 0U};
	}
	return {default_nan ? static_cast<Integer>(exponent_mask | quiet_bit) :
		static_cast<Integer>(selected | quiet_bit),
		(first_signaling || second_signaling) ? 1U : 0U};
}

template<typename Integer>
[[nodiscard]] constexpr FloatingAbsoluteMinMaxResult<Integer>
floating_alternate_minmax(Integer first, Integer second, bool maximum,
	std::uint32_t fpcr, bool number = false) noexcept
{
	constexpr unsigned bits = sizeof(Integer) * 8U;
	constexpr unsigned fraction_bits = sizeof(Integer) == 2 ? 10U :
		sizeof(Integer) == 4 ? 23U : 52U;
	constexpr Integer sign = Integer{1} << (bits - 1U);
	constexpr Integer fraction_mask =
		static_cast<Integer>((Integer{1} << fraction_bits) - 1U);
	constexpr Integer exponent_mask = static_cast<Integer>(
		((Integer{1} << (bits - fraction_bits - 1U)) - 1U) <<
		fraction_bits);
	constexpr Integer quiet_bit = Integer{1} << (fraction_bits - 1U);
	constexpr Integer magnitude_mask = static_cast<Integer>(sign - 1U);
	const auto is_nan = [](Integer value) {
		return (value & exponent_mask) == exponent_mask &&
			(value & fraction_mask) != 0U;
	};
	const auto is_subnormal = [](Integer value) {
		return (value & exponent_mask) == 0U &&
			(value & fraction_mask) != 0U;
	};
	const bool alternate = (fpcr & 2U) != 0U;
	std::uint32_t exceptions{};
	const auto flush_input = [&](Integer value) {
		if (!is_subnormal(value))
			return value;
		if constexpr (sizeof(Integer) == 2U) {
			if ((fpcr & (1U << 19)) == 0U)
				return value;
		} else {
			const bool flush_to_zero =
				!alternate && (fpcr & (1U << 24)) != 0U;
			const bool flush_input_to_zero = (fpcr & 1U) != 0U;
			if (!flush_to_zero && !flush_input_to_zero)
				return value;
			if (flush_to_zero)
				exceptions |= 1U << 7;
		}
		return static_cast<Integer>(value & sign);
	};
	first = flush_input(first);
	second = flush_input(second);
	const auto is_zero = [](Integer value) {
		return (value & magnitude_mask) == 0U;
	};
	const auto is_signaling = [&](Integer value) {
		return is_nan(value) && (value & quiet_bit) == 0U;
	};
	if (number) {
		if (alternate && is_nan(first) && is_nan(second)) {
			const bool invalid =
				is_signaling(first) || is_signaling(second);
			const Integer selected = (fpcr & (1U << 25)) != 0U ?
				static_cast<Integer>(exponent_mask | quiet_bit) :
				static_cast<Integer>(first | quiet_bit);
			return {selected, static_cast<std::uint32_t>(
				exceptions | (invalid ? 1U : 0U))};
		}
		const bool first_qnan = is_nan(first) && !is_signaling(first);
		const bool second_qnan = is_nan(second) && !is_signaling(second);
		const Integer replacement = static_cast<Integer>(
			exponent_mask | (maximum ? sign : Integer{}));
		if (first_qnan && !second_qnan)
			first = replacement;
		else if (!first_qnan && second_qnan)
			second = replacement;
	}
	if (!number && alternate && (is_nan(first) || is_nan(second))) {
		return {second, static_cast<std::uint32_t>(exceptions | 1U)};
	}
	if (is_nan(first) || is_nan(second)) {
		const bool invalid =
			is_signaling(first) || is_signaling(second);
		Integer selected = is_signaling(first) ? first :
			is_signaling(second) ? second :
			is_nan(first) ? first : second;
		selected = (fpcr & (1U << 25)) != 0U ?
			static_cast<Integer>(exponent_mask | quiet_bit) :
			static_cast<Integer>(selected | quiet_bit);
		return {selected, static_cast<std::uint32_t>(
			exceptions | (invalid ? 1U : 0U))};
	}

	Integer selected;
	if (is_zero(first) && is_zero(second)) {
		if (number || !alternate) {
			selected = maximum ?
				static_cast<Integer>(first & second & sign) :
				static_cast<Integer>((first | second) & sign);
		} else {
			selected = second;
		}
	} else {
		const bool first_negative = (first & sign) != 0U;
		const bool second_negative = (second & sign) != 0U;
		bool first_less;
		if (first_negative != second_negative)
			first_less = first_negative;
		else {
			const Integer first_magnitude = first & magnitude_mask;
			const Integer second_magnitude = second & magnitude_mask;
			first_less = first_negative ?
				first_magnitude > second_magnitude :
				first_magnitude < second_magnitude;
		}
		const bool choose_first = maximum ? !first_less &&
			first != second : first_less;
		selected = choose_first ? first : second;
	}
	if (alternate && sizeof(Integer) != 2U &&
	    (is_subnormal(first) || is_subnormal(second)))
		exceptions |= 1U << 7;
	if (number && alternate && is_subnormal(selected)) {
		const bool flush_output = sizeof(Integer) == 2U ?
			(fpcr & (1U << 19)) != 0U :
			(fpcr & (1U << 24)) != 0U;
		if (flush_output) {
			selected &= sign;
			exceptions |= (1U << 3) | (1U << 4);
		}
	}
	return {selected, exceptions};
}

template<typename Integer>
struct FloatingControlledArithmeticResult final {
	bool complete;
	Integer bits;
	Integer first;
	Integer second;
	std::uint32_t exceptions;
};

template<typename Integer>
[[nodiscard]] constexpr FloatingControlledArithmeticResult<Integer>
prepare_floating_controlled_arithmetic(Integer first, Integer second,
	std::uint32_t fpcr, bool absolute_result) noexcept
{
	constexpr unsigned bits = sizeof(Integer) * 8U;
	constexpr unsigned fraction_bits = sizeof(Integer) == 2 ? 10U :
		sizeof(Integer) == 4 ? 23U : 52U;
	constexpr Integer sign = Integer{1} << (bits - 1U);
	constexpr Integer fraction_mask =
		static_cast<Integer>((Integer{1} << fraction_bits) - 1U);
	constexpr Integer exponent_mask = static_cast<Integer>(
		((Integer{1} << (bits - fraction_bits - 1U)) - 1U) <<
		fraction_bits);
	constexpr Integer quiet_bit = Integer{1} << (fraction_bits - 1U);
	const auto is_nan = [](Integer value) {
		return (value & exponent_mask) == exponent_mask &&
			(value & fraction_mask) != 0U;
	};
	const auto is_signaling = [&](Integer value) {
		return is_nan(value) && (value & quiet_bit) == 0U;
	};
	const bool first_nan = is_nan(first);
	const bool second_nan = is_nan(second);
	if (first_nan || second_nan) {
		const bool alternate = (fpcr & 2U) != 0U;
		Integer selected;
		if (alternate && first_nan && second_nan)
			selected = first;
		else if (is_signaling(first))
			selected = first;
		else if (is_signaling(second))
			selected = second;
		else
			selected = first_nan ? first : second;
		selected = (fpcr & (1U << 25)) != 0U ?
			static_cast<Integer>(exponent_mask | quiet_bit) :
			static_cast<Integer>(selected | quiet_bit);
		if (absolute_result && !alternate)
			selected &= static_cast<Integer>(sign - 1U);
		return {true, selected, first, second,
			(is_signaling(first) || is_signaling(second)) ? 1U : 0U};
	}

	std::uint32_t exceptions{};
	const auto is_subnormal = [](Integer value) {
		return (value & exponent_mask) == 0U &&
			(value & fraction_mask) != 0U;
	};
	const bool first_subnormal = is_subnormal(first);
	const bool second_subnormal = is_subnormal(second);
	if constexpr (sizeof(Integer) == 2U) {
		if ((fpcr & (1U << 19)) != 0U) {
			if (first_subnormal)
				first &= sign;
			if (second_subnormal)
				second &= sign;
		}
	} else {
		if ((fpcr & 1U) != 0U) {
			if (first_subnormal)
				first &= sign;
			if (second_subnormal)
				second &= sign;
		} else if ((fpcr & 2U) != 0U &&
			   (first_subnormal || second_subnormal)) {
			exceptions |= 1U << 7;
		}
	}
	return {false, 0U, first, second, exceptions};
}

template<typename Integer>
[[nodiscard]] constexpr FloatingAbsoluteMinMaxResult<Integer>
complete_floating_controlled_arithmetic(Integer result, std::uint32_t fpcr,
	std::uint32_t exceptions) noexcept
{
	if ((fpcr & 2U) == 0U)
		return {result, exceptions};
	constexpr unsigned bits = sizeof(Integer) * 8U;
	constexpr unsigned fraction_bits = sizeof(Integer) == 2 ? 10U :
		sizeof(Integer) == 4 ? 23U : 52U;
	constexpr Integer sign = Integer{1} << (bits - 1U);
	constexpr Integer fraction_mask =
		static_cast<Integer>((Integer{1} << fraction_bits) - 1U);
	constexpr Integer exponent_mask = static_cast<Integer>(
		((Integer{1} << (bits - fraction_bits - 1U)) - 1U) <<
		fraction_bits);
	const bool subnormal = (result & exponent_mask) == 0U &&
		(result & fraction_mask) != 0U;
	if (subnormal) {
		const bool flush = sizeof(Integer) == 2U ?
			(fpcr & (1U << 19)) != 0U :
			(fpcr & (1U << 24)) != 0U;
		if (flush)
			return {static_cast<Integer>(result & sign),
				static_cast<std::uint32_t>(
					exceptions | (1U << 3) | (1U << 4))};
	} else if ((result & exponent_mask) != 0U) {
		/* Alternative mode detects tininess after unconstrained rounding.
		 * A legacy host can therefore report underflow for a value that
		 * rounded back into the normal range. */
		exceptions &= ~(1U << 3);
	}
	return {result, exceptions};
}

enum class FloatingHorizontalForm : unsigned {
	scalar_binary,
	vector_binary,
	scalar_pairwise,
	vector_pairwise,
	reduction,
};

struct FloatingHorizontal final {
	bool recognized{};
	bool valid{};
	FloatingHorizontalOperation operation{};
	FloatingHorizontalForm form{};
	unsigned width{};
	unsigned vector_bytes{};
};

[[nodiscard]] constexpr unsigned floating_width_from_type(unsigned type)
	noexcept
{
	return type == 3U ? 16U : type == 0U ? 32U : 64U;
}

[[nodiscard]] constexpr FloatingHorizontal decode_floating_horizontal(
	std::uint32_t instruction) noexcept
{
	/* Scalar FABD, including its separate FP16 allocation. */
	if ((instruction & 0xffe0'fc00U) == 0x7ec0'1400U)
		return {true, true, FloatingHorizontalOperation::absolute_difference,
			FloatingHorizontalForm::scalar_binary, 16U, 16U};
	if ((instruction & 0xff20'fc00U) == 0x7e20'd400U)
		return {true, (instruction & 0x0080'0000U) != 0U,
			FloatingHorizontalOperation::absolute_difference,
			FloatingHorizontalForm::scalar_binary,
			(instruction & 0x0040'0000U) != 0U ? 64U : 32U, 16U};

	/* Scalar FMAX/FMAXNM/FMIN/FMINNM. ftype == 10 is reserved. */
	FloatingHorizontalOperation operation;
	bool matched = true;
	switch (instruction & 0xff20'fc00U) {
	case 0x1e20'4800U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x1e20'6800U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x1e20'5800U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x1e20'7800U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	default: matched = false; break;
	}
	if (matched) {
		const unsigned type = (instruction >> 22) & 3U;
		return {true, type != 2U, operation,
			FloatingHorizontalForm::scalar_binary,
			floating_width_from_type(type), 16U};
	}

	const bool q = (instruction & 0x4000'0000U) != 0U;
	/* FEAT_FAMINMAX vector absolute min/max. These instructions deliberately
	 * ignore AH and all input/output flush controls. */
	matched = true;
	switch (instruction & 0xbfe0'fc00U) {
	case 0x0ec0'1c00U:
		operation = FloatingHorizontalOperation::absolute_maximum; break;
	case 0x2ec0'1c00U:
		operation = FloatingHorizontalOperation::absolute_minimum; break;
	default: matched = false; break;
	}
	if (matched)
		return {true, true, operation,
			FloatingHorizontalForm::vector_binary, 16U, q ? 16U : 8U};

	matched = true;
	switch (instruction & 0xbfa0'fc00U) {
	case 0x0ea0'dc00U:
		operation = FloatingHorizontalOperation::absolute_maximum; break;
	case 0x2ea0'dc00U:
		operation = FloatingHorizontalOperation::absolute_minimum; break;
	default: matched = false; break;
	}
	if (matched) {
		const bool is_double = (instruction & 0x0040'0000U) != 0U;
		return {true, !is_double || q, operation,
			FloatingHorizontalForm::vector_binary,
			is_double ? 64U : 32U, q ? 16U : 8U};
	}
	/* Vector FP16 FABD/FADDP and min/max families. */
	matched = true;
	switch (instruction & 0xbfe0'fc00U) {
	case 0x2ec0'1400U:
		operation = FloatingHorizontalOperation::absolute_difference; break;
	case 0x2e40'1400U: operation = FloatingHorizontalOperation::add; break;
	case 0x0e40'3400U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x2e40'3400U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x0e40'0400U:
	case 0x2e40'0400U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x0ec0'3400U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x2ec0'3400U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x0ec0'0400U:
	case 0x2ec0'0400U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	default: matched = false; break;
	}
	if (matched) {
		const bool pairwise = operation == FloatingHorizontalOperation::add ||
			(operation != FloatingHorizontalOperation::absolute_difference &&
			 (instruction & 0x2000'0000U) != 0U);
		return {true, true, operation, pairwise ?
			FloatingHorizontalForm::vector_pairwise :
			FloatingHorizontalForm::vector_binary,
			16U, q ? 16U : 8U};
	}

	/* Vector FP32/FP64 forms. The .1D allocation is reserved. */
	matched = true;
	switch (instruction & 0xbfa0'fc00U) {
	case 0x2ea0'd400U:
		operation = FloatingHorizontalOperation::absolute_difference; break;
	case 0x2e20'd400U: operation = FloatingHorizontalOperation::add; break;
	case 0x0e20'f400U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x0e20'c400U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x0ea0'f400U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x0ea0'c400U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	case 0x2e20'f400U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x2e20'c400U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x2ea0'f400U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x2ea0'c400U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	default: matched = false; break;
	}
	if (matched) {
		const bool pairwise = (instruction & 0x2000'0000U) != 0U &&
			operation != FloatingHorizontalOperation::absolute_difference;
		const bool is_double = (instruction & 0x0040'0000U) != 0U;
		return {true, !is_double || q, operation, pairwise ?
			FloatingHorizontalForm::vector_pairwise :
			FloatingHorizontalForm::vector_binary,
			is_double ? 64U : 32U, q ? 16U : 8U};
	}

	/* Scalar pairwise forms. The FP16 sz == 1 allocation is reserved. */
	matched = true;
	switch (instruction & 0xffbf'fc00U) {
	case 0x5e30'd800U:
	case 0x7e30'd800U: operation = FloatingHorizontalOperation::add; break;
	case 0x5e30'f800U:
	case 0x7e30'f800U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x5e30'c800U:
	case 0x7e30'c800U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x5eb0'f800U:
	case 0x7eb0'f800U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x5eb0'c800U:
	case 0x7eb0'c800U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	default: matched = false; break;
	}
	if (matched) {
		const bool half = (instruction & 0x2000'0000U) == 0U;
		const bool size = (instruction & 0x0040'0000U) != 0U;
		return {true, !half || !size, operation,
			FloatingHorizontalForm::scalar_pairwise,
			half ? 16U : size ? 64U : 32U, 16U};
	}

	/* Across-vector reductions: FP16 accepts 4H/8H; FP32 requires 4S. */
	matched = true;
	switch (instruction & 0xbfff'fc00U) {
	case 0x0e30'f800U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x0e30'c800U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x0eb0'f800U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x0eb0'c800U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	default: matched = false; break;
	}
	if (matched)
		return {true, true, operation, FloatingHorizontalForm::reduction,
			16U, q ? 16U : 8U};

	matched = true;
	switch (instruction & 0xbfbf'fc00U) {
	case 0x2e30'f800U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x2e30'c800U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x2eb0'f800U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x2eb0'c800U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	default: matched = false; break;
	}
	if (matched) {
		const bool valid = q && (instruction & 0x0040'0000U) == 0U;
		return {true, valid, operation, FloatingHorizontalForm::reduction,
			32U, 16U};
	}
	return {};
}

[[nodiscard]] constexpr bool floating_horizontal_uses_ah(
	const FloatingHorizontal &decoded) noexcept
{
	return decoded.operation == FloatingHorizontalOperation::maximum ||
		decoded.operation == FloatingHorizontalOperation::maximum_number ||
		decoded.operation == FloatingHorizontalOperation::minimum ||
		decoded.operation == FloatingHorizontalOperation::minimum_number;
}

[[nodiscard]] constexpr std::uintptr_t floating_horizontal_operand(
	std::uint32_t instruction, const FloatingHorizontal &decoded) noexcept
{
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
		(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16) |
		(static_cast<std::uintptr_t>(decoded.operation) << 24) |
		(static_cast<std::uintptr_t>(decoded.form) << 28) |
		(static_cast<std::uintptr_t>(decoded.width == 32U ? 1U :
			decoded.width == 64U ? 2U : 0U) << 32) |
		(static_cast<std::uintptr_t>(decoded.vector_bytes == 16U) << 34);
}

} // namespace uml::cpu::arm64::detail
