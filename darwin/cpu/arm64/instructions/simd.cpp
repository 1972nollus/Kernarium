// SPDX-License-Identifier: GPL-2.0-or-later
#include "../dispatcher.hpp"

#include "../detail/instruction_helpers.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64 {

using detail::continue_after;
using detail::decode_simd_modified_immediate;
using detail::decode_little_endian;
using detail::encode_little_endian;
using detail::set_vector_half;
using detail::vector_half;

RunResult Dispatcher::simd_modified_immediate(CpuState &cpu, GuestMemory &,
					      std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool q = (instruction & 0x4000'0000U) != 0;
	const unsigned destination = instruction & 0x1fU;
	const auto decoded = decode_simd_modified_immediate(instruction);
	if (!decoded)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	auto &vector = cpu.vectors[destination];
	if (decoded->action != 0U) {
		set_vector_half(vector, 0, decoded->action == 2U ?
			vector_half(vector, 0) & ~decoded->pattern :
			vector_half(vector, 0) | decoded->pattern);
		set_vector_half(vector, 1, q ? (decoded->action == 2U ?
			vector_half(vector, 1) & ~decoded->pattern :
			vector_half(vector, 1) | decoded->pattern) : 0);
	} else {
		set_vector_half(vector, 0, decoded->pattern);
		set_vector_half(vector, 1, q ? decoded->pattern : 0);
	}
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_compare_zero(CpuState &cpu, GuestMemory &,
					std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 12) & 0x1fU;
	const bool alternate = (instruction & 0x2000'0000U) != 0;
	if (opcode != 8 && opcode != 9 && (opcode != 10 || alternate))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned size = (instruction >> 22) & 0x3U;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const bool q = (instruction & 0x4000'0000U) != 0;
	if ((scalar && size != 3U) || (!scalar && !q && size == 3U))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const std::size_t lane_size = std::size_t{1} << size;
	const std::size_t vector_size = scalar ? lane_size :
		q ? 16 : 8;
	if (lane_size > vector_size)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t offset = 0; offset < vector_size; offset += lane_size) {
		const auto lane = std::span{source}.subspan(offset, lane_size);
		const bool zero = std::ranges::all_of(lane,
			[](std::byte value) { return value == std::byte{}; });
		const bool negative = (std::to_integer<unsigned>(
			lane[lane_size - 1]) & 0x80U) != 0;
		const bool matches = opcode == 8 ?
			(alternate ? !negative : !negative && !zero) :
			opcode == 9 ? (alternate ? negative || zero : zero) : negative;
		std::fill_n(result.begin() + offset, lane_size,
			matches ? std::byte{0xff} : std::byte{});
	}
	destination = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_compare_register(CpuState &cpu, GuestMemory &,
					    std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 11) & 0x1fU;
	const bool unsigned_compare = (instruction & 0x2000'0000U) != 0;
	if (opcode != 6 && opcode != 7 && opcode != 17)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const std::size_t lane_size = std::size_t{1} <<
		((instruction >> 22) & 0x3U);
	const std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ?
		16 : 8;
	if (lane_size > vector_size)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto &left = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto &right = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t offset = 0; offset < vector_size; offset += lane_size) {
		const auto left_value = decode_little_endian(
			std::span{left}.subspan(offset, lane_size));
		const auto right_value = decode_little_endian(
			std::span{right}.subspan(offset, lane_size));
		bool matches;
		if (opcode == 17) {
			matches = unsigned_compare ? left_value == right_value :
				(left_value & right_value) != 0;
		} else if (unsigned_compare) {
			matches = opcode == 6 ? left_value > right_value :
				left_value >= right_value;
		} else {
			const auto bits = static_cast<unsigned>(lane_size * 8);
			const auto signed_left = detail::sign_extend(left_value, bits);
			const auto signed_right = detail::sign_extend(right_value, bits);
			matches = opcode == 6 ? signed_left > signed_right :
				signed_left >= signed_right;
		}
		std::fill_n(result.begin() + offset, lane_size,
			matches ? std::byte{0xff} : std::byte{});
	}
	destination = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_bitwise_register(CpuState &cpu, GuestMemory &,
					    std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned operation = ((instruction >> 22) & 0x3U) |
		((instruction >> 27) & 0x4U);
	const std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ?
		16 : 8;
	const auto &left = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto &right = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t index = 0; index < vector_size; ++index) {
		const auto old = std::to_integer<unsigned>(destination[index]);
		const auto lhs = std::to_integer<unsigned>(left[index]);
		const auto rhs = std::to_integer<unsigned>(right[index]);
		unsigned value;
		switch (operation) {
		case 0: value = lhs & rhs; break;
		case 1: value = lhs & ~rhs; break;
		case 2: value = lhs | rhs; break;
		case 3: value = lhs | ~rhs; break;
		case 4: value = lhs ^ rhs; break;
		case 5: value = (old & lhs) | (~old & rhs); break;
		case 6: value = (old & ~rhs) | (lhs & rhs); break;
		case 7: value = (old & rhs) | (lhs & ~rhs); break;
		default: __builtin_unreachable();
		}
		result[index] = static_cast<std::byte>(value & 0xffU);
	}
	destination = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_min_max(CpuState &cpu, GuestMemory &,
				   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 11) & 0x1fU;
	const bool pairwise = (opcode & 0x10U) != 0;
	const bool minimum = (opcode & 1U) != 0;
	const bool unsigned_compare = (instruction & 0x2000'0000U) != 0;
	const std::size_t lane_size = std::size_t{1} <<
		((instruction >> 22) & 0x3U);
	const std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ?
		16 : 8;
	if (lane_size > 4 || lane_size > vector_size)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto &left = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto &right = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	const auto select = [&](std::uint64_t first, std::uint64_t second) {
		bool first_before_second;
		if (unsigned_compare) {
			first_before_second = first < second;
		} else {
			const auto bits = static_cast<unsigned>(lane_size * 8);
			first_before_second = detail::sign_extend(first, bits) <
				detail::sign_extend(second, bits);
		}
		return first_before_second == minimum ? first : second;
	};
	if (!pairwise) {
		for (std::size_t offset = 0; offset < vector_size;
		     offset += lane_size) {
			const auto first = decode_little_endian(
				std::span{left}.subspan(offset, lane_size));
			const auto second = decode_little_endian(
				std::span{right}.subspan(offset, lane_size));
			encode_little_endian(select(first, second),
				std::span{result}.subspan(offset, lane_size));
		}
	} else {
		const std::size_t output_lanes = vector_size / lane_size;
		const std::size_t source_lanes = output_lanes / 2;
		for (std::size_t output = 0; output < output_lanes; ++output) {
			const auto &source = output < source_lanes ? left : right;
			const std::size_t pair = output % source_lanes;
			const std::size_t source_offset = pair * lane_size * 2;
			const auto first = decode_little_endian(
				std::span{source}.subspan(source_offset, lane_size));
			const auto second = decode_little_endian(
				std::span{source}.subspan(
					source_offset + lane_size, lane_size));
			encode_little_endian(select(first, second),
				std::span{result}.subspan(output * lane_size, lane_size));
		}
	}
	destination = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_bit_unary(CpuState &cpu, GuestMemory &,
				     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 10) & 0x3fU;
	const bool alternate = (instruction & 0x2000'0000U) != 0;
	const unsigned size = (instruction >> 22) & 0x3U;
	const bool valid = opcode == 22 ?
		((!alternate && size == 0) || (alternate && size <= 1)) :
		opcode == 2 ? ((!alternate && size < 3) ||
			(alternate && size < 2)) :
		opcode == 6 && !alternate && size == 0;
	if (!valid)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ?
		16 : 8;
	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	if (opcode == 22) {
		for (std::size_t index = 0; index < vector_size; ++index) {
			const auto value = std::to_integer<std::uint8_t>(source[index]);
			if (alternate && size == 0) {
				result[index] = static_cast<std::byte>(~value);
			} else if (alternate) {
				auto reversed = static_cast<std::uint8_t>(
					(value >> 4) | (value << 4));
				reversed = static_cast<std::uint8_t>(
					((reversed & 0xccU) >> 2) |
					((reversed & 0x33U) << 2));
				reversed = static_cast<std::uint8_t>(
					((reversed & 0xaaU) >> 1) |
					((reversed & 0x55U) << 1));
				result[index] = static_cast<std::byte>(reversed);
			} else {
				result[index] = static_cast<std::byte>(std::popcount(value));
			}
		}
	} else {
		std::size_t block_size;
		if (opcode == 2 && !alternate && size < 3)
			block_size = 8;
		else if (opcode == 2 && alternate && size < 2)
			block_size = 4;
		else if (opcode == 6 && !alternate && size == 0)
			block_size = 2;
		else
			return {StopReason::illegal_instruction, address,
				instruction, 0, false};
		const std::size_t lane_size = std::size_t{1} << size;
		for (std::size_t block = 0; block < vector_size;
		     block += block_size)
			for (std::size_t lane = 0; lane < block_size;
			     lane += lane_size)
				std::copy_n(source.begin() + block + lane,
					lane_size,
					result.begin() + block + block_size -
					lane - lane_size);
	}
	destination = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_reduce_integer(CpuState &cpu, GuestMemory &,
					  std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 10) & 0x3fU;
	const bool addition = opcode == 46;
	const bool widening_addition = opcode == 14;
	const bool minimum = (instruction & 0x0001'0000U) != 0;
	const bool unsigned_compare = (instruction & 0x2000'0000U) != 0;
	const std::size_t source_lane_size = std::size_t{1} <<
		((instruction >> 22) & 0x3U);
	const std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ?
		16 : 8;
	if (source_lane_size > 4 ||
	    (source_lane_size == 4 && vector_size != 16) ||
	    (!addition && !widening_addition && opcode != 42) ||
	    (addition && unsigned_compare))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	std::uint64_t reduced = decode_little_endian(
		std::span{source}.first(source_lane_size));
	if (widening_addition && !unsigned_compare)
		reduced = static_cast<std::uint64_t>(detail::sign_extend(
			reduced, static_cast<unsigned>(source_lane_size * 8)));
	for (std::size_t offset = source_lane_size; offset < vector_size;
	     offset += source_lane_size) {
		const auto value = decode_little_endian(
			std::span{source}.subspan(offset, source_lane_size));
		if (addition || widening_addition) {
			reduced += widening_addition && !unsigned_compare ?
				static_cast<std::uint64_t>(detail::sign_extend(value,
					static_cast<unsigned>(source_lane_size * 8))) :
				value;
		} else {
			bool value_before_reduced;
			if (unsigned_compare) {
				value_before_reduced = value < reduced;
			} else {
				const auto bits = static_cast<unsigned>(source_lane_size * 8);
				value_before_reduced = detail::sign_extend(value, bits) <
					detail::sign_extend(reduced, bits);
			}
			if (value_before_reduced == minimum)
				reduced = value;
		}
	}
	std::array<std::byte, 16> result{};
	const std::size_t destination_lane_size = widening_addition ?
		source_lane_size * 2 : source_lane_size;
	encode_little_endian(reduced,
		std::span{result}.first(destination_lane_size));
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_arithmetic_register(CpuState &cpu, GuestMemory &,
					       std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 10) & 0x3fU;
	const bool scalar_pairwise =
		(instruction & 0xffff'fc00U) == 0x5ef1'b800U;
	const bool pairwise = opcode == 47 || scalar_pairwise;
	const bool multiply = opcode == 39;
	const bool subtract = (instruction & 0x2000'0000U) != 0;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const std::size_t lane_size = std::size_t{1} <<
		((instruction >> 22) & 0x3U);
	const std::size_t vector_size = scalar ? lane_size :
		(instruction & 0x4000'0000U) != 0 ? 16 : 8;
	if (lane_size > vector_size ||
	    (scalar && (lane_size != 8 ||
		(!scalar_pairwise && pairwise) || multiply)) ||
	    (!scalar && lane_size == 8 && (vector_size != 16 || multiply)))
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto &left = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto &right = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	if (scalar_pairwise) {
		const auto first = decode_little_endian(std::span{left}.first(8));
		const auto second = decode_little_endian(std::span{left}.subspan(8, 8));
		encode_little_endian(first + second, std::span{result}.first(8));
	} else if (pairwise) {
		const std::size_t output_lanes = vector_size / lane_size;
		const std::size_t source_lanes = output_lanes / 2;
		for (std::size_t output = 0; output < output_lanes; ++output) {
			const auto &source = output < source_lanes ? left : right;
			const std::size_t pair = output % source_lanes;
			const std::size_t source_offset = pair * lane_size * 2;
			const auto first = decode_little_endian(
				std::span{source}.subspan(source_offset, lane_size));
			const auto second = decode_little_endian(
				std::span{source}.subspan(
					source_offset + lane_size, lane_size));
			encode_little_endian(first + second,
				std::span{result}.subspan(output * lane_size, lane_size));
		}
	} else {
		for (std::size_t offset = 0; offset < vector_size;
		     offset += lane_size) {
			const auto first = decode_little_endian(
				std::span{left}.subspan(offset, lane_size));
			const auto second = decode_little_endian(
				std::span{right}.subspan(offset, lane_size));
			const auto value = multiply ? first * second :
				subtract ? first - second : first + second;
			encode_little_endian(value,
				std::span{result}.subspan(offset, lane_size));
		}
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::cpu::arm64
