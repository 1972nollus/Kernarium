// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"
#include "complete_helpers.hpp"

#include "../../detail/floating_compare.hpp"
#include "../../detail/floating_indexed.hpp"
#include "../../detail/instruction_helpers.hpp"
#include "../../detail/integer_multiply.hpp"

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

namespace uml::cpu::arm64 {

using detail::continue_after;
using detail::decode_little_endian;
using detail::encode_little_endian;
using namespace simd_complete_detail;

RunResult Dispatcher::simd_dot_product(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned size = (instruction >> 22) & 3U;
	if ((instruction & 0x1000'0000U) != 0U || size != 2U)
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
	const bool q = (instruction & 0x4000'0000U) != 0;
	const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
	const unsigned size = (instruction >> 22) & 3U;
	const bool alternate = (instruction & 0x800U) != 0;
	if (!q || size != 2U || (unsigned_group && alternate))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const bool mixed = alternate;
	const bool both_unsigned = unsigned_group;
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
	if (!detail::execute_saturating_rounding_multiply_accumulate(
		    cpu, instruction))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
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
			if (opcode == 18 && !unsigned_group)
				value = raw & mask_for(lane_bits); // XTN
			else if (opcode == 18)
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

} // namespace uml::cpu::arm64
