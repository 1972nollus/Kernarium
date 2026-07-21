// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/instruction_helpers.hpp"

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
using detail::decode_little_endian;
using detail::encode_little_endian;

RunResult Dispatcher::simd_shift_right_narrow(CpuState &cpu, GuestMemory &,
					      std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 11) & 0x1fU;
	if (opcode != 16 && opcode != 17)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const unsigned encoded_shift = (instruction >> 16) & 0x7fU;
	const unsigned source_bits = encoded_shift >= 32 ? 64 :
		encoded_shift >= 16 ? 32 : encoded_shift >= 8 ? 16 : 0;
	if (source_bits == 0)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const unsigned shift = source_bits - encoded_shift;
	if (shift == 0 || shift > source_bits / 2)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const std::size_t source_size = source_bits / 8;
	const std::size_t destination_size = source_size / 2;
	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	const std::size_t output_offset =
		(instruction & 0x4000'0000U) != 0 ? 8 : 0;
	if (output_offset != 0)
		std::copy_n(destination.begin(), output_offset, result.begin());
	for (std::size_t input_offset = 0, output = output_offset;
	     input_offset < source.size();
	     input_offset += source_size, output += destination_size) {
		auto value = decode_little_endian(
			std::span{source}.subspan(input_offset, source_size));
		if (opcode == 17)
			value += std::uint64_t{1} << (shift - 1);
		value >>= shift;
		encode_little_endian(value,
			std::span{result}.subspan(output, destination_size));
	}
	destination = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_shift_immediate(CpuState &cpu, GuestMemory &memory,
					   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 11) & 0x1fU;
	const bool alternate = (instruction & 0x2000'0000U) != 0;
	const unsigned encoded = (instruction >> 16) & 0x7fU;
	if (encoded < 8)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	if (opcode == 20)
		return simd_shift_left_long(cpu, memory, instruction);
	if (opcode >= 16 && opcode <= 19) {
		const unsigned source_bits = encoded >= 32 ? 64 :
			encoded >= 16 ? 32 : 16;
		const unsigned shift = source_bits - encoded;
		if (shift == 0 || shift > source_bits / 2)
			return {StopReason::unsupported_instruction, address, instruction, 0,
				false};
		const std::size_t source_size = source_bits / 8;
		const std::size_t destination_size = source_size / 2;
		const unsigned destination_bits = source_bits / 2;
		const bool scalar = (instruction & 0x1000'0000U) != 0;
		const auto source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
		const auto old_destination = cpu.vectors[instruction & 0x1fU].bytes;
		std::array<std::byte, 16> result{};
		const std::size_t output_base = !scalar &&
			(instruction & 0x4000'0000U) != 0 ? 8 : 0;
		if (output_base != 0)
			std::copy_n(old_destination.begin(), 8, result.begin());
		const std::size_t source_bytes = scalar ? source_size : 16;
		for (std::size_t input = 0, output = output_base; input < source_bytes;
		     input += source_size, output += destination_size) {
			const auto raw = decode_little_endian(
				std::span{source}.subspan(input, source_size));
			const bool rounding = opcode == 17 || opcode == 19;
			const bool signed_input = opcode <= 17 || !alternate;
			__int128 shifted;
			if (signed_input) {
				shifted = detail::sign_extend(raw, source_bits);
				if (rounding)
					shifted += static_cast<__int128>(1) << (shift - 1);
				shifted >>= shift;
			} else {
				unsigned __int128 value = raw;
				if (rounding)
					value += static_cast<unsigned __int128>(1) << (shift - 1);
				shifted = static_cast<__int128>(value >> shift);
			}
			const bool unsigned_result = opcode <= 17 || alternate;
			const __int128 minimum = unsigned_result ? 0 :
				-static_cast<__int128>(static_cast<unsigned __int128>(1) <<
					(destination_bits - 1));
			const __int128 maximum = unsigned_result ?
				static_cast<__int128>((static_cast<unsigned __int128>(1) <<
					destination_bits) - 1) :
				static_cast<__int128>((static_cast<unsigned __int128>(1) <<
					(destination_bits - 1)) - 1);
			if (shifted < minimum || shifted > maximum)
				cpu.fpsr |= 1U << 27;
			shifted = std::clamp(shifted, minimum, maximum);
			encode_little_endian(static_cast<std::uint64_t>(shifted),
				std::span{result}.subspan(output, destination_size));
		}
		cpu.vectors[instruction & 0x1fU].bytes = result;
		cpu.pc += 4;
		return continue_after(address, instruction);
	}
	const unsigned lane_bits = std::bit_floor(encoded);
	if (lane_bits > 64 || (opcode == 8 && !alternate) ||
	    (opcode == 12 && !alternate))
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const std::size_t lane_size = lane_bits / 8;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	if (scalar && lane_bits != 64 && opcode != 12 && opcode != 14 &&
	    opcode != 28 && opcode != 31)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	if (scalar && (opcode == 28 || opcode == 31) && lane_bits < 32)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const std::size_t vector_size = scalar ? lane_size :
		(instruction & 0x4000'0000U) != 0 ? 16 : 8;
	if (lane_size > vector_size ||
	    (!scalar && lane_size == 8 && vector_size != 16))
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	if (opcode == 28 || opcode == 31) {
		const unsigned fractional_bits = 2 * lane_bits - encoded;
		const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
		std::array<std::byte, 16> result{};
		for (std::size_t offset = 0; offset < vector_size; offset += lane_size) {
			const auto raw = decode_little_endian(
				std::span{source}.subspan(offset, lane_size));
			if (opcode == 28) {
				long double value = alternate ? static_cast<long double>(raw) :
					static_cast<long double>(detail::sign_extend(raw, lane_bits));
				value = std::ldexp(value, -static_cast<int>(fractional_bits));
				const auto converted = lane_bits == 64 ? std::bit_cast<std::uint64_t>(
					static_cast<double>(value)) : std::bit_cast<std::uint32_t>(
					static_cast<float>(value));
				encode_little_endian(converted,
					std::span{result}.subspan(offset, lane_size));
			} else {
				long double value = lane_bits == 64 ? static_cast<long double>(
					std::bit_cast<double>(raw)) : static_cast<long double>(
					std::bit_cast<float>(static_cast<std::uint32_t>(raw)));
				value = std::trunc(std::ldexp(value,
					static_cast<int>(fractional_bits)));
				std::uint64_t converted;
				if (std::isnan(value)) {
					converted = 0;
				} else if (alternate) {
					const long double maximum = lane_bits == 64 ?
						static_cast<long double>(std::numeric_limits<std::uint64_t>::max()) :
						static_cast<long double>(std::numeric_limits<std::uint32_t>::max());
					converted = value <= 0 ? 0 : value >= maximum ?
						(lane_bits == 64 ? ~std::uint64_t{0} : 0xffff'ffffULL) :
						static_cast<std::uint64_t>(value);
				} else {
					const long double minimum = lane_bits == 64 ?
						static_cast<long double>(std::numeric_limits<std::int64_t>::min()) :
						static_cast<long double>(std::numeric_limits<std::int32_t>::min());
					const long double maximum = lane_bits == 64 ?
						static_cast<long double>(std::numeric_limits<std::int64_t>::max()) :
						static_cast<long double>(std::numeric_limits<std::int32_t>::max());
					converted = value <= minimum ? std::uint64_t{1} << (lane_bits - 1) :
						value >= maximum ? (std::uint64_t{1} << (lane_bits - 1)) - 1 :
						static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
				}
				encode_little_endian(converted,
					std::span{result}.subspan(offset, lane_size));
			}
		}
		cpu.vectors[instruction & 0x1fU].bytes = result;
		cpu.pc += 4;
		return continue_after(address, instruction);
	}
	const bool shift_left = opcode == 10 || opcode == 12 || opcode == 14;
	if (opcode != 0 && opcode != 2 && opcode != 4 && opcode != 6 &&
	    opcode != 8 && opcode != 10 && opcode != 12 && opcode != 14)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const unsigned shift = shift_left ? encoded - lane_bits :
		2 * lane_bits - encoded;
	if ((shift_left && shift >= lane_bits) ||
	    (!shift_left && (shift == 0 || shift > lane_bits)))
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto mask = lane_bits == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << lane_bits) - 1;
	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto &old_destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t offset = 0; offset < vector_size; offset += lane_size) {
		const auto source_value = decode_little_endian(
			std::span{source}.subspan(offset, lane_size));
		const auto old_value = decode_little_endian(
			std::span{old_destination}.subspan(offset, lane_size));
		std::uint64_t value;
		if (opcode == 0 || opcode == 2 || opcode == 4 || opcode == 6) {
			if (alternate) {
				if (opcode == 4 || opcode == 6) {
					const unsigned __int128 rounded =
						static_cast<unsigned __int128>(source_value) +
						(static_cast<unsigned __int128>(1) <<
						 (shift - 1));
					value = static_cast<std::uint64_t>(rounded >> shift);
				} else {
					value = shift == lane_bits ? 0 : source_value >> shift;
				}
			} else {
				__int128 signed_value = detail::sign_extend(
					source_value, lane_bits);
				if (opcode == 4 || opcode == 6)
					signed_value += static_cast<__int128>(1) <<
						(shift - 1);
				value = shift == lane_bits ?
					(signed_value < 0 ? mask : 0) :
					static_cast<std::uint64_t>(signed_value >> shift);
			}
			if (opcode == 2 || opcode == 6)
				value += old_value;
		} else if (opcode == 8) {
			const auto low_mask = shift == lane_bits ? 0 :
				mask >> shift;
			value = (old_value & ~low_mask) |
				(shift == lane_bits ? 0 : source_value >> shift);
		} else if (opcode == 12 || opcode == 14) {
			const __int128 signed_value = detail::sign_extend(source_value,
				lane_bits);
			const __int128 shifted = signed_value << shift;
			const bool unsigned_result = alternate;
			const __int128 minimum = unsigned_result ? 0 :
				-static_cast<__int128>(static_cast<unsigned __int128>(1) <<
					(lane_bits - 1));
			const __int128 maximum = unsigned_result ?
				static_cast<__int128>((static_cast<unsigned __int128>(1) <<
					lane_bits) - 1) :
				static_cast<__int128>((static_cast<unsigned __int128>(1) <<
					(lane_bits - 1)) - 1);
			if (opcode == 14 && alternate) {
				const unsigned __int128 unsigned_shifted =
					static_cast<unsigned __int128>(source_value) << shift;
				if (unsigned_shifted > static_cast<unsigned __int128>(maximum))
					cpu.fpsr |= 1U << 27;
				value = static_cast<std::uint64_t>(std::min(unsigned_shifted,
					static_cast<unsigned __int128>(maximum)));
			} else {
				if (shifted < minimum || shifted > maximum)
					cpu.fpsr |= 1U << 27;
				value = static_cast<std::uint64_t>(
					std::clamp(shifted, minimum, maximum));
			}
		} else {
			const auto shifted = (source_value << shift) & mask;
			if (alternate) {
				const auto low_mask = shift == 0 ? 0 :
					(std::uint64_t{1} << shift) - 1;
				value = shifted | (old_value & low_mask);
			} else {
				value = shifted;
			}
		}
		encode_little_endian(value & mask,
			std::span{result}.subspan(offset, lane_size));
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_shift_register(CpuState &cpu, GuestMemory &,
					  std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool unsigned_shift = (instruction & 0x2000'0000U) != 0;
	const bool rounding = ((instruction >> 11) & 0x1fU) == 10;
	const std::size_t lane_size = std::size_t{1} <<
		((instruction >> 22) & 0x3U);
	const unsigned lane_bits = static_cast<unsigned>(lane_size * 8);
	const std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ?
		16 : 8;
	if (lane_size > vector_size || (lane_size == 8 && vector_size != 16))
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto mask = lane_bits == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << lane_bits) - 1;
	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto &shifts = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t offset = 0; offset < vector_size; offset += lane_size) {
		const auto source_value = decode_little_endian(
			std::span{source}.subspan(offset, lane_size));
		const auto shift_value = decode_little_endian(
			std::span{shifts}.subspan(offset, lane_size));
		const int shift = static_cast<std::int8_t>(shift_value & 0xffU);
		std::uint64_t value;
		if (shift >= 0) {
			value = static_cast<unsigned>(shift) >= lane_bits ? 0 :
				(source_value << shift) & mask;
		} else {
			const unsigned right = static_cast<unsigned>(-shift);
			if (right > lane_bits) {
				value = !unsigned_shift && !rounding &&
					(source_value & (std::uint64_t{1} <<
					 (lane_bits - 1))) != 0 ? mask : 0;
			} else if (unsigned_shift) {
				if (rounding) {
					const unsigned __int128 rounded =
						static_cast<unsigned __int128>(source_value) +
						(static_cast<unsigned __int128>(1) <<
						 (right - 1));
					value = static_cast<std::uint64_t>(rounded >> right);
				} else {
					value = right == lane_bits ? 0 : source_value >> right;
				}
			} else {
				__int128 signed_value = detail::sign_extend(
					source_value, lane_bits);
				if (rounding)
					signed_value += static_cast<__int128>(1) <<
						(right - 1);
				value = right == lane_bits ?
					(signed_value < 0 ? mask : 0) :
					static_cast<std::uint64_t>(signed_value >> right);
			}
		}
		encode_little_endian(value & mask,
			std::span{result}.subspan(offset, lane_size));
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_shift_left_long(CpuState &cpu, GuestMemory &,
					   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned encoded = (instruction >> 16) & 0x7fU;
	const unsigned source_bits = std::bit_floor(encoded);
	if (source_bits < 8 || source_bits > 32)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const unsigned shift = encoded - source_bits;
	if (shift >= source_bits)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const bool unsigned_source = (instruction & 0x2000'0000U) != 0;
	const std::size_t source_size = source_bits / 8;
	const std::size_t destination_size = source_size * 2;
	const std::size_t source_base =
		(instruction & 0x4000'0000U) != 0 ? 8 : 0;
	const auto source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t input = source_base, output = 0; output < 16;
	     input += source_size, output += destination_size) {
		const auto raw = decode_little_endian(
			std::span{source}.subspan(input, source_size));
		const auto value = unsigned_source ? raw << shift :
			static_cast<std::uint64_t>(detail::sign_extend(raw, source_bits)) <<
				shift;
		encode_little_endian(value,
			std::span{result}.subspan(output, destination_size));
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::backend::arm64
