// SPDX-License-Identifier: GPL-2.0-or-later
#include "../dispatcher.hpp"

#include "../detail/floating_compare.hpp"
#include "../detail/floating_conversion.hpp"
#include "../detail/instruction_helpers.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace uml::backend::arm64 {

using detail::continue_after;
using detail::condition_holds;
using detail::vector_half;

namespace {

[[nodiscard]] long double round_floating(long double value, unsigned mode)
{
	switch (mode) {
	case 0:
		return std::nearbyint(value);
	case 1:
		return std::ceil(value);
	case 2:
		return std::floor(value);
	case 4:
		return std::round(value);
	default:
		return std::trunc(value);
	}
}

template<typename Floating>
[[nodiscard]] bool round_floating_to_integral(Floating value,
					       unsigned operation,
					       unsigned fpcr_mode,
					       Floating &result)
{
	switch (operation) {
	case 8:
		result = static_cast<Floating>(round_floating(value, 0));
		return true;
	case 9:
		result = std::ceil(value);
		return true;
	case 10:
		result = std::floor(value);
		return true;
	case 11:
		result = std::trunc(value);
		return true;
	case 12:
		result = std::round(value);
		return true;
	case 14:
	case 15:
		result = static_cast<Floating>(round_floating(value, fpcr_mode));
		return true;
	default:
		return false;
	}
}

struct IntegerConversion final {
	std::uint64_t value;
	std::uint32_t exceptions;
};

[[nodiscard]] IntegerConversion convert_floating_to_integer(long double value,
							    bool unsigned_result,
							    unsigned bits,
							    unsigned rounding_mode)
{
	if (std::isnan(value))
		return {0U, 1U};
	const auto rounded = round_floating(value, rounding_mode);
	if (unsigned_result) {
		const auto upper_exclusive = std::ldexp(1.0L, static_cast<int>(bits));
		if (rounded < 0)
			return {0U, 1U};
		if (rounded >= upper_exclusive)
			return {bits == 64 ? ~std::uint64_t{0} : 0xffff'ffffULL, 1U};
		return {static_cast<std::uint64_t>(rounded),
			value != rounded ? 1U << 4 : 0U};
	}
	const auto upper_exclusive = std::ldexp(1.0L, static_cast<int>(bits - 1U));
	if (rounded < -upper_exclusive)
		return {bits == 64 ? std::uint64_t{1} << 63 : 0x8000'0000ULL, 1U};
	if (rounded >= upper_exclusive)
		return {bits == 64 ? 0x7fff'ffff'ffff'ffffULL : 0x7fff'ffffULL, 1U};
	return {static_cast<std::uint64_t>(static_cast<std::int64_t>(rounded)),
		value != rounded ? 1U << 4 : 0U};
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
	/* FPAbs/FPNeg preserve the entire NaN encoding when FPCR.AH is set,
	 * including its sign and signaling/quiet payload. They never raise an
	 * exception, so integer-space semantics also avoid quieting an SNaN or
	 * accidentally setting FPSR.IOC in the debug interpreter. */
	if (alternative_handling && (value & exponent) == exponent &&
	    (value & fraction) != 0U)
		return value;
	return negate ? value ^ sign : value & ~sign;
}

[[nodiscard]] constexpr std::uint64_t expand_vfp_immediate(
	std::uint8_t immediate, unsigned bits) noexcept
{
	const unsigned exponent_bits = bits == 16U ? 5U : bits == 32U ? 8U : 11U;
	const unsigned fraction_bits = bits - exponent_bits - 1U;
	const auto sign = static_cast<std::uint64_t>(immediate >> 7) << (bits - 1U);
	const auto control = static_cast<std::uint64_t>((immediate >> 6) & 1U);
	const auto exponent_top = (control ^ 1U) << (bits - 2U);
	const auto exponent_repeat = control == 0U ? 0U :
		((std::uint64_t{1} << (exponent_bits - 3U)) - 1U) <<
			(fraction_bits + 2U);
	const auto payload = static_cast<std::uint64_t>(immediate & 0x3fU) <<
		(fraction_bits - 4U);
	return sign | exponent_top | exponent_repeat | payload;
}

} // namespace

RunResult Dispatcher::floating_compare(CpuState &cpu, GuestMemory &,
				       std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned type = (instruction >> 22) & 3U;
	const bool compare_zero = (instruction & 8U) != 0U;
	const unsigned right_register = (instruction >> 16) & 0x1fU;
	if (type == 2U || (instruction & 7U) != 0U ||
	    (compare_zero && right_register != 0U))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned left_register = (instruction >> 5) & 0x1fU;
	const unsigned width = type == 3U ? 16U : type == 0U ? 32U : 64U;
	const auto comparison = detail::compare_floating_bits(
		vector_half(cpu.vectors[left_register], 0),
		compare_zero ? 0U : vector_half(cpu.vectors[right_register], 0),
		width, (instruction & 16U) != 0U, cpu.fpcr);
	cpu.nzcv = comparison.nzcv;
	cpu.fpsr |= comparison.exceptions;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_conditional_compare(CpuState &cpu,
						    GuestMemory &,
						    std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned type = (instruction >> 22) & 0x3U;
	if (type == 2U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (!condition_holds(cpu.nzcv, (instruction >> 12) & 0xfU)) {
		cpu.nzcv = (instruction & 0xfU) << 28;
	} else {
		const unsigned left_register = (instruction >> 5) & 0x1fU;
		const unsigned right_register = (instruction >> 16) & 0x1fU;
		const unsigned width = type == 3U ? 16U : type == 0U ? 32U : 64U;
		const auto comparison = detail::compare_floating_bits(
			vector_half(cpu.vectors[left_register], 0),
			vector_half(cpu.vectors[right_register], 0), width,
			(instruction & 16U) != 0U, cpu.fpcr);
		cpu.nzcv = comparison.nzcv;
		cpu.fpsr |= comparison.exceptions;
	}
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_conditional_select(CpuState &cpu,
						   GuestMemory &,
						   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned type = (instruction >> 22) & 0x3U;
	if (type == 2U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned source = condition_holds(cpu.nzcv,
		(instruction >> 12) & 0xfU) ? (instruction >> 5) & 0x1fU :
		(instruction >> 16) & 0x1fU;
	const unsigned lane_bytes = type == 3U ? 2U : type == 0U ? 4U : 8U;
	const auto source_bytes = cpu.vectors[source].bytes;
	const unsigned destination = instruction & 0x1fU;
	std::array<std::byte, 16> result{};
	std::copy_n(source_bytes.begin(), lane_bytes, result.begin());
	cpu.vectors[destination].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_multiply_add(CpuState &cpu, GuestMemory &,
					     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned type = (instruction >> 22) & 0x3U;
	if (type > 1)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const unsigned first = (instruction >> 5) & 0x1fU;
	const unsigned second = (instruction >> 16) & 0x1fU;
	const unsigned addend = (instruction >> 10) & 0x1fU;
	const bool negate_result = (instruction & 0x0020'0000U) != 0;
	const bool subtract_product = (instruction & 0x0000'8000U) != 0;
	std::uint64_t output;
	if (type == 0) {
		const auto left = std::bit_cast<float>(static_cast<std::uint32_t>(
			vector_half(cpu.vectors[first], 0)));
		const auto right = std::bit_cast<float>(static_cast<std::uint32_t>(
			vector_half(cpu.vectors[second], 0)));
		const auto accumulated = std::bit_cast<float>(
			static_cast<std::uint32_t>(
				vector_half(cpu.vectors[addend], 0)));
		float result = subtract_product ? std::fma(-left, right, accumulated) :
			std::fma(left, right, accumulated);
		if (negate_result)
			result = -result;
		output = std::bit_cast<std::uint32_t>(result);
	} else {
		const auto left = std::bit_cast<double>(
			vector_half(cpu.vectors[first], 0));
		const auto right = std::bit_cast<double>(
			vector_half(cpu.vectors[second], 0));
		const auto accumulated = std::bit_cast<double>(
			vector_half(cpu.vectors[addend], 0));
		double result = subtract_product ? std::fma(-left, right, accumulated) :
			std::fma(left, right, accumulated);
		if (negate_result)
			result = -result;
		output = std::bit_cast<std::uint64_t>(result);
	}
	const unsigned destination = instruction & 0x1fU;
	detail::set_vector_half(cpu.vectors[destination], 0, output);
	detail::set_vector_half(cpu.vectors[destination], 1, 0);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_move(CpuState &cpu, GuestMemory &,
				    std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned type = (instruction >> 22) & 3U;
	if (type == 2U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned lane_bytes = type == 3U ? 2U : type == 0U ? 4U : 8U;
	const unsigned destination = instruction & 0x1fU;
	std::uint64_t bits{};
	if ((instruction & 0xff20'1fe0U) == 0x1e20'1000U) {
		const auto immediate = static_cast<std::uint8_t>(
			(instruction >> 13) & 0xffU);
		bits = expand_vfp_immediate(immediate, lane_bytes * 8U);
	} else if ((instruction & 0xff3f'fc00U) == 0x1e20'4000U) {
		const auto source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
		bits = detail::decode_little_endian(
			std::span{source}.first(lane_bytes));
	} else {
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	}
	std::array<std::byte, 16> result{};
	detail::encode_little_endian(bits, std::span{result}.first(lane_bytes));
	cpu.vectors[destination].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_arithmetic(CpuState &cpu, GuestMemory &,
					  std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned type = (instruction >> 22) & 0x3U;
	const unsigned source = (instruction >> 5) & 0x1fU;
	const unsigned destination = instruction & 0x1fU;
	const bool one_source =
		(instruction & 0xff20'7c00U) == 0x1e20'4000U;
	const unsigned unary_operation = (instruction >> 15) & 0x3fU;
	if (one_source && (unary_operation == 1U || unary_operation == 2U)) {
		/* ftype=10 is reserved. Validate it before any register read so a bad
		 * instruction cannot partially mutate state in the safety-first path. */
		if (type == 2U)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		const unsigned lane_bytes = type == 3U ? 2U : type == 0U ? 4U : 8U;
		const auto source_bytes = cpu.vectors[source].bytes;
		auto result = (cpu.fpcr & 4U) != 0U ?
			cpu.vectors[destination].bytes : std::array<std::byte, 16>{};
		const auto raw = detail::decode_little_endian(
			std::span{source_bytes}.first(lane_bytes));
		const auto output = floating_sign_operation(raw, lane_bytes * 8U,
			unary_operation == 2U, (cpu.fpcr & 2U) != 0U);
		detail::encode_little_endian(output,
			std::span{result}.first(lane_bytes));
		cpu.vectors[destination].bytes = result;
		cpu.pc += 4;
		return continue_after(address, instruction);
	}
	if (one_source && unary_operation >= 4U && unary_operation <= 7U) {
		const unsigned destination_type = (instruction >> 15) & 3U;
		if (type == 2U || destination_type == 2U || type == destination_type)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		const unsigned source_width = type == 3U ? 16U : type == 0U ? 32U : 64U;
		const unsigned destination_width = destination_type == 3U ? 16U :
			destination_type == 0U ? 32U : 64U;
		const auto converted = detail::convert_floating_precision(
			vector_half(cpu.vectors[source], 0), source_width,
			destination_width, cpu.fpcr);
		auto result = (cpu.fpcr & 4U) != 0U ?
			cpu.vectors[destination].bytes : std::array<std::byte, 16>{};
		detail::encode_little_endian(converted.bits,
			std::span{result}.first(destination_width / 8U));
		cpu.vectors[destination].bytes = result;
		cpu.fpsr |= converted.exceptions;
		cpu.pc += 4;
		return continue_after(address, instruction);
	}
	if (type > 1U)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	std::uint64_t output;
	if (type == 0) {
		const auto left = std::bit_cast<float>(static_cast<std::uint32_t>(
			vector_half(cpu.vectors[source], 0)));
		float result;
		if (one_source) {
			switch (unary_operation) {
			case 0: result = left; break;
			case 1: result = std::fabs(left); break;
			case 2: result = -left; break;
			case 3: result = std::sqrt(left); break;
			default:
				if (!round_floating_to_integral(left,
						unary_operation,
						(cpu.fpcr >> 22) & 0x3U,
						result))
					return {StopReason::unsupported_instruction,
						address, instruction, 0, false};
				break;
			}
		} else {
			const auto right = std::bit_cast<float>(
				static_cast<std::uint32_t>(vector_half(
					cpu.vectors[(instruction >> 16) & 0x1fU], 0)));
			switch ((instruction >> 12) & 0xfU) {
			case 0: result = left * right; break;
			case 1: result = left / right; break;
			case 2: result = left + right; break;
			case 3: result = left - right; break;
			case 4: result = std::isnan(left) || std::isnan(right) ?
				std::numeric_limits<float>::quiet_NaN() :
				std::fmax(left, right); break;
			case 5: result = std::isnan(left) || std::isnan(right) ?
				std::numeric_limits<float>::quiet_NaN() :
				std::fmin(left, right); break;
			case 6: result = std::fmax(left, right); break;
			case 7: result = std::fmin(left, right); break;
			case 8: result = -(left * right); break;
			default:
				return {StopReason::unsupported_instruction, address,
					instruction, 0, false};
			}
		}
		output = std::bit_cast<std::uint32_t>(result);
	} else {
		const auto left = std::bit_cast<double>(
			vector_half(cpu.vectors[source], 0));
		double result;
		if (one_source) {
			switch (unary_operation) {
			case 0: result = left; break;
			case 1: result = std::fabs(left); break;
			case 2: result = -left; break;
			case 3: result = std::sqrt(left); break;
			default:
				if (!round_floating_to_integral(left,
						unary_operation,
						(cpu.fpcr >> 22) & 0x3U,
						result))
					return {StopReason::unsupported_instruction,
						address, instruction, 0, false};
				break;
			}
		} else {
			const auto right = std::bit_cast<double>(
				vector_half(cpu.vectors[(instruction >> 16) & 0x1fU], 0));
			switch ((instruction >> 12) & 0xfU) {
			case 0: result = left * right; break;
			case 1: result = left / right; break;
			case 2: result = left + right; break;
			case 3: result = left - right; break;
			case 4: result = std::isnan(left) || std::isnan(right) ?
				std::numeric_limits<double>::quiet_NaN() :
				std::fmax(left, right); break;
			case 5: result = std::isnan(left) || std::isnan(right) ?
				std::numeric_limits<double>::quiet_NaN() :
				std::fmin(left, right); break;
			case 6: result = std::fmax(left, right); break;
			case 7: result = std::fmin(left, right); break;
			case 8: result = -(left * right); break;
			default:
				return {StopReason::unsupported_instruction, address,
					instruction, 0, false};
			}
		}
		output = std::bit_cast<std::uint64_t>(result);
	}
	if (one_source && unary_operation == 14) {
		bool inexact;
		if (type == 0) {
			const auto input = std::bit_cast<float>(
				static_cast<std::uint32_t>(
					vector_half(cpu.vectors[source], 0)));
			const auto result = std::bit_cast<float>(
				static_cast<std::uint32_t>(output));
			inexact = std::isfinite(input) && input != result;
		} else {
			const auto input = std::bit_cast<double>(
				vector_half(cpu.vectors[source], 0));
			const auto result = std::bit_cast<double>(output);
			inexact = std::isfinite(input) && input != result;
		}
		if (inexact)
			cpu.fpsr |= 1U << 4;
	}
	detail::set_vector_half(cpu.vectors[destination], 0, output);
	detail::set_vector_half(cpu.vectors[destination], 1, 0);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_integer_convert(CpuState &cpu, GuestMemory &,
						std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool wide = (instruction & 0x8000'0000U) != 0;
	const unsigned type = (instruction >> 22) & 0x3U;
	const unsigned rounding_mode = (instruction >> 19) & 0x3U;
	const unsigned opcode = (instruction >> 16) & 0x7U;
	const unsigned source = (instruction >> 5) & 0x1fU;
	const unsigned destination = instruction & 0x1fU;
	const bool fixed_point = (instruction & 0x0020'0000U) == 0;
	const unsigned fractional_bits = fixed_point ?
		64U - ((instruction >> 10) & 0x3fU) : 0U;
	if (type > 1)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	if (fixed_point && ((!wide && fractional_bits > 32) ||
		((opcode <= 1 && rounding_mode != 3) ||
		 (opcode >= 2 && rounding_mode != 0))))
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	if (!fixed_point && opcode >= 4 &&
	    (opcode > 5 || rounding_mode != 0))
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	if (rounding_mode == 0 && (opcode == 2 || opcode == 3)) {
		const auto raw = cpu.read_register(source, false);
		const bool unsigned_source = opcode == 3;
		long double value;
		if (unsigned_source)
			value = wide ? static_cast<long double>(raw) :
				static_cast<long double>(static_cast<std::uint32_t>(raw));
		else
			value = wide ? static_cast<long double>(
				static_cast<std::int64_t>(raw)) :
				static_cast<long double>(static_cast<std::int32_t>(raw));
		value = std::ldexp(value, -static_cast<int>(fractional_bits));
		const auto bits = type == 0 ? static_cast<std::uint64_t>(
			std::bit_cast<std::uint32_t>(static_cast<float>(value))) :
			std::bit_cast<std::uint64_t>(static_cast<double>(value));
		detail::set_vector_half(cpu.vectors[destination], 0, bits);
		detail::set_vector_half(cpu.vectors[destination], 1, 0);
	} else if (opcode <= 1 || opcode == 4 || opcode == 5) {
		const auto raw = vector_half(cpu.vectors[source], 0);
		long double value = type == 0 ?
			static_cast<long double>(std::bit_cast<float>(
				static_cast<std::uint32_t>(raw))) :
			static_cast<long double>(std::bit_cast<double>(raw));
		value = std::ldexp(value, static_cast<int>(fractional_bits));
		const auto converted = convert_floating_to_integer(
			value, (opcode & 1U) != 0, wide ? 64 : 32,
			opcode >= 4 ? 4 : rounding_mode);
		cpu.fpsr |= converted.exceptions;
		cpu.write_register(destination,
			wide ? converted.value : converted.value & 0xffff'ffffU, false);
	} else {
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	}
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::backend::arm64
