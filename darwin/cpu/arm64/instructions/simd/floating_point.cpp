// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/floating_arithmetic.hpp"
#include "../../detail/floating_compare.hpp"
#include "../../detail/floating_complex.hpp"
#include "../../detail/floating_conversion.hpp"
#include "../../detail/floating_fused.hpp"
#include "../../detail/floating_horizontal.hpp"
#include "../../detail/floating_round.hpp"
#include "../../detail/floating_scale.hpp"
#include "../../detail/instruction_helpers.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace uml::cpu::arm64 {

using detail::continue_after;
using detail::decode_little_endian;
using detail::encode_little_endian;

namespace {

template<typename Floating>
using FloatingInteger = std::conditional_t<sizeof(Floating) == 2,
	std::uint16_t, std::conditional_t<sizeof(Floating) == 8,
		std::uint64_t, std::uint32_t>>;

template<typename Floating>
[[nodiscard]] Floating decode_lane(const std::array<std::byte, 16> &bytes,
				   std::size_t lane)
{
	using Integer = FloatingInteger<Floating>;
	return std::bit_cast<Floating>(static_cast<Integer>(decode_little_endian(
		std::span{bytes}.subspan(lane * sizeof(Floating), sizeof(Floating)))));
}

template<typename Floating>
void encode_lane(std::array<std::byte, 16> &bytes, std::size_t lane,
		 Floating value)
{
	using Integer = FloatingInteger<Floating>;
	encode_little_endian(std::bit_cast<Integer>(value),
		std::span{bytes}.subspan(lane * sizeof(Floating), sizeof(Floating)));
}

template<typename Floating>
[[nodiscard]] Floating floating_square_root(Floating value)
{
	if constexpr (sizeof(Floating) == 2) {
		Floating result;
		__asm__ volatile("fsqrt %h0, %h1" : "=w"(result) : "w"(value));
		return result;
	} else {
		return std::sqrt(value);
	}
}

template<typename Floating>
[[nodiscard]] Floating basic_floating_operation(
	detail::BasicFloatingOperation operation, Floating first, Floating second)
{
	switch (operation) {
	case detail::BasicFloatingOperation::multiply: return first * second;
	case detail::BasicFloatingOperation::divide: return first / second;
	case detail::BasicFloatingOperation::add: return first + second;
	case detail::BasicFloatingOperation::subtract: return first - second;
	case detail::BasicFloatingOperation::negative_multiply:
		return -(first * second);
	case detail::BasicFloatingOperation::square_root:
		return floating_square_root(first);
	}
	/* The decoder constructs exactly the six enumerators above and the
	 * dispatcher rejects an unrecognized descriptor before this helper. */
	return first;
}

template<typename Floating>
[[nodiscard]] Floating fused_floating_operation(bool negate, bool subtract,
	Floating first, Floating second, Floating addend)
{
	Floating result{};
#define UML_FUSED_ASM(suffix, operation) \
	__asm__ volatile(#operation " %" #suffix "0, %" #suffix "1, %" #suffix \
		"2, %" #suffix "3" : "=w"(result) : "w"(first), "w"(second), \
		"w"(addend))
	if constexpr (sizeof(Floating) == 2) {
		if (!negate && !subtract)
			UML_FUSED_ASM(h, fmadd);
		else if (!negate)
			UML_FUSED_ASM(h, fmsub);
		else if (!subtract)
			UML_FUSED_ASM(h, fnmadd);
		else
			UML_FUSED_ASM(h, fnmsub);
	} else if constexpr (sizeof(Floating) == 4) {
		if (!negate && !subtract)
			UML_FUSED_ASM(s, fmadd);
		else if (!negate)
			UML_FUSED_ASM(s, fmsub);
		else if (!subtract)
			UML_FUSED_ASM(s, fnmadd);
		else
			UML_FUSED_ASM(s, fnmsub);
	} else {
		if (!negate && !subtract)
			UML_FUSED_ASM(d, fmadd);
		else if (!negate)
			UML_FUSED_ASM(d, fmsub);
		else if (!subtract)
			UML_FUSED_ASM(d, fnmadd);
		else
			UML_FUSED_ASM(d, fnmsub);
	}
#undef UML_FUSED_ASM
	return result;
}

template<typename Floating>
[[nodiscard]] Floating standard_floating_round(
	detail::FloatingRoundOperation operation, Floating value)
{
	Floating result = value;
#define UML_ROUND_ASM(suffix, instruction) \
	__asm__ volatile(#instruction " %" #suffix "0, %" #suffix "1" : \
		"=w"(result) : "w"(value))
#define UML_ROUND_SWITCH(suffix) \
	/* Sized forms are handled before this helper. */ \
	switch (operation) { \
	case detail::FloatingRoundOperation::nearest: \
		UML_ROUND_ASM(suffix, frintn); break; \
	case detail::FloatingRoundOperation::plus: \
		UML_ROUND_ASM(suffix, frintp); break; \
	case detail::FloatingRoundOperation::minus: \
		UML_ROUND_ASM(suffix, frintm); break; \
	case detail::FloatingRoundOperation::zero: \
		UML_ROUND_ASM(suffix, frintz); break; \
	case detail::FloatingRoundOperation::ties_away: \
		UML_ROUND_ASM(suffix, frinta); break; \
	case detail::FloatingRoundOperation::exact_current: \
		UML_ROUND_ASM(suffix, frintx); break; \
	case detail::FloatingRoundOperation::current: \
		UML_ROUND_ASM(suffix, frinti); break; \
	default: break; \
	}
	if constexpr (sizeof(Floating) == 2)
		UML_ROUND_SWITCH(h)
	else if constexpr (sizeof(Floating) == 4)
		UML_ROUND_SWITCH(s)
	else
		UML_ROUND_SWITCH(d)
#undef UML_ROUND_SWITCH
#undef UML_ROUND_ASM
	return result;
}

template<typename Floating>
[[nodiscard]] Floating native_floating_horizontal(
	detail::FloatingHorizontalOperation operation, Floating first,
	Floating second)
{
	Floating result = first;
#define UML_HORIZONTAL_ASM(suffix, instruction) \
	__asm__ volatile(#instruction " %" #suffix "0, %" #suffix "1, %" #suffix \
		"2" : "=w"(result) : "w"(first), "w"(second))
#define UML_HORIZONTAL_SWITCH(suffix) \
	switch (operation) { \
	case detail::FloatingHorizontalOperation::absolute_difference: \
		UML_HORIZONTAL_ASM(suffix, fabd); break; \
	case detail::FloatingHorizontalOperation::add: \
		UML_HORIZONTAL_ASM(suffix, fadd); break; \
	case detail::FloatingHorizontalOperation::maximum: \
		UML_HORIZONTAL_ASM(suffix, fmax); break; \
	case detail::FloatingHorizontalOperation::maximum_number: \
		UML_HORIZONTAL_ASM(suffix, fmaxnm); break; \
	case detail::FloatingHorizontalOperation::minimum: \
		UML_HORIZONTAL_ASM(suffix, fmin); break; \
	case detail::FloatingHorizontalOperation::minimum_number: \
		UML_HORIZONTAL_ASM(suffix, fminnm); break; \
	case detail::FloatingHorizontalOperation::absolute_maximum: \
	case detail::FloatingHorizontalOperation::absolute_minimum: \
		/* The caller handles absolute forms before this helper. */ \
		break; \
	}
	if constexpr (sizeof(Floating) == 2)
		UML_HORIZONTAL_SWITCH(h)
	else if constexpr (sizeof(Floating) == 4)
		UML_HORIZONTAL_SWITCH(s)
	else
		UML_HORIZONTAL_SWITCH(d)
#undef UML_HORIZONTAL_SWITCH
#undef UML_HORIZONTAL_ASM
	return result;
}

template<typename Floating>
[[nodiscard]] Floating alternate_floating_minmax(CpuState &cpu,
	detail::FloatingHorizontalOperation operation, Floating first,
	Floating second)
{
	using Integer = FloatingInteger<Floating>;
	const auto first_bits = std::bit_cast<Integer>(first);
	const auto second_bits = std::bit_cast<Integer>(second);
	const auto result = detail::floating_alternate_minmax(
		first_bits, second_bits,
		operation == detail::FloatingHorizontalOperation::maximum ||
			operation == detail::FloatingHorizontalOperation::
				maximum_number,
		cpu.fpcr,
		operation == detail::FloatingHorizontalOperation::maximum_number ||
			operation == detail::FloatingHorizontalOperation::
				minimum_number);
	cpu.fpsr |= result.exceptions;
	return std::bit_cast<Floating>(result.bits);
}

template<typename Floating>
[[nodiscard]] Floating controlled_floating_arithmetic(CpuState &cpu,
	detail::FloatingHorizontalOperation operation, Floating first,
	Floating second)
{
	using Integer = FloatingInteger<Floating>;
	const auto prepared = detail::prepare_floating_controlled_arithmetic(
		std::bit_cast<Integer>(first), std::bit_cast<Integer>(second),
		cpu.fpcr,
		operation == detail::FloatingHorizontalOperation::
			absolute_difference);
	cpu.fpsr |= prepared.exceptions;
	if (prepared.complete)
		return std::bit_cast<Floating>(prepared.bits);
	first = std::bit_cast<Floating>(prepared.first);
	second = std::bit_cast<Floating>(prepared.second);
	std::uint64_t saved_fpcr{};
	std::uint64_t saved_fpsr{};
	__asm__ volatile("mrs %0, fpcr" : "=r"(saved_fpcr) : : "memory");
	__asm__ volatile("mrs %0, fpsr" : "=r"(saved_fpsr) : : "memory");
	const auto local_fpcr = (cpu.fpcr & 2U) != 0U ?
		saved_fpcr & ~((std::uint64_t{1} << 24) |
			(std::uint64_t{1} << 19)) : saved_fpcr;
	if (local_fpcr != saved_fpcr)
		__asm__ volatile("msr fpcr, %0" : : "r"(local_fpcr) : "memory");
	__asm__ volatile("msr fpsr, xzr" : : : "memory");
	const auto result =
		native_floating_horizontal(operation, first, second);
	std::uint64_t raised{};
	__asm__ volatile("mrs %0, fpsr" : "=r"(raised) : : "memory");
	__asm__ volatile("msr fpsr, %0" : : "r"(saved_fpsr) : "memory");
	if (local_fpcr != saved_fpcr)
		__asm__ volatile("msr fpcr, %0" : : "r"(saved_fpcr) : "memory");
	const auto completed = detail::complete_floating_controlled_arithmetic(
		std::bit_cast<Integer>(result), cpu.fpcr,
		static_cast<std::uint32_t>(raised));
	cpu.fpsr |= completed.exceptions;
	return std::bit_cast<Floating>(completed.bits);
}

template<typename Floating>
[[nodiscard]] Floating floating_horizontal_element(CpuState &cpu,
	const detail::FloatingHorizontal &decoded, Floating first, Floating second)
{
	if ((decoded.operation ==
			detail::FloatingHorizontalOperation::absolute_difference ||
	     decoded.operation == detail::FloatingHorizontalOperation::add) &&
	    ((cpu.fpcr & 2U) != 0U ||
	     (decoded.width != 16U && (cpu.fpcr & 1U) != 0U)))
		return controlled_floating_arithmetic(
			cpu, decoded.operation, first, second);
	if (decoded.operation ==
			detail::FloatingHorizontalOperation::absolute_maximum ||
	    decoded.operation ==
			detail::FloatingHorizontalOperation::absolute_minimum) {
		using Integer = FloatingInteger<Floating>;
		const auto result = detail::floating_absolute_minmax(
			std::bit_cast<Integer>(first), std::bit_cast<Integer>(second),
			decoded.operation == detail::FloatingHorizontalOperation::
				absolute_maximum,
			(cpu.fpcr & (1U << 25)) != 0U);
		cpu.fpsr |= result.exceptions;
		return std::bit_cast<Floating>(result.bits);
	}
	if (detail::floating_horizontal_uses_ah(decoded) &&
	    ((cpu.fpcr & 2U) != 0U ||
	     (decoded.width != 16U && (cpu.fpcr & 1U) != 0U)))
		return alternate_floating_minmax(cpu, decoded.operation, first, second);
	return native_floating_horizontal(decoded.operation, first, second);
}

template<typename Floating>
[[nodiscard]] Floating native_floating_reduction(
	detail::FloatingHorizontalOperation operation,
	const std::array<std::byte, 16> &source, unsigned vector_bytes)
{
	Floating result{};
#define UML_REDUCE_ASM(suffix, instruction, arrangement, load) \
	__asm__ volatile(load " [%1]\n\t" #instruction " %" #suffix \
		"0, v1." #arrangement : "=w"(result) : "r"(source.data()) : \
		"v1", "memory")
#define UML_REDUCE_SWITCH(suffix, arrangement, load) \
	/* Reduction decode admits only the four cases below. */ \
	switch (operation) { \
	case detail::FloatingHorizontalOperation::maximum: \
		UML_REDUCE_ASM(suffix, fmaxv, arrangement, load); break; \
	case detail::FloatingHorizontalOperation::maximum_number: \
		UML_REDUCE_ASM(suffix, fmaxnmv, arrangement, load); break; \
	case detail::FloatingHorizontalOperation::minimum: \
		UML_REDUCE_ASM(suffix, fminv, arrangement, load); break; \
	case detail::FloatingHorizontalOperation::minimum_number: \
		UML_REDUCE_ASM(suffix, fminnmv, arrangement, load); break; \
	default: break; \
	}
	if constexpr (sizeof(Floating) == 2) {
		if (vector_bytes == 8U)
			UML_REDUCE_SWITCH(h, 4h, "ldr d1,")
		else
			UML_REDUCE_SWITCH(h, 8h, "ldr q1,")
	} else {
		UML_REDUCE_SWITCH(s, 4s, "ldr q1,")
	}
#undef UML_REDUCE_SWITCH
#undef UML_REDUCE_ASM
	return result;
}

void execute_floating_complex(CpuState &cpu, std::uint32_t instruction,
	const detail::FloatingComplex &decoded)
{
	const unsigned destination = instruction & 31U;
	const auto first = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto second = cpu.vectors[decoded.second_register].bytes;
	auto result = decoded.operation ==
		detail::FloatingComplexOperation::multiply_accumulate ?
		cpu.vectors[destination].bytes : std::array<std::byte, 16>{};
#define UML_COMPLEX_VECTOR(operation, arrangement, rotation) \
	__asm__ volatile("ldr q0, [%1]\n\tldr q1, [%2]\n\tldr q2, [%0]\n\t" \
		#operation " v2." #arrangement ", v0." #arrangement ", v1." \
		#arrangement ", #" #rotation "\n\tstr q2, [%0]" : : \
		"r"(result.data()), "r"(first.data()), "r"(second.data()) : \
		"v0", "v1", "v2", "memory")
#define UML_COMPLEX_ELEMENT(operation, arrangement, element, index, rotation) \
	__asm__ volatile("ldr q0, [%1]\n\tldr q1, [%2]\n\tldr q2, [%0]\n\t" \
		#operation " v2." #arrangement ", v0." #arrangement ", v1." \
		#element "[" #index "], #" #rotation "\n\tstr q2, [%0]" : : \
		"r"(result.data()), "r"(first.data()), "r"(second.data()) : \
		"v0", "v1", "v2", "memory")
#define UML_COMPLEX_ROTATIONS(operation, arrangement) \
	switch (decoded.rotation) { \
	case 0: UML_COMPLEX_VECTOR(operation, arrangement, 0); break; \
	case 1: UML_COMPLEX_VECTOR(operation, arrangement, 90); break; \
	case 2: UML_COMPLEX_VECTOR(operation, arrangement, 180); break; \
	default: UML_COMPLEX_VECTOR(operation, arrangement, 270); break; \
	}
#define UML_COMPLEX_ADD_ROTATIONS(arrangement) \
	do { \
		if (decoded.rotation == 1U) \
			UML_COMPLEX_VECTOR(fcadd, arrangement, 90); \
		else \
			UML_COMPLEX_VECTOR(fcadd, arrangement, 270); \
	} while (false)
#define UML_COMPLEX_ELEMENT_ROTATIONS(arrangement, element, index) \
	switch (decoded.rotation) { \
	case 0: UML_COMPLEX_ELEMENT(fcmla, arrangement, element, index, 0); break; \
	case 1: UML_COMPLEX_ELEMENT(fcmla, arrangement, element, index, 90); break; \
	case 2: UML_COMPLEX_ELEMENT(fcmla, arrangement, element, index, 180); break; \
	default: UML_COMPLEX_ELEMENT(fcmla, arrangement, element, index, 270); break; \
	}
	if (!decoded.by_element) {
		if (decoded.operation == detail::FloatingComplexOperation::add) {
			if (decoded.width == 16U && decoded.vector_bytes == 8U)
				UML_COMPLEX_ADD_ROTATIONS(4h);
			else if (decoded.width == 16U)
				UML_COMPLEX_ADD_ROTATIONS(8h);
			else if (decoded.width == 32U && decoded.vector_bytes == 8U)
				UML_COMPLEX_ADD_ROTATIONS(2s);
			else if (decoded.width == 32U)
				UML_COMPLEX_ADD_ROTATIONS(4s);
			else
				UML_COMPLEX_ADD_ROTATIONS(2d);
		} else if (decoded.width == 16U && decoded.vector_bytes == 8U)
			UML_COMPLEX_ROTATIONS(fcmla, 4h)
		else if (decoded.width == 16U)
			UML_COMPLEX_ROTATIONS(fcmla, 8h)
		else if (decoded.width == 32U && decoded.vector_bytes == 8U)
			UML_COMPLEX_ROTATIONS(fcmla, 2s)
		else if (decoded.width == 32U)
			UML_COMPLEX_ROTATIONS(fcmla, 4s)
		else
			UML_COMPLEX_ROTATIONS(fcmla, 2d)
	} else if (decoded.width == 16U && decoded.vector_bytes == 8U) {
		if (decoded.index == 0U)
			UML_COMPLEX_ELEMENT_ROTATIONS(4h, h, 0)
		else
			UML_COMPLEX_ELEMENT_ROTATIONS(4h, h, 1)
	} else if (decoded.width == 16U) {
		switch (decoded.index) {
		case 0: UML_COMPLEX_ELEMENT_ROTATIONS(8h, h, 0) break;
		case 1: UML_COMPLEX_ELEMENT_ROTATIONS(8h, h, 1) break;
		case 2: UML_COMPLEX_ELEMENT_ROTATIONS(8h, h, 2) break;
		default: UML_COMPLEX_ELEMENT_ROTATIONS(8h, h, 3) break;
		}
	} else if (decoded.index == 0U) {
		UML_COMPLEX_ELEMENT_ROTATIONS(4s, s, 0)
	} else {
		UML_COMPLEX_ELEMENT_ROTATIONS(4s, s, 1)
	}
#undef UML_COMPLEX_ELEMENT_ROTATIONS
#undef UML_COMPLEX_ADD_ROTATIONS
#undef UML_COMPLEX_ROTATIONS
#undef UML_COMPLEX_ELEMENT
#undef UML_COMPLEX_VECTOR
	cpu.vectors[destination].bytes = result;
}

template<typename Floating>
void execute_floating_horizontal(CpuState &cpu, std::uint32_t instruction,
	const detail::FloatingHorizontal &decoded)
{
	const unsigned destination = instruction & 31U;
	const unsigned first_register = (instruction >> 5) & 31U;
	const unsigned second_register = (instruction >> 16) & 31U;
	const auto first_source = cpu.vectors[first_register].bytes;
	const auto second_source = cpu.vectors[second_register].bytes;
	std::array<std::byte, 16> result{};
	if (decoded.operation ==
			detail::FloatingHorizontalOperation::absolute_maximum ||
	    decoded.operation ==
			detail::FloatingHorizontalOperation::absolute_minimum) {
		using Integer = FloatingInteger<Floating>;
		const unsigned lanes = decoded.vector_bytes / sizeof(Integer);
		for (unsigned lane = 0; lane < lanes; ++lane) {
			const auto first = static_cast<Integer>(decode_little_endian(
				std::span{first_source}.subspan(lane * sizeof(Integer),
					sizeof(Integer))));
			const auto second = static_cast<Integer>(decode_little_endian(
				std::span{second_source}.subspan(lane * sizeof(Integer),
					sizeof(Integer))));
			const auto value = detail::floating_absolute_minmax(first, second,
				decoded.operation == detail::FloatingHorizontalOperation::
					absolute_maximum,
				(cpu.fpcr & (1U << 25)) != 0U);
			encode_little_endian(value.bits, std::span{result}.subspan(
				lane * sizeof(Integer), sizeof(Integer)));
			cpu.fpsr |= value.exceptions;
		}
		cpu.vectors[destination].bytes = result;
		return;
	}
	if (decoded.form == detail::FloatingHorizontalForm::scalar_binary &&
	    (cpu.fpcr & 4U) != 0U)
		result = first_source;

	if (decoded.form == detail::FloatingHorizontalForm::scalar_binary) {
		encode_lane(result, 0, floating_horizontal_element(cpu, decoded,
			decode_lane<Floating>(first_source, 0),
			decode_lane<Floating>(second_source, 0)));
	} else if (decoded.form ==
		   detail::FloatingHorizontalForm::scalar_pairwise) {
		encode_lane(result, 0, floating_horizontal_element(cpu, decoded,
			decode_lane<Floating>(first_source, 0),
			decode_lane<Floating>(first_source, 1)));
	} else if (decoded.form == detail::FloatingHorizontalForm::vector_binary) {
		const unsigned lanes = decoded.vector_bytes / sizeof(Floating);
		for (unsigned lane = 0; lane < lanes; ++lane)
			encode_lane(result, lane, floating_horizontal_element(cpu, decoded,
				decode_lane<Floating>(first_source, lane),
				decode_lane<Floating>(second_source, lane)));
	} else if (decoded.form ==
		   detail::FloatingHorizontalForm::vector_pairwise) {
		const unsigned lanes = decoded.vector_bytes / sizeof(Floating);
		for (unsigned lane = 0; lane < lanes; ++lane) {
			const auto &source = lane < lanes / 2U ?
				first_source : second_source;
			const unsigned pair = (lane % (lanes / 2U)) * 2U;
			encode_lane(result, lane, floating_horizontal_element(cpu, decoded,
				decode_lane<Floating>(source, pair),
				decode_lane<Floating>(source, pair + 1U)));
		}
	} else {
		const unsigned lanes = decoded.vector_bytes / sizeof(Floating);
		Floating value;
		if ((cpu.fpcr & 2U) == 0U ||
		    !detail::floating_horizontal_uses_ah(decoded)) {
			value = native_floating_reduction<Floating>(decoded.operation,
				first_source, decoded.vector_bytes);
		} else {
			std::array<Floating, 8> values{};
			for (unsigned lane = 0; lane < lanes; ++lane)
				values[lane] = decode_lane<Floating>(first_source, lane);
			for (unsigned remaining = lanes; remaining > 1U;
			     remaining /= 2U)
				for (unsigned lane = 0; lane < remaining / 2U; ++lane)
					values[lane] = floating_horizontal_element(cpu, decoded,
						values[lane * 2U], values[lane * 2U + 1U]);
			value = values[0];
		}
		encode_lane(result, 0, value);
	}
	cpu.vectors[destination].bytes = result;
}

template<typename Floating>
void execute_floating_round(CpuState &cpu, std::uint32_t instruction,
	const detail::FloatingRound &decoded)
{
	const unsigned destination = instruction & 31U;
	const auto source = cpu.vectors[(instruction >> 5) & 31U].bytes;
	auto result = decoded.scalar && (cpu.fpcr & 4U) != 0U ?
		cpu.vectors[destination].bytes : std::array<std::byte, 16>{};
	const unsigned lanes = decoded.scalar ? 1U :
		decoded.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto value = decode_lane<Floating>(source, lane);
		if (detail::is_sized_round(decoded.operation)) {
			if constexpr (sizeof(Floating) == 2) {
				/* Sized-round encodings never decode as half precision. Keep
				 * the source unchanged if an internal descriptor is corrupted. */
				encode_lane(result, lane, value);
			} else {
				const unsigned integer_bits =
					decoded.operation ==
						detail::FloatingRoundOperation::int32_exact ||
					decoded.operation ==
						detail::FloatingRoundOperation::int32_zero ? 32U : 64U;
				const bool current =
					decoded.operation ==
						detail::FloatingRoundOperation::int32_exact ||
					decoded.operation ==
						detail::FloatingRoundOperation::int64_exact;
				const auto rounded = detail::round_floating_sized<Floating>(
					std::bit_cast<FloatingInteger<Floating>>(value),
					integer_bits, current);
				cpu.fpsr |= rounded.exceptions;
				encode_lane(result, lane, std::bit_cast<Floating>(
					static_cast<FloatingInteger<Floating>>(rounded.bits)));
			}
		} else {
			encode_lane(result, lane,
				standard_floating_round(decoded.operation, value));
		}
	}
	cpu.vectors[destination].bytes = result;
}

template<typename Integer>
[[nodiscard]] constexpr std::int64_t signed_scale(Integer value) noexcept
{
	using Signed = std::make_signed_t<Integer>;
	return static_cast<std::int64_t>(std::bit_cast<Signed>(value));
}

template<typename Integer>
void execute_floating_scale(CpuState &cpu, std::uint32_t instruction,
	const detail::FloatingScale &decoded)
{
	const unsigned destination = instruction & 31U;
	const auto first = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto second = cpu.vectors[(instruction >> 16) & 31U].bytes;
	std::array<std::byte, 16> result{};
	const unsigned lanes = decoded.vector_bytes / sizeof(Integer);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto value = static_cast<Integer>(decode_little_endian(
			std::span{first}.subspan(lane * sizeof(Integer), sizeof(Integer))));
		const auto scale = static_cast<Integer>(decode_little_endian(
			std::span{second}.subspan(lane * sizeof(Integer), sizeof(Integer))));
		const auto scaled = detail::scale_floating(value, decoded.width,
			signed_scale(scale), cpu.fpcr);
		encode_little_endian(scaled.bits, std::span{result}.subspan(
			lane * sizeof(Integer), sizeof(Integer)));
		cpu.fpsr |= scaled.exceptions;
	}
	cpu.vectors[destination].bytes = result;
}

template<typename Floating>
void execute_fused_floating_arithmetic(CpuState &cpu,
	std::uint32_t instruction, const detail::FusedFloatingArithmetic &decoded)
{
	const unsigned destination = instruction & 31U;
	const auto left = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 31U].bytes;
	const unsigned addend_register = decoded.scalar ?
		(instruction >> 10) & 31U : destination;
	const auto addend = cpu.vectors[addend_register].bytes;
	std::array<std::byte, 16> result{};
	if (decoded.scalar) {
		if ((cpu.fpcr & 4U) != 0U)
			result = addend;
	} else {
		std::copy_n(addend.begin(), decoded.vector_bytes, result.begin());
	}
	const unsigned lanes = decoded.scalar ? 1U :
		decoded.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; ++lane)
		encode_lane(result, lane, fused_floating_operation(
			decoded.negate, decoded.subtract,
			decode_lane<Floating>(left, lane),
			decode_lane<Floating>(right, lane),
			decode_lane<Floating>(addend, lane)));
	cpu.vectors[destination].bytes = result;
}

template<typename Floating>
void execute_basic_floating_arithmetic(CpuState &cpu,
	std::uint32_t instruction, const detail::BasicFloatingArithmetic &decoded)
{
	const unsigned destination = instruction & 31U;
	const auto left = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 31U].bytes;
	auto result = decoded.scalar && (cpu.fpcr & 4U) != 0U ?
		(decoded.unary ? cpu.vectors[destination].bytes : left) :
		std::array<std::byte, 16>{};
	const unsigned lanes = decoded.scalar ? 1U :
		decoded.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto first = decode_lane<Floating>(left, lane);
		const auto second = decoded.unary ? Floating{} :
			decode_lane<Floating>(right, lane);
		encode_lane(result, lane,
			basic_floating_operation(decoded.operation, first, second));
	}
	cpu.vectors[destination].bytes = result;
}

/*
 * The three-same floating-point allocations that execute_floating_vector()
 * implements, by U, a (bit 23) and opcode. FMULX has its own decoder, and the
 * scalar class holds only the compares, FABD and the reciprocal steps.
 */
[[nodiscard]] constexpr bool allocated_floating_three_same(
	std::uint32_t instruction) noexcept
{
	const unsigned opcode = (instruction >> 11) & 0x1fU;
	const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
	const bool alternate = (instruction & 0x0080'0000U) != 0;
	if ((instruction & 0x1000'0000U) != 0)
		return unsigned_group ?
			(opcode == 0x1a && alternate) || opcode == 0x1c ||
				opcode == 0x1d :
			(opcode == 0x1c && !alternate) || opcode == 0x1f;
	switch (opcode) {
	case 0x18: // FMAXNM/FMINNM, FMAXNMP/FMINNMP
	case 0x1a: // FADD/FSUB, FADDP/FABD
	case 0x1e: // FMAX/FMIN, FMAXP/FMINP
		return true;
	case 0x19: // FMLA/FMLS
		return !unsigned_group;
	case 0x1b: // FMUL
		return unsigned_group && !alternate;
	case 0x1c: // FCMEQ, FCMGE/FCMGT
		return unsigned_group || !alternate;
	case 0x1d: // FACGE/FACGT
		return unsigned_group;
	case 0x1f: // FRECPS/FRSQRTS, FDIV
		return !unsigned_group || !alternate;
	default:
		return false;
	}
}

template<typename Floating>
[[nodiscard]] bool execute_floating_vector(CpuState &cpu,
					   std::uint32_t instruction,
					   std::size_t lanes)
{
	const unsigned opcode = (instruction >> 11) & 0x1fU;
	const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
	const bool alternate = (instruction & 0x0080'0000U) != 0;
	if (opcode < 0x18)
		return false;
	const auto left = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	const auto old_destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t lane = 0; lane < lanes; ++lane) {
		Floating first;
		Floating second;
		if (unsigned_group &&
		    (opcode == 0x18 || (opcode == 0x1a && !alternate) ||
		     opcode == 0x1e)) {
			const auto &source = lane < lanes / 2 ? left : right;
			const std::size_t pair = (lane % (lanes / 2)) * 2;
			first = decode_lane<Floating>(source, pair);
			second = decode_lane<Floating>(source, pair + 1);
		} else {
			first = decode_lane<Floating>(left, lane);
			second = decode_lane<Floating>(right, lane);
		}
		Floating value;
		if (opcode == 0x18) {
			value = alternate ? std::fmin(first, second) :
				std::fmax(first, second);
		} else if (opcode == 0x19 && !unsigned_group) {
			const auto accumulator = decode_lane<Floating>(
				old_destination, lane);
			value = alternate ? accumulator - first * second :
				accumulator + first * second;
		} else if (opcode == 0x1a && !unsigned_group) {
			value = alternate ? first - second : first + second;
		} else if (opcode == 0x1a && unsigned_group) {
			value = alternate ? std::fabs(first - second) : first + second;
		} else if (opcode == 0x1b && !alternate) {
			value = first * second;
		} else if (opcode == 0x1c) {
			const bool comparison = !unsigned_group ? first == second :
				alternate ? first > second : first >= second;
			using Integer = std::conditional_t<sizeof(Floating) == 8,
				std::uint64_t, std::uint32_t>;
			encode_little_endian(comparison ? ~Integer{0} : Integer{0},
				std::span{result}.subspan(lane * sizeof(Floating),
					sizeof(Floating)));
			continue;
		} else if (opcode == 0x1d && unsigned_group) {
			const bool comparison = alternate ?
				std::fabs(first) > std::fabs(second) :
				std::fabs(first) >= std::fabs(second);
			using Integer = std::conditional_t<sizeof(Floating) == 8,
				std::uint64_t, std::uint32_t>;
			encode_little_endian(comparison ? ~Integer{0} : Integer{0},
				std::span{result}.subspan(lane * sizeof(Floating),
					sizeof(Floating)));
			continue;
		} else if (opcode == 0x1e) {
			if (std::isnan(first) || std::isnan(second))
				value = std::numeric_limits<Floating>::quiet_NaN();
			else
				value = alternate ? std::fmin(first, second) :
					std::fmax(first, second);
		} else if (opcode == 0x1f && unsigned_group && !alternate) {
			value = first / second;
		} else if (opcode == 0x1f && !unsigned_group) {
			value = alternate ?
				(static_cast<Floating>(3) - first * second) /
					static_cast<Floating>(2) :
				static_cast<Floating>(2) - first * second;
		} else {
			return false;
		}
		encode_lane(result, lane, value);
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	return true;
}

template<typename Floating>
void execute_floating_sqrt(CpuState &cpu, std::uint32_t instruction,
			   std::size_t lanes)
{
	using Integer = std::conditional_t<sizeof(Floating) == 8,
		std::uint64_t, std::uint32_t>;
	const auto source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t lane = 0; lane < lanes; ++lane) {
		const auto raw = static_cast<Integer>(decode_little_endian(
			std::span{source}.subspan(lane * sizeof(Floating),
				sizeof(Floating))));
		const auto value = std::bit_cast<Integer>(
			std::sqrt(std::bit_cast<Floating>(raw)));
		encode_little_endian(value,
			std::span{result}.subspan(lane * sizeof(Floating),
				sizeof(Floating)));
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
}

[[nodiscard]] constexpr std::uint64_t floating_sign_operation(
	std::uint64_t value, unsigned bits, bool negate,
	bool alternative_handling) noexcept
{
	const unsigned fraction_bits = bits == 16U ? 10U : bits == 32U ? 23U : 52U;
	const unsigned exponent_bits = bits == 16U ? 5U : bits == 32U ? 8U : 11U;
	const auto sign = std::uint64_t{1} << (bits - 1U);
	const auto fraction = (std::uint64_t{1} << fraction_bits) - 1U;
	const auto exponent = ((std::uint64_t{1} << exponent_bits) - 1U) <<
		fraction_bits;
	if (alternative_handling && (value & exponent) == exponent &&
	    (value & fraction) != 0U)
		return value;
	return negate ? value ^ sign : value & ~sign;
}

void execute_floating_sign_unary(CpuState &cpu, std::uint32_t instruction,
				 unsigned lane_bytes, unsigned vector_bytes,
				 bool negate)
{
	const auto source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (unsigned offset = 0; offset < vector_bytes; offset += lane_bytes) {
		const auto raw = decode_little_endian(
			std::span{source}.subspan(offset, lane_bytes));
		const auto value = floating_sign_operation(raw, lane_bytes * 8U,
			negate, (cpu.fpcr & 2U) != 0U);
		encode_little_endian(value,
			std::span{result}.subspan(offset, lane_bytes));
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
}

} // namespace

RunResult Dispatcher::simd_floating_unary(CpuState &cpu, GuestMemory &,
					   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool q = (instruction & 0x4000'0000U) != 0U;
	const unsigned vector_bytes = q ? 16U : 8U;
	const auto half_operation = instruction & 0xbfff'fc00U;
	const auto ordinary_operation = instruction & 0xbf3f'fc00U;
	if (half_operation == 0x0ef8'f800U ||
	    half_operation == 0x2ef8'f800U) {
		execute_floating_sign_unary(cpu, instruction, 2U, vector_bytes,
			half_operation == 0x2ef8'f800U);
	} else if (ordinary_operation == 0x0e20'f800U ||
		   ordinary_operation == 0x2e20'f800U) {
		if ((instruction & 0x0080'0000U) == 0U)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		const bool is_double = (instruction & 0x0040'0000U) != 0U;
		if (is_double && !q)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		execute_floating_sign_unary(cpu, instruction, is_double ? 8U : 4U,
			vector_bytes, ordinary_operation == 0x2e20'f800U);
	} else if (ordinary_operation == 0x2e21'f800U &&
		   (instruction & 0x0080'0000U) != 0U) {
		const bool is_double = (instruction & 0x0040'0000U) != 0U;
		if (is_double && !q)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		if (is_double)
			execute_floating_sqrt<double>(cpu, instruction, 2U);
		else
			execute_floating_sqrt<float>(cpu, instruction, vector_bytes / 4U);
	} else {
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	}
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_floating_precision_vector(CpuState &cpu,
	GuestMemory &, std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded =
		detail::decode_simd_floating_precision_vector(instruction);
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = instruction & 31U;
	const auto source = cpu.vectors[(instruction >> 5) & 31U].bytes;
	auto result = (decoded.narrow && decoded.upper) ||
		(decoded.scalar && (cpu.fpcr & 4U) != 0U) ?
		cpu.vectors[destination].bytes : std::array<std::byte, 16>{};
	const unsigned source_bytes = decoded.source_width / 8U;
	const unsigned destination_bytes = decoded.destination_width / 8U;
	const unsigned source_base = !decoded.narrow && decoded.upper ? 8U : 0U;
	const unsigned destination_base = decoded.narrow && decoded.upper ? 8U : 0U;
	std::uint32_t exceptions{};
	for (unsigned lane = 0; lane < decoded.lanes; ++lane) {
		const auto input = decode_little_endian(std::span{source}.subspan(
			source_base + lane * source_bytes, source_bytes));
		const auto converted = detail::convert_floating_precision(input,
			decoded.source_width, decoded.destination_width, cpu.fpcr,
			decoded.round_odd ? 4U : ~0U);
		encode_little_endian(converted.bits, std::span{result}.subspan(
			destination_base + lane * destination_bytes, destination_bytes));
		exceptions |= converted.exceptions;
	}
	cpu.vectors[destination].bytes = result;
	cpu.fpsr |= exceptions;
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_floating_arithmetic(CpuState &cpu, GuestMemory &,
						std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_double = (instruction & 0x0040'0000U) != 0;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const std::size_t vector_size =
		(instruction & 0x4000'0000U) != 0 ? 16 : 8;
	if (!allocated_floating_three_same(instruction))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if ((is_double && !scalar && vector_size != 16) ||
	    !(is_double ? execute_floating_vector<double>(cpu, instruction,
			scalar ? 1 : 2) : execute_floating_vector<float>(cpu,
			instruction, scalar ? 1 : vector_size / 4)))
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_basic_arithmetic(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded =
		detail::decode_basic_floating_arithmetic(instruction);
	/* The reference path validates the complete allocation before reading a
	 * source or changing FPSR, destination state, or PC. */
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	switch (decoded.width) {
	case 16:
		execute_basic_floating_arithmetic<_Float16>(cpu, instruction, decoded);
		break;
	case 32:
		execute_basic_floating_arithmetic<float>(cpu, instruction, decoded);
		break;
	case 64:
		execute_basic_floating_arithmetic<double>(cpu, instruction, decoded);
		break;
	default:
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	}
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_fused_arithmetic(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_fused_floating_arithmetic(instruction);
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	switch (decoded.width) {
	case 16:
		execute_fused_floating_arithmetic<_Float16>(cpu, instruction, decoded);
		break;
	case 32:
		execute_fused_floating_arithmetic<float>(cpu, instruction, decoded);
		break;
	case 64:
		execute_fused_floating_arithmetic<double>(cpu, instruction, decoded);
		break;
	default:
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	}
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_round(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_floating_round(instruction);
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	switch (decoded.width) {
	case 16:
		execute_floating_round<_Float16>(cpu, instruction, decoded);
		break;
	case 32:
		execute_floating_round<float>(cpu, instruction, decoded);
		break;
	case 64:
		execute_floating_round<double>(cpu, instruction, decoded);
		break;
	default:
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	}
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_horizontal(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_floating_horizontal(instruction);
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	switch (decoded.width) {
	case 16:
		execute_floating_horizontal<_Float16>(cpu, instruction, decoded);
		break;
	case 32:
		execute_floating_horizontal<float>(cpu, instruction, decoded);
		break;
	case 64:
		execute_floating_horizontal<double>(cpu, instruction, decoded);
		break;
	default:
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	}
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_scale(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_floating_scale(instruction);
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (decoded.width == 16U)
		execute_floating_scale<std::uint16_t>(cpu, instruction, decoded);
	else if (decoded.width == 32U)
		execute_floating_scale<std::uint32_t>(cpu, instruction, decoded);
	else if (decoded.width == 64U)
		execute_floating_scale<std::uint64_t>(cpu, instruction, decoded);
	else
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_complex(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_floating_complex(instruction);
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	execute_floating_complex(cpu, instruction, decoded);
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

} // namespace uml::cpu::arm64
