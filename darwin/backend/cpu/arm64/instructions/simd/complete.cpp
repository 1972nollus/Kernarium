// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/floating_compare.hpp"
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

#pragma STDC FENV_ACCESS ON

namespace uml::backend::arm64 {

using detail::continue_after;
using detail::decode_little_endian;
using detail::encode_little_endian;

namespace {

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

void record_saturation(CpuState &cpu, bool saturated)
{
	if (saturated)
		cpu.fpsr |= 1U << 27; // QC, cumulative saturation flag.
}

std::uint64_t saturate_signed(CpuState &cpu, SignedWide value, unsigned bits)
{
	const auto minimum = signed_minimum(bits);
	const auto maximum = signed_maximum(bits);
	const bool saturated = value < minimum || value > maximum;
	value = std::clamp(value, minimum, maximum);
	record_saturation(cpu, saturated);
	return static_cast<std::uint64_t>(value) & mask_for(bits);
}

std::uint64_t saturate_unsigned(CpuState &cpu, SignedWide value, unsigned bits)
{
	const auto maximum = static_cast<UnsignedWide>(mask_for(bits));
	const bool below = value < 0;
	const bool above = !below && static_cast<UnsignedWide>(value) > maximum;
	record_saturation(cpu, below || above);
	if (below)
		return 0;
	if (above)
		return mask_for(bits);
	return static_cast<std::uint64_t>(value);
}

std::uint64_t rounded_shift_right_signed(std::uint64_t raw, unsigned bits,
					 unsigned shift, bool rounding)
{
	SignedWide value = as_signed(raw, bits);
	if (shift > bits)
		return !rounding && value < 0 ? mask_for(bits) : 0;
	if (rounding && shift != 0)
		value += SignedWide{1} << (shift - 1);
	if (shift >= bits)
		return value < 0 ? mask_for(bits) : 0;
	return static_cast<std::uint64_t>(value >> shift) & mask_for(bits);
}

std::uint64_t rounded_shift_right_unsigned(std::uint64_t value, unsigned bits,
					   unsigned shift, bool rounding)
{
	UnsignedWide wide = value & mask_for(bits);
	if (shift > bits)
		return 0;
	if (rounding && shift != 0)
		wide += UnsignedWide{1} << (shift - 1);
	return shift >= bits ? static_cast<std::uint64_t>(wide >> bits) :
		static_cast<std::uint64_t>(wide >> shift) & mask_for(bits);
}

std::uint64_t polynomial_multiply_byte(std::uint64_t left, std::uint64_t right)
{
	std::uint64_t product = 0;
	for (unsigned bit = 0; bit < 8; ++bit)
		if ((right & (1U << bit)) != 0)
			product ^= left << bit;
	return product & 0xffU;
}

constexpr bool valid_integer_two_register_misc(unsigned opcode,
	bool unsigned_group, bool scalar, bool q, unsigned size)
{
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

std::uint64_t floating_to_integer(long double value, bool unsigned_result,
					 unsigned bits, unsigned rounding)
{
	if (std::isnan(value))
		return 0;
	switch (rounding) {
	case 0: value = std::nearbyint(value); break;
	case 1: value = std::ceil(value); break;
	case 2: value = std::floor(value); break;
	case 4: value = std::round(value); break;
	default: value = std::trunc(value); break;
	}
	if (unsigned_result) {
		const auto maximum = bits == 64 ?
			static_cast<long double>(std::numeric_limits<std::uint64_t>::max()) :
			static_cast<long double>(std::numeric_limits<std::uint32_t>::max());
		if (value <= 0)
			return 0;
		if (value >= maximum)
			return mask_for(bits);
		return static_cast<std::uint64_t>(value);
	}
	const auto minimum = bits == 64 ?
		static_cast<long double>(std::numeric_limits<std::int64_t>::min()) :
		static_cast<long double>(std::numeric_limits<std::int32_t>::min());
	const auto maximum = bits == 64 ?
		static_cast<long double>(std::numeric_limits<std::int64_t>::max()) :
		static_cast<long double>(std::numeric_limits<std::int32_t>::max());
	if (value <= minimum)
		return std::uint64_t{1} << (bits - 1);
	if (value >= maximum)
		return (std::uint64_t{1} << (bits - 1)) - 1;
	return static_cast<std::uint64_t>(static_cast<std::int64_t>(value)) &
		mask_for(bits);
}

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
	Floating accumulator)
{
	if constexpr (sizeof(Floating) == 2)
		return __builtin_fmaf16(left, right, accumulator);
	else
		return std::fma(left, right, accumulator);
}

} // namespace

RunResult Dispatcher::simd_dot_product(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned size = (instruction >> 22) & 3U;
	if (size != 2U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const bool ordinary =
		(instruction & 0x9f20'fc00U) == 0x0e00'9400U;
	const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
	const bool left_signed = ordinary && !unsigned_group;
	const bool right_signed = ordinary ? !unsigned_group : true;
	const unsigned vector_bytes =
		(instruction & 0x4000'0000U) != 0 ? 16U : 8U;
	const auto left = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 31U].bytes;
	const auto accumulator = cpu.vectors[instruction & 31U].bytes;
	std::array<std::byte, 16> result{};
	for (unsigned group = 0; group < vector_bytes / 4U; ++group) {
		std::int64_t sum = 0;
		for (unsigned byte = 0; byte < 4U; ++byte) {
			const unsigned offset = group * 4U + byte;
			const auto left_raw = std::to_integer<std::uint8_t>(left[offset]);
			const auto right_raw = std::to_integer<std::uint8_t>(right[offset]);
			const std::int64_t left_value = left_signed ?
				static_cast<std::int8_t>(left_raw) : left_raw;
			const std::int64_t right_value = right_signed ?
				static_cast<std::int8_t>(right_raw) : right_raw;
			sum += left_value * right_value;
		}
		const auto old = decode_little_endian(
			std::span{accumulator}.subspan(group * 4U, 4U));
		encode_little_endian(static_cast<std::uint32_t>(old + sum),
			std::span{result}.subspan(group * 4U, 4U));
	}
	cpu.vectors[instruction & 31U].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_matrix_multiply(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool mixed =
		(instruction & 0xffe0'fc00U) == 0x4e80'ac00U;
	const bool both_unsigned = !mixed &&
		(instruction & 0x2000'0000U) != 0;
	const bool left_unsigned = mixed || both_unsigned;
	const bool right_unsigned = both_unsigned;
	const unsigned destination = instruction & 31U;
	const auto left = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 31U].bytes;
	const auto accumulator = cpu.vectors[destination].bytes;
	std::array<std::byte, 16> result{};
	for (unsigned row = 0; row < 2U; ++row)
		for (unsigned column = 0; column < 2U; ++column) {
			std::int64_t sum = 0;
			for (unsigned k = 0; k < 8U; ++k) {
				const auto left_raw = std::to_integer<std::uint8_t>(
					left[row * 8U + k]);
				const auto right_raw = std::to_integer<std::uint8_t>(
					right[column * 8U + k]);
				const std::int64_t left_value = left_unsigned ? left_raw :
					static_cast<std::int8_t>(left_raw);
				const std::int64_t right_value = right_unsigned ? right_raw :
					static_cast<std::int8_t>(right_raw);
				sum += left_value * right_value;
			}
			const unsigned lane = row * 2U + column;
			const auto old = decode_little_endian(
				std::span{accumulator}.subspan(lane * 4U, 4U));
			encode_little_endian(static_cast<std::uint32_t>(old + sum),
				std::span{result}.subspan(lane * 4U, 4U));
		}
	cpu.vectors[destination].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_floating_multiply_extended(CpuState &cpu,
	GuestMemory &, std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const bool q = (instruction & 0x4000'0000U) != 0;
	const bool half = (instruction & 0x00e0'0000U) == 0x0040'0000U;
	const bool double_precision = !half &&
		(instruction & 0x0040'0000U) != 0;
	if (double_precision && !scalar && !q)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = instruction & 31U;
	const auto left = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 31U].bytes;
	const std::size_t lane_bytes = half ? 2U : double_precision ? 8U : 4U;
	const std::size_t vector_bytes = scalar ? lane_bytes : q ? 16U : 8U;
	std::array<std::byte, 16> result{};
	if (scalar && (cpu.fpcr & 4U) != 0)
		result = left;
	auto execute = [&]<typename Floating>() {
		using Integer = std::conditional_t<sizeof(Floating) == 8,
			std::uint64_t, std::conditional_t<sizeof(Floating) == 4,
			std::uint32_t, std::uint16_t>>;
		for (std::size_t lane = 0; lane < vector_bytes / lane_bytes; ++lane) {
			const auto first = std::bit_cast<Floating>(
				static_cast<Integer>(decode_little_endian(
					std::span{left}.subspan(lane * lane_bytes, lane_bytes))));
			const auto second = std::bit_cast<Floating>(
				static_cast<Integer>(decode_little_endian(
					std::span{right}.subspan(lane * lane_bytes, lane_bytes))));
			const auto value = multiply_extended(first, second);
			encode_little_endian(std::bit_cast<Integer>(value),
				std::span{result}.subspan(lane * lane_bytes, lane_bytes));
		}
	};
	if (half)
		execute.template operator()<_Float16>();
	else if (double_precision)
		execute.template operator()<double>();
	else
		execute.template operator()<float>();
	cpu.vectors[destination].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_saturating_rounding_multiply_accumulate(
	CpuState &cpu, GuestMemory &, std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned size = (instruction >> 22) & 3U;
	if (size != 1U && size != 2U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const bool subtract = (instruction & 0x800U) != 0;
	const unsigned lane_bytes = 1U << size;
	const unsigned lane_bits = lane_bytes * 8U;
	const unsigned vector_bytes = scalar ? lane_bytes :
		(instruction & 0x4000'0000U) != 0 ? 16U : 8U;
	const auto left = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 31U].bytes;
	const auto accumulator = cpu.vectors[instruction & 31U].bytes;
	std::array<std::byte, 16> result{};
	for (unsigned offset = 0; offset < vector_bytes; offset += lane_bytes) {
		const auto left_raw = decode_little_endian(
			std::span{left}.subspan(offset, lane_bytes));
		const auto right_raw = decode_little_endian(
			std::span{right}.subspan(offset, lane_bytes));
		const auto accumulator_raw = decode_little_endian(
			std::span{accumulator}.subspan(offset, lane_bytes));
		const SignedWide product = as_signed(left_raw, lane_bits) *
			as_signed(right_raw, lane_bits) * 2;
		SignedWide combined = as_signed(accumulator_raw, lane_bits) *
			(SignedWide{1} << lane_bits);
		combined += subtract ? -product : product;
		combined += SignedWide{1} << (lane_bits - 1U);
		const auto value = saturate_signed(
			cpu, combined >> lane_bits, lane_bits);
		encode_little_endian(value,
			std::span{result}.subspan(offset, lane_bytes));
	}
	cpu.vectors[instruction & 31U].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_three_same(CpuState &cpu, GuestMemory &memory,
				      std::uint32_t instruction) const
{
	if (detail::decode_simd_floating_compare(instruction).recognized)
		return simd_floating_compare(cpu, memory, instruction);
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 11) & 0x1fU;
	if (opcode >= 24)
		return simd_floating_arithmetic(cpu, memory, instruction);
	if (opcode == 3)
		return simd_bitwise_register(cpu, memory, instruction);

	const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const unsigned size = (instruction >> 22) & 0x3U;
	const std::size_t lane_size = std::size_t{1} << size;
	const unsigned lane_bits = static_cast<unsigned>(lane_size * 8);
	const std::size_t vector_size = scalar ? lane_size :
		(instruction & 0x4000'0000U) != 0 ? 16 : 8;
	if (lane_size > vector_size || (!scalar && lane_size == 8 && vector_size != 16))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (scalar) {
		const bool all_width_saturating = opcode == 1 || opcode == 5 ||
			opcode == 9 || opcode == 11;
		const bool d_only = opcode == 6 || opcode == 7 || opcode == 8 ||
			opcode == 10 || opcode == 16 || opcode == 17;
		const bool doubling = opcode == 22 && (size == 1 || size == 2);
		if (!all_width_saturating && !(d_only && size == 3) && !doubling)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
	}
	if (lane_size == 8 && opcode != 1 && opcode != 5 && opcode != 6 &&
	    opcode != 7 && opcode != 8 && opcode != 9 && opcode != 10 &&
	    opcode != 11 && opcode != 16 && opcode != 17 && opcode != 23)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	/* PMUL is allocated only for byte elements in this class. */
	if (opcode == 19 && unsigned_group && size != 0)
		return {StopReason::illegal_instruction, address, instruction, 0, false};
	if (opcode == 22 && size == 0)
		return {StopReason::illegal_instruction, address, instruction, 0, false};
	if (opcode == 23 && unsigned_group)
		return {StopReason::illegal_instruction, address, instruction, 0, false};

	const auto left = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	const auto old_destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	const auto read_lane = [&](const auto &bytes, std::size_t lane) {
		return decode_little_endian(std::span{bytes}.subspan(
			lane * lane_size, lane_size));
	};
	const auto write_lane = [&](std::size_t lane, std::uint64_t value) {
		encode_little_endian(value & mask_for(lane_bits),
			std::span{result}.subspan(lane * lane_size, lane_size));
	};
	const std::size_t lanes = vector_size / lane_size;
	if (!scalar && (opcode == 20 || opcode == 21 || opcode == 23)) {
		for (std::size_t output = 0; output < lanes; ++output) {
			const auto &source = output < lanes / 2 ? left : right;
			const auto pair = (output % (lanes / 2)) * 2;
			const auto first = read_lane(source, pair);
			const auto second = read_lane(source, pair + 1);
			std::uint64_t value;
			if (opcode == 23)
				value = first + second;
			else if (unsigned_group)
				value = opcode == 20 ? std::max(first, second) :
					std::min(first, second);
			else {
				const auto a = as_signed(first, lane_bits);
				const auto b = as_signed(second, lane_bits);
				value = static_cast<std::uint64_t>(opcode == 20 ?
					std::max(a, b) : std::min(a, b));
			}
			write_lane(output, value);
		}
	} else {
		for (std::size_t lane = 0; lane < lanes; ++lane) {
			const auto first = read_lane(left, lane);
			const auto second = read_lane(right, lane);
			const auto old = read_lane(old_destination, lane);
			std::uint64_t value;
			switch (opcode) {
			case 0: {
				if (unsigned_group)
					value = (first + second) >> 1;
				else
					value = static_cast<std::uint64_t>(
						(as_signed(first, lane_bits) +
						 as_signed(second, lane_bits)) >> 1);
				break;
			}
			case 1:
				value = unsigned_group ? saturate_unsigned(cpu,
					static_cast<SignedWide>(first) + second, lane_bits) :
					saturate_signed(cpu, as_signed(first, lane_bits) +
						as_signed(second, lane_bits), lane_bits);
				break;
			case 2:
				if (unsigned_group)
					value = (first + second + 1) >> 1;
				else
					value = static_cast<std::uint64_t>(
						(as_signed(first, lane_bits) +
						 as_signed(second, lane_bits) + 1) >> 1);
				break;
			case 4:
				if (unsigned_group)
					value = (first - second) >> 1;
				else
					value = static_cast<std::uint64_t>(
						(as_signed(first, lane_bits) -
						 as_signed(second, lane_bits)) >> 1);
				break;
			case 5:
				value = unsigned_group ? saturate_unsigned(cpu,
					static_cast<SignedWide>(first) - second, lane_bits) :
					saturate_signed(cpu, as_signed(first, lane_bits) -
						as_signed(second, lane_bits), lane_bits);
				break;
			case 6:
			case 7: {
				const bool match = unsigned_group ? (opcode == 6 ?
					first > second : first >= second) : (opcode == 6 ?
					as_signed(first, lane_bits) > as_signed(second, lane_bits) :
					as_signed(first, lane_bits) >= as_signed(second, lane_bits));
				value = match ? mask_for(lane_bits) : 0;
				break;
			}
			case 8:
			case 10: {
				const int shift = static_cast<std::int8_t>(second & 0xffU);
				const bool rounding = opcode == 10;
				if (shift >= 0)
					value = static_cast<unsigned>(shift) >= lane_bits ? 0 :
						(first << shift) & mask_for(lane_bits);
				else if (unsigned_group)
					value = rounded_shift_right_unsigned(first, lane_bits,
						static_cast<unsigned>(-shift), rounding);
				else
					value = rounded_shift_right_signed(first, lane_bits,
						static_cast<unsigned>(-shift), rounding);
				break;
			}
			case 9:
			case 11: {
				const int shift = static_cast<std::int8_t>(second & 0xffU);
				if (shift < 0)
					value = unsigned_group ? rounded_shift_right_unsigned(first,
						lane_bits, static_cast<unsigned>(-shift), opcode == 11) :
						rounded_shift_right_signed(first, lane_bits,
							static_cast<unsigned>(-shift), opcode == 11);
				else if (unsigned_group)
					value = static_cast<unsigned>(shift) >= lane_bits ?
						(first == 0 ? 0 : saturate_unsigned(cpu,
							SignedWide{1} << lane_bits, lane_bits)) : saturate_unsigned(cpu,
							static_cast<SignedWide>(first) *
								(SignedWide{1} << shift), lane_bits);
				else
					value = static_cast<unsigned>(shift) >= lane_bits ?
						(first == 0 ? 0 : saturate_signed(cpu,
							as_signed(first, lane_bits) < 0 ?
							signed_minimum(lane_bits) - 1 :
							signed_maximum(lane_bits) + 1, lane_bits)) :
						saturate_signed(cpu, as_signed(first, lane_bits) *
							(SignedWide{1} << shift), lane_bits);
				break;
			}
			case 12:
			case 13:
				if (unsigned_group)
					value = opcode == 12 ? std::max(first, second) :
						std::min(first, second);
				else {
					const auto a = as_signed(first, lane_bits);
					const auto b = as_signed(second, lane_bits);
					value = static_cast<std::uint64_t>(opcode == 12 ?
						std::max(a, b) : std::min(a, b));
				}
				break;
			case 14: {
				const SignedWide difference = unsigned_group ?
					(first >= second ? first - second : second - first) :
					absolute_wide(as_signed(first, lane_bits) -
						as_signed(second, lane_bits));
				value = static_cast<std::uint64_t>(difference);
				break;
			}
			case 15: {
				const SignedWide difference = unsigned_group ?
					(first >= second ? first - second : second - first) :
					absolute_wide(as_signed(first, lane_bits) -
						as_signed(second, lane_bits));
				value = old + static_cast<std::uint64_t>(difference);
				break;
			}
			case 16: value = unsigned_group ? first - second : first + second; break;
			case 17: value = unsigned_group ? (first == second ?
				mask_for(lane_bits) : 0) : ((first & second) != 0 ?
				mask_for(lane_bits) : 0); break;
			case 18: value = unsigned_group ? old - first * second :
				old + first * second; break;
			case 19: value = unsigned_group ?
				polynomial_multiply_byte(first, second) : first * second; break;
			case 22: {
				const auto product = as_signed(first, lane_bits) *
					as_signed(second, lane_bits) * 2 +
					(unsigned_group ? SignedWide{1} << (lane_bits - 1) : 0);
				value = saturate_signed(cpu, product >> lane_bits, lane_bits);
				break;
			}
			default:
				return {StopReason::illegal_instruction, address,
					instruction, 0, false};
			}
			write_lane(lane, value);
		}
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_two_register_misc(CpuState &cpu, GuestMemory &memory,
					      std::uint32_t instruction) const
{
	if (detail::decode_simd_floating_compare(instruction).recognized)
		return simd_floating_compare(cpu, memory, instruction);
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 12) & 0x1fU;
	const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const bool q = (instruction & 0x4000'0000U) != 0;
	const bool alternate = (instruction & 0x0080'0000U) != 0;
	const unsigned size = (instruction >> 22) & 0x3U;
	if (!valid_integer_two_register_misc(opcode, unsigned_group, scalar, q,
	    size))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (opcode == 28U && alternate && (scalar || size != 2U))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const std::size_t lane_size = std::size_t{1} << size;
	const unsigned lane_bits = static_cast<unsigned>(lane_size * 8);
	const std::size_t vector_size = scalar ? lane_size :
		(instruction & 0x4000'0000U) != 0 ? 16 : 8;
	const auto source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto old_destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};

	if (opcode == 2 || opcode == 6) { // SADDLP/UADDLP, SADALP/UADALP
		if (scalar || size == 3 || lane_size * 2 > vector_size)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		for (std::size_t input = 0, output = 0; input < vector_size;
		     input += lane_size * 2, output += lane_size * 2) {
			const auto first = decode_little_endian(
				std::span{source}.subspan(input, lane_size));
			const auto second = decode_little_endian(
				std::span{source}.subspan(input + lane_size, lane_size));
			std::uint64_t value = unsigned_group ? first + second :
				static_cast<std::uint64_t>(as_signed(first, lane_bits) +
					as_signed(second, lane_bits));
			if (opcode == 6)
				value += decode_little_endian(
					std::span{old_destination}.subspan(output, lane_size * 2));
			encode_little_endian(value,
				std::span{result}.subspan(output, lane_size * 2));
		}
	} else if (opcode == 3) { // SUQADD / USQADD
		for (std::size_t offset = 0; offset < vector_size; offset += lane_size) {
			const auto input = decode_little_endian(
				std::span{source}.subspan(offset, lane_size));
			const auto destination = decode_little_endian(
				std::span{old_destination}.subspan(offset, lane_size));
			const auto value = unsigned_group ? saturate_unsigned(cpu,
				static_cast<SignedWide>(destination) + as_signed(input, lane_bits),
				lane_bits) : saturate_signed(cpu, as_signed(destination, lane_bits) +
					static_cast<SignedWide>(input), lane_bits);
			encode_little_endian(value,
				std::span{result}.subspan(offset, lane_size));
		}
	} else if (opcode == 4) { // CLS / CLZ
		if (scalar)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		for (std::size_t offset = 0; offset < vector_size; offset += lane_size) {
			const auto input = decode_little_endian(
				std::span{source}.subspan(offset, lane_size));
			unsigned count = 0;
			if (unsigned_group) {
				for (int bit = static_cast<int>(lane_bits) - 1; bit >= 0 &&
				     ((input >> bit) & 1U) == 0; --bit)
					++count;
			} else {
				const auto sign = (input >> (lane_bits - 1)) & 1U;
				for (int bit = static_cast<int>(lane_bits) - 2; bit >= 0 &&
				     ((input >> bit) & 1U) == sign; --bit)
					++count;
			}
			encode_little_endian(count,
				std::span{result}.subspan(offset, lane_size));
		}
	} else if (opcode == 7 || opcode == 11) { // SQABS/SQNEG, ABS/NEG
		if (scalar && opcode == 11 && size != 3)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		for (std::size_t offset = 0; offset < vector_size; offset += lane_size) {
			const auto input = decode_little_endian(
				std::span{source}.subspan(offset, lane_size));
			const auto signed_input = as_signed(input, lane_bits);
			const auto transformed = unsigned_group ? -signed_input :
				(signed_input < 0 ? -signed_input : signed_input);
			const auto value = opcode == 7 ? saturate_signed(cpu, transformed,
				lane_bits) : static_cast<std::uint64_t>(transformed) &
				mask_for(lane_bits);
			encode_little_endian(value,
				std::span{result}.subspan(offset, lane_size));
		}
	} else if (opcode == 12 || opcode == 13 || opcode == 14) {
		// Floating comparisons with zero.
		const bool is_double = (instruction & 0x0040'0000U) != 0;
		const std::size_t fp_size = is_double ? 8 : 4;
		const std::size_t bytes = scalar ? fp_size : vector_size;
		if ((!scalar && is_double && vector_size != 16) || lane_size < fp_size)
			return {StopReason::unsupported_instruction, address, instruction, 0,
				false};
		for (std::size_t offset = 0; offset < bytes; offset += fp_size) {
			bool match;
			if (is_double) {
				const auto value = std::bit_cast<double>(decode_little_endian(
					std::span{source}.subspan(offset, 8)));
				match = opcode == 12 ? (unsigned_group ? value >= 0 : value > 0) :
					opcode == 13 ? (unsigned_group ? value <= 0 : value == 0) :
					value < 0;
			} else {
				const auto value = std::bit_cast<float>(static_cast<std::uint32_t>(
					decode_little_endian(std::span{source}.subspan(offset, 4))));
				match = opcode == 12 ? (unsigned_group ? value >= 0 : value > 0) :
					opcode == 13 ? (unsigned_group ? value <= 0 : value == 0) :
					value < 0;
			}
			encode_little_endian(match ? mask_for(fp_size * 8) : 0,
				std::span{result}.subspan(offset, fp_size));
		}
	} else if (opcode == 18 || opcode == 20) {
		const std::size_t source_size = lane_size * 2;
		if (size == 3 || (scalar && opcode == 18 && !unsigned_group))
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		const std::size_t output_offset = !scalar &&
			(instruction & 0x4000'0000U) != 0 ? 8 : 0;
		if (output_offset != 0)
			std::copy_n(old_destination.begin(), 8, result.begin());
		for (std::size_t input = 0, output = output_offset;
		     input < (scalar ? source_size : 16);
		     input += source_size, output += lane_size) {
			const auto raw = decode_little_endian(
				std::span{source}.subspan(input, source_size));
			std::uint64_t value;
			if (opcode == 18)
				value = saturate_unsigned(cpu, as_signed(raw, source_size * 8),
					lane_bits); // SQXTUN
			else if (unsigned_group)
				value = saturate_unsigned(cpu, static_cast<SignedWide>(raw),
					lane_bits); // UQXTN
			else
				value = saturate_signed(cpu, as_signed(raw, source_size * 8),
					lane_bits); // SQXTN
			encode_little_endian(value,
				std::span{result}.subspan(output, lane_size));
		}
	} else if (opcode == 19 && unsigned_group && !scalar) { // SHLL
		if (size == 3)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		const std::size_t source_base =
			(instruction & 0x4000'0000U) != 0 ? 8 : 0;
		for (std::size_t input = source_base, output = 0; output < 16;
		     input += lane_size, output += lane_size * 2) {
			const auto value = decode_little_endian(
				std::span{source}.subspan(input, lane_size)) << lane_bits;
			encode_little_endian(value,
				std::span{result}.subspan(output, lane_size * 2));
		}
	} else if (opcode == 22 || opcode == 23) { // FCVTN/FCVTXN, FCVTL
		if (scalar || size != 1)
			return {StopReason::unsupported_instruction, address, instruction, 0,
				false};
		if (opcode == 23) {
			const std::size_t source_base =
				(instruction & 0x4000'0000U) != 0 ? 8 : 0;
			for (std::size_t lane = 0; lane < 2; ++lane) {
				const auto value = std::bit_cast<float>(static_cast<std::uint32_t>(
					decode_little_endian(std::span{source}.subspan(
						source_base + lane * 4, 4))));
				encode_little_endian(std::bit_cast<std::uint64_t>(
					static_cast<double>(value)),
					std::span{result}.subspan(lane * 8, 8));
			}
		} else {
			const std::size_t output_base =
				(instruction & 0x4000'0000U) != 0 ? 8 : 0;
			if (output_base != 0)
				std::copy_n(old_destination.begin(), 8, result.begin());
			for (std::size_t lane = 0; lane < 2; ++lane) {
				const auto value = std::bit_cast<double>(decode_little_endian(
					std::span{source}.subspan(lane * 8, 8)));
				const auto narrowed = unsigned_group ? round_to_odd<float>(value) :
					static_cast<float>(value);
				encode_little_endian(std::bit_cast<std::uint32_t>(narrowed),
					std::span{result}.subspan(output_base + lane * 4, 4));
			}
		}
	} else if (opcode >= 24 && opcode <= 29) {
		const bool is_double = (instruction & 0x0040'0000U) != 0;
		const std::size_t fp_size = is_double ? 8 : 4;
		const std::size_t bytes = scalar ? fp_size : vector_size;
		if ((!scalar && is_double && vector_size != 16) || fp_size > bytes)
			return {StopReason::unsupported_instruction, address, instruction, 0,
				false};
		for (std::size_t offset = 0; offset < bytes; offset += fp_size) {
			long double input = is_double ? static_cast<long double>(
				std::bit_cast<double>(decode_little_endian(
					std::span{source}.subspan(offset, 8)))) :
				static_cast<long double>(std::bit_cast<float>(
					static_cast<std::uint32_t>(decode_little_endian(
						std::span{source}.subspan(offset, 4)))));
			if (opcode == 24 || opcode == 25) {
				const unsigned mode = opcode == 24 ? (unsigned_group ? 4 :
					alternate ? 1 : 0) : unsigned_group ? (alternate ?
					(cpu.fpcr >> 22) & 3U : (cpu.fpcr >> 22) & 3U) :
					alternate ? 3 : 2;
				const auto value = is_double ? static_cast<std::uint64_t>(
					std::bit_cast<std::uint64_t>(round_integral(
						static_cast<double>(input), mode))) :
					std::bit_cast<std::uint32_t>(round_integral(
						static_cast<float>(input), mode));
				encode_little_endian(value,
					std::span{result}.subspan(offset, fp_size));
			} else if ((opcode == 28 || opcode == 29) && alternate) {
				long double estimate;
				if (opcode == 28) { // URECPE / URSQRTE
					const auto raw = static_cast<std::uint32_t>(
						decode_little_endian(std::span{source}.subspan(offset, 4)));
					const auto integer_estimate = unsigned_group ?
						unsigned_reciprocal_sqrt_estimate(raw) :
						unsigned_reciprocal_estimate(raw);
					encode_little_endian(integer_estimate,
						std::span{result}.subspan(offset, 4));
					continue;
				} else if (unsigned_group)
					estimate = 1 / std::sqrt(input);
				else
					estimate = 1 / input;
				const auto value = is_double ? std::bit_cast<std::uint64_t>(
					static_cast<double>(estimate)) : std::bit_cast<std::uint32_t>(
					static_cast<float>(estimate));
				encode_little_endian(value,
					std::span{result}.subspan(offset, fp_size));
			} else {
				const unsigned rounding = opcode == 26 ? (alternate ? 1 : 0) :
					opcode == 27 ? (alternate ? 3 : 2) : opcode == 28 ? 4 : 0;
				if (opcode == 29) {
					const auto raw = decode_little_endian(
						std::span{source}.subspan(offset, fp_size));
					input = unsigned_group ? static_cast<long double>(raw) :
						static_cast<long double>(as_signed(raw, fp_size * 8));
					const auto value = is_double ? std::bit_cast<std::uint64_t>(
						static_cast<double>(input)) : std::bit_cast<std::uint32_t>(
						static_cast<float>(input));
					encode_little_endian(value,
						std::span{result}.subspan(offset, fp_size));
				} else {
					encode_little_endian(floating_to_integer(input,
						unsigned_group, fp_size * 8, rounding),
						std::span{result}.subspan(offset, fp_size));
				}
			}
		}
	} else if (opcode == 31 && scalar && !unsigned_group && alternate) {
		const bool is_double = (instruction & 0x0040'0000U) != 0;
		if (!is_double) {
			const auto value = reciprocal_exponent(std::bit_cast<float>(
				static_cast<std::uint32_t>(decode_little_endian(
					std::span{source}.first(4)))));
			encode_little_endian(std::bit_cast<std::uint32_t>(value),
				std::span{result}.first(4));
		} else {
			const auto value = reciprocal_exponent(std::bit_cast<double>(
				decode_little_endian(std::span{source}.first(8))));
			encode_little_endian(std::bit_cast<std::uint64_t>(value),
				std::span{result}.first(8));
		}
	} else {
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_across_lanes(CpuState &cpu, GuestMemory &,
					std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 12) & 0x1fU;
	if (opcode != 12 && opcode != 15)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const bool alternate = (instruction & 0x0080'0000U) != 0;
	const bool is_double = (instruction & 0x0040'0000U) != 0;
	if (is_double || (instruction & 0x4000'0000U) == 0)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU];
	float value = read_floating_lane<float>(source, 0);
	for (std::size_t lane = 1; lane < 4; ++lane) {
		const auto next = read_floating_lane<float>(source, lane);
		if (opcode == 12)
			value = alternate ? std::fmin(value, next) : std::fmax(value, next);
		else if (std::isnan(value) || std::isnan(next))
			value = std::numeric_limits<float>::quiet_NaN();
		else
			value = alternate ? std::min(value, next) : std::max(value, next);
	}
	auto &destination = cpu.vectors[instruction & 0x1fU];
	destination.bytes.fill(std::byte{});
	write_floating_lane(destination, 0, value);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_scalar_pairwise(CpuState &cpu, GuestMemory &,
					    std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 12) & 0x1fU;
	if (opcode != 12 && opcode != 13 && opcode != 15)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const bool alternate = (instruction & 0x0080'0000U) != 0;
	const bool is_double = (instruction & 0x0040'0000U) != 0;
	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU];
	auto &destination = cpu.vectors[instruction & 0x1fU];
	destination.bytes.fill(std::byte{});
	if (is_double) {
		const auto first = read_floating_lane<double>(source, 0);
		const auto second = read_floating_lane<double>(source, 1);
		double value;
		if (opcode == 13)
			value = first + second;
		else if (opcode == 12)
			value = alternate ? std::fmin(first, second) : std::fmax(first, second);
		else
			value = alternate ? std::min(first, second) : std::max(first, second);
		write_floating_lane(destination, 0, value);
	} else {
		const auto first = read_floating_lane<float>(source, 0);
		const auto second = read_floating_lane<float>(source, 1);
		float value;
		if (opcode == 13)
			value = first + second;
		else if (opcode == 12)
			value = alternate ? std::fmin(first, second) : std::fmax(first, second);
		else
			value = alternate ? std::min(first, second) : std::max(first, second);
		write_floating_lane(destination, 0, value);
	}
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_by_indexed_element(CpuState &cpu, GuestMemory &,
					       std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const bool q = (instruction & 0x4000'0000U) != 0;
	const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
	const unsigned size = (instruction >> 22) & 0x3U;
	const unsigned operation = (instruction >> 12) & 0xfU;
	const bool integer_group = scalar ?
		(unsigned_group ? operation == 13U || operation == 15U :
		 operation == 3U || operation == 7U || operation == 11U ||
		 operation == 12U || operation == 13U) :
		(unsigned_group ? operation == 0U || operation == 2U ||
		 operation == 4U || operation == 6U || operation == 10U ||
		 operation == 13U || operation == 15U :
		 operation == 2U || operation == 3U || operation == 6U ||
		 operation == 7U || operation == 8U || operation == 10U ||
		 operation == 11U || operation == 12U || operation == 13U);
	const bool floating = operation == 9U || (!unsigned_group &&
		(operation == 1U || operation == 5U));
	const bool ordinary_dot = !scalar && operation == 14U;
	const bool mixed_dot = !scalar && !unsigned_group && operation == 15U &&
		(size == 0U || size == 2U);
	const bool dot = ordinary_dot || mixed_dot;
	const bool scalar_allocated_operation = unsigned_group ?
		operation == 9U || operation == 13U || operation == 15U :
		operation == 1U || operation == 3U || operation == 5U ||
		operation == 7U || operation == 9U || operation == 11U ||
		operation == 12U || operation == 13U;
	if (integer_group && size != 1U && size != 2U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (ordinary_dot && size != 2U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (scalar && (!scalar_allocated_operation ||
	    ((operation == 1U || operation == 5U || operation == 9U) &&
	     size == 1U)))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (!integer_group && !floating && !dot)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	if (floating && size == 1U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (floating && size == 3U && !scalar && !q)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (floating && size == 3U && (instruction & 0x0020'0000U) != 0)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	unsigned element_register = (instruction >> 16) & 0xfU;
	unsigned element_index;
	if (floating && size == 0U) {
		element_index = ((instruction >> 9) & 4U) |
			((instruction >> 20) & 1U) |
			((instruction >> 20) & 2U);
	} else if (size == 1U) {
		element_index = ((instruction >> 9) & 4U) |
			((instruction >> 20) & 1U) |
			((instruction >> 20) & 2U);
	} else {
		element_register |= ((instruction >> 20) & 1U) << 4;
		element_index = ((instruction >> 10) & 2U) |
			((instruction >> 21) & 1U);
		if (size == 3)
			element_index >>= 1;
	}
	const std::size_t narrow_size = floating && size == 0U ? 2U :
		std::size_t{1} << size;
	if (element_index * narrow_size + narrow_size > 16)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (dot) {
		const bool left_signed = ordinary_dot ? !unsigned_group : size == 0U;
		const bool right_signed = ordinary_dot ? !unsigned_group : size == 2U;
		const unsigned vector_bytes = q ? 16U : 8U;
		const auto left = cpu.vectors[(instruction >> 5) & 31U].bytes;
		const auto right = cpu.vectors[element_register].bytes;
		const auto accumulator = cpu.vectors[instruction & 31U].bytes;
		std::array<std::byte, 16> result{};
		for (unsigned group = 0; group < vector_bytes / 4U; ++group) {
			std::int64_t sum = 0;
			for (unsigned byte = 0; byte < 4U; ++byte) {
				const auto left_raw = std::to_integer<std::uint8_t>(
					left[group * 4U + byte]);
				const auto right_raw = std::to_integer<std::uint8_t>(
					right[element_index * 4U + byte]);
				const std::int64_t left_value = left_signed ?
					static_cast<std::int8_t>(left_raw) : left_raw;
				const std::int64_t right_value = right_signed ?
					static_cast<std::int8_t>(right_raw) : right_raw;
				sum += left_value * right_value;
			}
			const auto old = decode_little_endian(
				std::span{accumulator}.subspan(group * 4U, 4U));
			encode_little_endian(static_cast<std::uint32_t>(old + sum),
				std::span{result}.subspan(group * 4U, 4U));
		}
		cpu.vectors[instruction & 31U].bytes = result;
		cpu.pc += 4;
		return continue_after(address, instruction);
	}
	const auto element_raw = decode_little_endian(
		std::span{cpu.vectors[element_register].bytes}.subspan(
			element_index * narrow_size, narrow_size));
	const auto source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto old_destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	if (floating) {
		if (scalar && (cpu.fpcr & 4U) != 0)
			result = operation == 1U || operation == 5U ?
				old_destination : source;
		const std::size_t vector_size = scalar ? narrow_size : q ? 16 : 8;
		const std::size_t lanes = vector_size / narrow_size;
		auto execute = [&]<typename Floating>() {
			using Integer = std::conditional_t<sizeof(Floating) == 8,
				std::uint64_t, std::conditional_t<sizeof(Floating) == 4,
				std::uint32_t, std::uint16_t>>;
			const auto element = std::bit_cast<Floating>(
				static_cast<Integer>(element_raw));
			for (std::size_t lane = 0; lane < lanes; ++lane) {
				const auto left = std::bit_cast<Floating>(
					static_cast<Integer>(decode_little_endian(
						std::span{source}.subspan(
							lane * sizeof(Floating), sizeof(Floating)))));
				const auto accumulator = std::bit_cast<Floating>(
					static_cast<Integer>(decode_little_endian(
						std::span{old_destination}.subspan(
							lane * sizeof(Floating), sizeof(Floating)))));
				const Floating value = operation == 1U ?
					fused_multiply_add(left, element, accumulator) :
					operation == 5U ? fused_multiply_add(
						-left, element, accumulator) : unsigned_group ?
					multiply_extended(left, element) : left * element;
				encode_little_endian(std::bit_cast<Integer>(value),
					std::span{result}.subspan(
						lane * sizeof(Floating), sizeof(Floating)));
			}
		};
		if (size == 0U)
			execute.template operator()<_Float16>();
		else if (size == 2U)
			execute.template operator()<float>();
		else
			execute.template operator()<double>();
	} else {
		const bool widening = operation == 2 || operation == 3 ||
			operation == 6 || operation == 7 || operation == 10 || operation == 11;
		const bool doubling = operation == 3 || operation == 7 ||
			operation == 11 || operation == 12 || operation == 13 ||
			operation == 15;
		const bool rounding_accumulate = unsigned_group &&
			(operation == 13U || operation == 15U);
		if (widening) {
			const std::size_t wide_size = narrow_size * 2;
			const unsigned narrow_bits = static_cast<unsigned>(narrow_size * 8);
			const unsigned wide_bits = narrow_bits * 2;
			const std::size_t source_base = !scalar && q ? 8 : 0;
			const std::size_t lanes = scalar ? 1 : 16 / wide_size;
			for (std::size_t lane = 0; lane < lanes; ++lane) {
				const auto raw = decode_little_endian(std::span{source}.subspan(
					source_base + lane * narrow_size, narrow_size));
				const SignedWide first = unsigned_group && !doubling ? raw :
					as_signed(raw, narrow_bits);
				const SignedWide second = unsigned_group && !doubling ? element_raw :
					as_signed(element_raw, narrow_bits);
				SignedWide product = first * second * (doubling ? 2 : 1);
				std::uint64_t value;
				if (doubling) {
					value = saturate_signed(cpu, product, wide_bits);
					if (operation == 3U || operation == 7U) {
						const auto accumulator = decode_little_endian(
							std::span{old_destination}.subspan(
								lane * wide_size, wide_size));
						const SignedWide combined = as_signed(accumulator,
							wide_bits) + (operation == 3U ?
							as_signed(value, wide_bits) :
							-as_signed(value, wide_bits));
						value = saturate_signed(cpu, combined, wide_bits);
					}
				} else {
					const auto accumulator = decode_little_endian(
						std::span{old_destination}.subspan(lane * wide_size,
							wide_size));
					value = operation == 2 ? accumulator +
						static_cast<std::uint64_t>(product) : operation == 6 ?
						accumulator - static_cast<std::uint64_t>(product) :
						static_cast<std::uint64_t>(product);
				}
				encode_little_endian(value & mask_for(wide_bits),
					std::span{result}.subspan(lane * wide_size, wide_size));
			}
		} else {
			const unsigned bits = static_cast<unsigned>(narrow_size * 8);
			const std::size_t vector_size = scalar ? narrow_size : q ? 16 : 8;
			for (std::size_t offset = 0; offset < vector_size; offset += narrow_size) {
				const auto first = decode_little_endian(
					std::span{source}.subspan(offset, narrow_size));
				const auto old = decode_little_endian(
					std::span{old_destination}.subspan(offset, narrow_size));
				const SignedWide doubled_product = as_signed(first, bits) *
					as_signed(element_raw, bits) * 2;
				std::uint64_t value;
				if (operation == 0)
					value = old + first * element_raw;
				else if (operation == 4)
					value = old - first * element_raw;
				else if (operation == 8)
					value = first * element_raw;
				else if (rounding_accumulate) {
					const SignedWide accumulator = as_signed(old, bits);
					SignedWide combined = accumulator *
						(SignedWide{1} << bits);
					combined += operation == 13U ?
						doubled_product : -doubled_product;
					combined += SignedWide{1} << (bits - 1U);
					value = saturate_signed(cpu, combined >> bits, bits);
				} else {
					const auto product = doubled_product +
						(operation == 13 ? SignedWide{1} << (bits - 1) : 0);
					value = saturate_signed(cpu, product >> bits, bits);
				}
				encode_little_endian(value & mask_for(bits),
					std::span{result}.subspan(offset, narrow_size));
			}
		}
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::backend::arm64
