// SPDX-License-Identifier: GPL-2.0-or-later
#include "../dispatcher.hpp"

#include "../detail/instruction_helpers.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace uml::backend::arm64 {

using detail::continue_after;
using detail::condition_holds;
using detail::vector_half;

namespace {

template<typename Floating>
[[nodiscard]] std::uint32_t compare_floating(Floating left, Floating right)
{
	if (std::isnan(left) || std::isnan(right))
		return 0x3000'0000U;
	if (left == right)
		return 0x6000'0000U;
	if (left < right)
		return 0x8000'0000U;
	return 0x2000'0000U;
}

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

[[nodiscard]] std::uint64_t convert_floating_to_integer(long double value,
							 bool unsigned_result,
							 unsigned bits,
							 unsigned rounding_mode)
{
	if (std::isnan(value))
		return 0;
	value = round_floating(value, rounding_mode);
	if (unsigned_result) {
		const auto maximum = bits == 64 ?
			static_cast<long double>(std::numeric_limits<std::uint64_t>::max()) :
			static_cast<long double>(std::numeric_limits<std::uint32_t>::max());
		if (value <= 0)
			return 0;
		if (value >= maximum)
			return bits == 64 ? ~std::uint64_t{0} : 0xffff'ffffULL;
		return static_cast<std::uint64_t>(value);
	}
	const auto minimum = bits == 64 ?
		static_cast<long double>(std::numeric_limits<std::int64_t>::min()) :
		static_cast<long double>(std::numeric_limits<std::int32_t>::min());
	const auto maximum = bits == 64 ?
		static_cast<long double>(std::numeric_limits<std::int64_t>::max()) :
		static_cast<long double>(std::numeric_limits<std::int32_t>::max());
	if (value <= minimum)
		return bits == 64 ? std::uint64_t{1} << 63 : 0x8000'0000ULL;
	if (value >= maximum)
		return bits == 64 ? 0x7fff'ffff'ffff'ffffULL : 0x7fff'ffffULL;
	return static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

} // namespace

RunResult Dispatcher::floating_compare(CpuState &cpu, GuestMemory &,
				       std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned type = (instruction >> 22) & 3U;
	if (type > 1)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const unsigned left_register = (instruction >> 5) & 0x1fU;
	const unsigned right_register = (instruction >> 16) & 0x1fU;
	const bool compare_zero = (instruction & 8U) != 0;
	if (type == 0) {
		const auto left = std::bit_cast<float>(static_cast<std::uint32_t>(
			vector_half(cpu.vectors[left_register], 0)));
		const auto right = compare_zero ? 0.0F :
			std::bit_cast<float>(static_cast<std::uint32_t>(
				vector_half(cpu.vectors[right_register], 0)));
		cpu.nzcv = compare_floating(left, right);
	} else {
		const auto left = std::bit_cast<double>(
			vector_half(cpu.vectors[left_register], 0));
		const auto right = compare_zero ? 0.0 : std::bit_cast<double>(
			vector_half(cpu.vectors[right_register], 0));
		cpu.nzcv = compare_floating(left, right);
	}
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_conditional_compare(CpuState &cpu,
						    GuestMemory &,
						    std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned type = (instruction >> 22) & 0x3U;
	if (type > 1)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	if (!condition_holds(cpu.nzcv, (instruction >> 12) & 0xfU)) {
		cpu.nzcv = (instruction & 0xfU) << 28;
	} else {
		const unsigned left_register = (instruction >> 5) & 0x1fU;
		const unsigned right_register = (instruction >> 16) & 0x1fU;
		if (type == 0) {
			const auto left = std::bit_cast<float>(
				static_cast<std::uint32_t>(vector_half(
					cpu.vectors[left_register], 0)));
			const auto right = std::bit_cast<float>(
				static_cast<std::uint32_t>(vector_half(
					cpu.vectors[right_register], 0)));
			cpu.nzcv = compare_floating(left, right);
		} else {
			const auto left = std::bit_cast<double>(
				vector_half(cpu.vectors[left_register], 0));
			const auto right = std::bit_cast<double>(
				vector_half(cpu.vectors[right_register], 0));
			cpu.nzcv = compare_floating(left, right);
		}
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
	if (type > 1)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const unsigned source = condition_holds(cpu.nzcv,
		(instruction >> 12) & 0xfU) ? (instruction >> 5) & 0x1fU :
		(instruction >> 16) & 0x1fU;
	std::uint64_t bits = vector_half(cpu.vectors[source], 0);
	if (type == 0)
		bits &= 0xffff'ffffU;
	const unsigned destination = instruction & 0x1fU;
	detail::set_vector_half(cpu.vectors[destination], 0, bits);
	detail::set_vector_half(cpu.vectors[destination], 1, 0);
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
	const bool is_double = (instruction & 0x0040'0000U) != 0;
	const unsigned destination = instruction & 0x1fU;
	std::uint64_t bits;
	if ((instruction & 0xff20'1fe0U) == 0x1e20'1000U) {
		const auto immediate = static_cast<std::uint8_t>(
			((instruction >> 13) & 0xffU) ^ 0x40U);
		const bool negative = (immediate & 0x80U) != 0;
		const unsigned exponent = (immediate >> 4) & 0x7U;
		const unsigned fraction = immediate & 0xfU;
		double value = static_cast<double>(16 + fraction) /
			static_cast<double>(1U << (7 - exponent));
		if (negative)
			value = -value;
		bits = is_double ? std::bit_cast<std::uint64_t>(value) :
			std::bit_cast<std::uint32_t>(static_cast<float>(value));
	} else if ((instruction & 0xffbf'fc00U) == 0x1e20'4000U) {
		bits = vector_half(cpu.vectors[(instruction >> 5) & 0x1fU], 0);
	} else {
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	}
	if (!is_double)
		bits &= 0xffff'ffffU;
	detail::set_vector_half(cpu.vectors[destination], 0, bits);
	detail::set_vector_half(cpu.vectors[destination], 1, 0);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_arithmetic(CpuState &cpu, GuestMemory &,
					  std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned type = (instruction >> 22) & 0x3U;
	if (type > 1)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const unsigned source = (instruction >> 5) & 0x1fU;
	const unsigned destination = instruction & 0x1fU;
	const bool one_source =
		(instruction & 0xff20'7c00U) == 0x1e20'4000U;
	const unsigned unary_operation = (instruction >> 15) & 0x3fU;
	if (one_source && (unary_operation == 4 || unary_operation == 5)) {
		std::uint64_t converted;
		if (unary_operation == 4 && type == 1) {
			const auto value = static_cast<float>(std::bit_cast<double>(
				vector_half(cpu.vectors[source], 0)));
			converted = std::bit_cast<std::uint32_t>(value);
		} else if (unary_operation == 5 && type == 0) {
			const auto value = static_cast<double>(std::bit_cast<float>(
				static_cast<std::uint32_t>(vector_half(
					cpu.vectors[source], 0))));
			converted = std::bit_cast<std::uint64_t>(value);
		} else {
			return {StopReason::unsupported_instruction, address, instruction,
				0, false};
		}
		detail::set_vector_half(cpu.vectors[destination], 0, converted);
		detail::set_vector_half(cpu.vectors[destination], 1, 0);
		cpu.pc += 4;
		return continue_after(address, instruction);
	}
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
		cpu.write_register(destination,
			wide ? converted : converted & 0xffff'ffffU, false);
	} else {
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	}
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::backend::arm64
