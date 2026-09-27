// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "floating_exception.hpp"
#include "floating_reciprocal.hpp"
#include "instruction_primitives.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace uml::cpu::arm64::detail {

struct FloatingReciprocalLaneResult final {
	std::uint64_t bits{};
	std::uint32_t exceptions{};
};

namespace floating_reciprocal_software_detail {

struct Format final {
	unsigned width;
	unsigned exponent_bits;
	unsigned fraction_bits;
	int bias;
};

enum class ValueClass {
	zero,
	finite,
	infinity,
	quiet_nan,
	signaling_nan,
};

struct Unpacked final {
	ValueClass type;
	bool negative;
	std::uint64_t raw;
};

inline constexpr std::uint32_t fpcr_fiz = 1U << 0;
inline constexpr std::uint32_t fpcr_ah = 1U << 1;
inline constexpr std::uint32_t fpcr_nep = 1U << 2;
inline constexpr std::uint32_t fpcr_fz16 = 1U << 19;
inline constexpr std::uint32_t fpcr_fz = 1U << 24;
inline constexpr std::uint32_t fpcr_dn = 1U << 25;
inline constexpr std::uint32_t fpcr_rounding = 3U << 22;
inline constexpr std::uint32_t fpcr_exception_traps = 0x0000'9f00U;

[[nodiscard]] constexpr Format format(unsigned width) noexcept
{
	return width == 16U ? Format{16U, 5U, 10U, 15} :
		width == 32U ? Format{32U, 8U, 23U, 127} :
			Format{64U, 11U, 52U, 1023};
}

[[nodiscard]] constexpr std::uint64_t low_mask(unsigned bits) noexcept
{
	return bits == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] constexpr std::uint64_t sign_mask(
	const Format &value) noexcept
{
	return std::uint64_t{1} << (value.width - 1U);
}

[[nodiscard]] constexpr std::uint64_t fraction_mask(
	const Format &value) noexcept
{
	return low_mask(value.fraction_bits);
}

[[nodiscard]] constexpr std::uint64_t maximum_exponent(
	const Format &value) noexcept
{
	return low_mask(value.exponent_bits);
}

[[nodiscard]] constexpr std::uint64_t exponent_mask(
	const Format &value) noexcept
{
	return maximum_exponent(value) << value.fraction_bits;
}

[[nodiscard]] constexpr std::uint64_t infinity(
	const Format &value, bool negative = false) noexcept
{
	return (negative ? sign_mask(value) : 0U) | exponent_mask(value);
}

[[nodiscard]] constexpr std::uint64_t normal(const Format &value,
	unsigned exponent, std::uint64_t fraction,
	bool negative = false) noexcept
{
	return (negative ? sign_mask(value) : 0U) |
		(static_cast<std::uint64_t>(exponent) << value.fraction_bits) |
		(fraction & fraction_mask(value));
}

[[nodiscard]] constexpr std::uint64_t default_nan(
	const Format &value, std::uint32_t fpcr) noexcept
{
	return ((fpcr & fpcr_ah) != 0U ? sign_mask(value) : 0U) |
		exponent_mask(value) |
		(std::uint64_t{1} << (value.fraction_bits - 1U));
}

[[nodiscard]] constexpr Unpacked unpack(std::uint64_t raw,
	const Format &value) noexcept
{
	raw &= low_mask(value.width);
	const bool negative = (raw & sign_mask(value)) != 0U;
	const auto exponent =
		(raw & exponent_mask(value)) >> value.fraction_bits;
	const auto fraction = raw & fraction_mask(value);
	if (exponent == maximum_exponent(value)) {
		if (fraction == 0U)
			return {ValueClass::infinity, negative, raw};
		const auto quiet =
			std::uint64_t{1} << (value.fraction_bits - 1U);
		return {(fraction & quiet) != 0U ?
				ValueClass::quiet_nan : ValueClass::signaling_nan,
			negative, raw};
	}
	if (exponent == 0U && fraction == 0U)
		return {ValueClass::zero, negative, raw};
	return {ValueClass::finite, negative, raw};
}

[[nodiscard]] constexpr bool is_nan(ValueClass type) noexcept
{
	return type == ValueClass::quiet_nan ||
		type == ValueClass::signaling_nan;
}

[[nodiscard]] constexpr bool is_subnormal(std::uint64_t raw,
	const Format &value) noexcept
{
	return (raw & exponent_mask(value)) == 0U &&
		(raw & fraction_mask(value)) != 0U;
}

[[nodiscard]] constexpr std::uint64_t flush_subnormal(std::uint64_t raw,
	const Format &value) noexcept
{
	return is_subnormal(raw, value) ? raw & sign_mask(value) : raw;
}

[[nodiscard]] constexpr std::uint64_t quiet_nan(std::uint64_t raw,
	const Format &value, std::uint32_t fpcr) noexcept
{
	return (fpcr & fpcr_dn) != 0U ? default_nan(value, fpcr) :
		raw | (std::uint64_t{1} << (value.fraction_bits - 1U));
}

template<typename Floating>
using FloatingInteger = std::conditional_t<sizeof(Floating) == 2U,
	std::uint16_t, std::conditional_t<sizeof(Floating) == 4U,
		std::uint32_t, std::uint64_t>>;

template<typename Floating>
[[nodiscard]] inline Floating native_operation(
	FloatingReciprocalOperation operation, Floating first,
	Floating second) noexcept
{
	Floating result{};
#define UML_RECIPROCAL_SOFTWARE_UNARY(suffix, instruction) \
	__asm__ volatile(#instruction " %" #suffix "0, %" #suffix "1" : \
		"=w"(result) : "w"(first))
#define UML_RECIPROCAL_SOFTWARE_BINARY(suffix, instruction) \
	__asm__ volatile(#instruction " %" #suffix "0, %" #suffix "1, %" \
		#suffix "2" : "=w"(result) : "w"(first), "w"(second))
#define UML_RECIPROCAL_SOFTWARE_SWITCH(suffix) \
	switch (operation) { \
	case FloatingReciprocalOperation::reciprocal_estimate: \
		UML_RECIPROCAL_SOFTWARE_UNARY(suffix, frecpe); break; \
	case FloatingReciprocalOperation::reciprocal_step: \
		UML_RECIPROCAL_SOFTWARE_BINARY(suffix, frecps); break; \
	case FloatingReciprocalOperation::reciprocal_exponent: \
		UML_RECIPROCAL_SOFTWARE_UNARY(suffix, frecpx); break; \
	case FloatingReciprocalOperation::reciprocal_sqrt_estimate: \
		UML_RECIPROCAL_SOFTWARE_UNARY(suffix, frsqrte); break; \
	case FloatingReciprocalOperation::reciprocal_sqrt_step: \
		UML_RECIPROCAL_SOFTWARE_BINARY(suffix, frsqrts); break; \
	}
	if constexpr (sizeof(Floating) == 2U)
		UML_RECIPROCAL_SOFTWARE_SWITCH(h)
	else if constexpr (sizeof(Floating) == 4U)
		UML_RECIPROCAL_SOFTWARE_SWITCH(s)
	else
		UML_RECIPROCAL_SOFTWARE_SWITCH(d)
#undef UML_RECIPROCAL_SOFTWARE_SWITCH
#undef UML_RECIPROCAL_SOFTWARE_BINARY
#undef UML_RECIPROCAL_SOFTWARE_UNARY
	return result;
}

template<typename Floating>
[[nodiscard]] inline FloatingReciprocalLaneResult captured_operation(
	FloatingReciprocalOperation operation, std::uint64_t first,
	std::uint64_t second, std::uint32_t fpcr) noexcept
{
	using Integer = FloatingInteger<Floating>;
	const auto captured =
		run_with_local_fpcr(host_fpcr(fpcr) & ~fpcr_nep, [&] {
			return native_operation(operation,
				std::bit_cast<Floating>(static_cast<Integer>(first)),
				std::bit_cast<Floating>(
					static_cast<Integer>(second)));
		});
	return {std::bit_cast<Integer>(captured.value), captured.fpsr};
}

[[nodiscard]] inline FloatingReciprocalLaneResult captured_operation(
	FloatingReciprocalOperation operation, std::uint64_t first,
	std::uint64_t second, unsigned width, std::uint32_t fpcr) noexcept
{
	if (width == 16U)
		return captured_operation<_Float16>(
			operation, first, second, fpcr);
	if (width == 32U)
		return captured_operation<float>(
			operation, first, second, fpcr);
	return captured_operation<double>(operation, first, second, fpcr);
}

[[nodiscard]] constexpr std::uint64_t select_alternative_nan(
	std::uint64_t first, std::uint64_t second, const Format &value,
	bool unary, std::uint32_t fpcr) noexcept
{
	const auto left = unpack(first, value);
	if (unary)
		return quiet_nan(first, value, fpcr);
	const auto right = unpack(second, value);
	if (is_nan(left.type) && is_nan(right.type))
		return quiet_nan(first, value, fpcr);
	if (is_nan(left.type))
		return quiet_nan(first, value, fpcr);
	return quiet_nan(second, value, fpcr);
}

[[nodiscard]] constexpr std::uint32_t alternative_local_fpcr(
	std::uint32_t fpcr) noexcept
{
	return fpcr & ~(fpcr_fiz | fpcr_ah | fpcr_nep | fpcr_fz16 |
		fpcr_fz | fpcr_rounding | fpcr_exception_traps);
}

[[nodiscard]] constexpr std::uint64_t flush_output(std::uint64_t bits,
	const Format &value, bool enabled) noexcept
{
	return enabled && is_subnormal(bits, value) ?
		bits & sign_mask(value) : bits;
}

} // namespace floating_reciprocal_software_detail

[[nodiscard]] inline FloatingReciprocalLaneResult
floating_reciprocal_software_lane(FloatingReciprocalOperation operation,
	bool unary, std::uint64_t first, std::uint64_t second,
	unsigned width, std::uint32_t fpcr) noexcept
{
	using namespace floating_reciprocal_software_detail;
	const auto value = format(width);
	first &= low_mask(width);
	second &= low_mask(width);
	const bool alternative = (fpcr & fpcr_ah) != 0U;

	if (!alternative) {
		if (width != 16U && (fpcr & fpcr_fiz) != 0U) {
			first = flush_subnormal(first, value);
			if (!unary)
				second = flush_subnormal(second, value);
		}
		return captured_operation(operation, first, second, width,
			fpcr & ~fpcr_fiz);
	}

	const bool flush_inputs = width == 16U ?
		(fpcr & fpcr_fz16) != 0U : true;
	if (flush_inputs) {
		first = flush_subnormal(first, value);
		if (!unary)
			second = flush_subnormal(second, value);
	}
	const auto left = unpack(first, value);
	const auto right = unpack(second, value);
	if (is_nan(left.type) || (!unary && is_nan(right.type)))
		return {select_alternative_nan(
			first, second, value, unary, fpcr), 0U};

	const auto local_fpcr = alternative_local_fpcr(fpcr);
	switch (operation) {
	case FloatingReciprocalOperation::reciprocal_estimate: {
		if (left.type == ValueClass::infinity)
			return {left.negative ? sign_mask(value) : 0U, 0U};
		if (left.type == ValueClass::zero)
			return {infinity(value, left.negative), 0U};
		auto result = captured_operation(
			operation, first, second, width, local_fpcr);
		result.bits = flush_output(result.bits, value,
			width == 16U ? (fpcr & fpcr_fz16) != 0U : true);
		result.exceptions = 0U;
		return result;
	}
	case FloatingReciprocalOperation::reciprocal_exponent: {
		auto result = captured_operation(
			operation, first, second, width, local_fpcr);
		result.exceptions = 0U;
		return result;
	}
	case FloatingReciprocalOperation::reciprocal_sqrt_estimate: {
		if (left.type == ValueClass::zero)
			return {infinity(value, left.negative), 0U};
		if (left.negative)
			return {default_nan(value, fpcr), 0U};
		if (left.type == ValueClass::infinity)
			return {0U, 0U};
		auto result = captured_operation(
			operation, first, second, width, local_fpcr);
		result.exceptions = 0U;
		return result;
	}
	case FloatingReciprocalOperation::reciprocal_step:
	case FloatingReciprocalOperation::reciprocal_sqrt_step: {
		const bool left_infinity = left.type == ValueClass::infinity;
		const bool right_infinity = right.type == ValueClass::infinity;
		const bool left_zero = left.type == ValueClass::zero;
		const bool right_zero = right.type == ValueClass::zero;
		const bool square_root =
			operation ==
				FloatingReciprocalOperation::reciprocal_sqrt_step;
		if ((left_infinity && right_zero) ||
		    (left_zero && right_infinity))
			return {square_root ?
				normal(value, static_cast<unsigned>(value.bias),
					std::uint64_t{1} <<
						(value.fraction_bits - 1U)) :
				normal(value,
					static_cast<unsigned>(value.bias + 1), 0U),
				0U};
		if (left_infinity || right_infinity) {
			const bool negative =
				((first ^ second ^ sign_mask(value)) &
				 sign_mask(value)) != 0U;
			return {infinity(value, negative), 0U};
		}
		auto result = captured_operation(
			operation, first, second, width, local_fpcr);
		result.bits = flush_output(result.bits, value,
			width == 16U ? (fpcr & fpcr_fz16) != 0U :
				true);
		result.exceptions = 0U;
		return result;
	}
	}
	return {};
}

inline void execute_floating_reciprocal_software(CpuState &cpu,
	const FloatingReciprocal &decoded, unsigned destination,
	unsigned first_register, unsigned second_register) noexcept
{
	const auto first = cpu.vectors[first_register].bytes;
	const auto second = decoded.unary ? std::array<std::byte, 16>{} :
		cpu.vectors[second_register].bytes;
	auto result = decoded.scalar && (cpu.fpcr & (1U << 2)) != 0U ?
		(decoded.unary ? cpu.vectors[destination].bytes : first) :
		std::array<std::byte, 16>{};
	const unsigned lane_bytes = decoded.width / 8U;
	const unsigned lanes = decoded.scalar ? 1U :
		decoded.vector_bytes / lane_bytes;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto read_lane = [lane, lane_bytes](
			const std::array<std::byte, 16> &bytes) {
			return decode_little_endian(std::span{bytes}.subspan(
				lane * lane_bytes, lane_bytes));
		};
		const auto lane_result = floating_reciprocal_software_lane(
			decoded.operation, decoded.unary, read_lane(first),
			decoded.unary ? 0U : read_lane(second), decoded.width,
			cpu.fpcr);
		encode_little_endian(lane_result.bits,
			std::span{result}.subspan(lane * lane_bytes, lane_bytes));
		cpu.fpsr |= lane_result.exceptions;
	}
	cpu.vectors[destination].bytes = result;
}

} // namespace uml::cpu::arm64::detail
