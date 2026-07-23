// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/instruction_helpers.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::backend::arm64 {

using detail::continue_after;
using detail::decode_little_endian;
using detail::encode_little_endian;

RunResult Dispatcher::simd_arithmetic_narrow(CpuState &cpu, GuestMemory &,
					     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool subtract = ((instruction >> 11) & 0x1fU) == 12;
	const bool rounding = (instruction & 0x2000'0000U) != 0;
	const std::size_t source_size = std::size_t{2} <<
		((instruction >> 22) & 0x3U);
	if (source_size > 8)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const std::size_t destination_size = source_size / 2;
	const unsigned destination_bits =
		static_cast<unsigned>(destination_size * 8);
	const auto source_mask = source_size == 8 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << (source_size * 8)) - 1;
	const auto &left = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto &right = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	const std::size_t output_offset =
		(instruction & 0x4000'0000U) != 0 ? 8 : 0;
	if (output_offset != 0)
		std::copy_n(destination.begin(), output_offset, result.begin());
	for (std::size_t input = 0, output = output_offset; input < 16;
	     input += source_size, output += destination_size) {
		const auto first = decode_little_endian(
			std::span{left}.subspan(input, source_size));
		const auto second = decode_little_endian(
			std::span{right}.subspan(input, source_size));
		auto value = (subtract ? first - second : first + second) & source_mask;
		if (rounding)
			value = (value + (std::uint64_t{1} <<
				(destination_bits - 1))) & source_mask;
		encode_little_endian(value >> destination_bits,
			std::span{result}.subspan(output, destination_size));
	}
	destination = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_arithmetic_wide(CpuState &cpu, GuestMemory &,
					   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned operation = (instruction >> 12) & 0xfU;
	const unsigned size = (instruction >> 22) & 0x3U;
	const bool multiply = operation == 8 || operation == 10 || operation == 12;
	const bool absolute_difference = operation == 5 || operation == 7;
	const bool saturating_multiply = operation == 9 || operation == 11 ||
		operation == 13;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	if ((!multiply && !absolute_difference && !saturating_multiply &&
	     operation > 3) || size == 3 || (scalar && !saturating_multiply))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const bool unsigned_values = (instruction & 0x2000'0000U) != 0;
	if (saturating_multiply && (unsigned_values || size == 0))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const bool subtract = operation >= 2;
	const bool wide_left = (operation & 1U) != 0;
	const std::size_t narrow_size = std::size_t{1} << size;
	const std::size_t wide_size = narrow_size * 2;
	const unsigned narrow_bits = static_cast<unsigned>(narrow_size * 8);
	const unsigned wide_bits = narrow_bits * 2;
	const auto wide_mask = wide_bits == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << wide_bits) - 1;
	const std::size_t narrow_base = !scalar &&
		(instruction & 0x4000'0000U) != 0 ? 8 : 0;
	const auto left = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	const auto old_destination =
		cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	const std::size_t output_size = scalar ? wide_size : 16;
	for (std::size_t output = 0; output < output_size; output += wide_size) {
		const std::size_t lane = output / wide_size;
		const auto decode_raw_narrow = [&](const auto &bytes) {
			return decode_little_endian(std::span{bytes}.subspan(
				narrow_base + lane * narrow_size, narrow_size));
		};
		const auto decode_narrow = [&](const auto &bytes) {
			const auto raw = decode_raw_narrow(bytes);
			return unsigned_values ? raw : static_cast<std::uint64_t>(
				detail::sign_extend(raw, narrow_bits));
		};
		std::uint64_t value;
		if (saturating_multiply) {
			const __int128 product = static_cast<__int128>(
				detail::sign_extend(decode_raw_narrow(left), narrow_bits)) *
				static_cast<__int128>(detail::sign_extend(
					decode_raw_narrow(right), narrow_bits)) * 2;
			__int128 combined = product;
			if (operation != 13) {
				const auto accumulator = decode_little_endian(
					std::span{old_destination}.subspan(output, wide_size));
				combined = static_cast<__int128>(
					detail::sign_extend(accumulator, wide_bits)) +
					(operation == 11 ? -product : product);
			}
			const __int128 minimum = -static_cast<__int128>(
				static_cast<unsigned __int128>(1) << (wide_bits - 1));
			const __int128 maximum = static_cast<__int128>(
				(static_cast<unsigned __int128>(1) << (wide_bits - 1)) - 1);
			if (combined < minimum || combined > maximum)
				cpu.fpsr |= 1U << 27;
			combined = std::clamp(combined, minimum, maximum);
			value = static_cast<std::uint64_t>(combined);
		} else if (absolute_difference) {
			const auto first = decode_raw_narrow(left);
			const auto second = decode_raw_narrow(right);
			std::uint64_t difference;
			if (unsigned_values)
				difference = first >= second ? first - second : second - first;
			else {
				const auto signed_first = static_cast<__int128>(
					detail::sign_extend(first, narrow_bits));
				const auto signed_second = static_cast<__int128>(
					detail::sign_extend(second, narrow_bits));
				const auto delta = signed_first - signed_second;
				difference = static_cast<std::uint64_t>(
					delta < 0 ? -delta : delta);
			}
			value = difference;
			if (operation == 5)
				value += decode_little_endian(
					std::span{old_destination}.subspan(output, wide_size));
		} else if (multiply) {
			const auto product =
				(decode_narrow(left) * decode_narrow(right)) & wide_mask;
			if (operation == 12) {
				value = product;
			} else {
				const auto accumulator = decode_little_endian(
					std::span{old_destination}.subspan(
						output, wide_size));
				value = operation == 10 ? accumulator - product :
					accumulator + product;
			}
		} else {
			const auto first = wide_left ? decode_little_endian(
				std::span{left}.subspan(output, wide_size)) :
				decode_narrow(left);
			const auto second = decode_narrow(right);
			value = subtract ? first - second : first + second;
		}
		value &= wide_mask;
		encode_little_endian(value,
			std::span{result}.subspan(output, wide_size));
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_narrow_move(CpuState &cpu, GuestMemory &,
				       std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned size = (instruction >> 22) & 0x3U;
	if (size == 3)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const std::size_t source_size = std::size_t{2} << size;
	const std::size_t destination_size = source_size / 2;
	const auto source =
		cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	const std::size_t output_offset =
		(instruction & 0x4000'0000U) != 0 ? 8 : 0;
	if (output_offset != 0)
		std::copy_n(destination.begin(), output_offset, result.begin());
	for (std::size_t input = 0, output = output_offset; input < 16;
	     input += source_size, output += destination_size) {
		const auto value = decode_little_endian(
			std::span{source}.subspan(input, source_size));
		encode_little_endian(value,
			std::span{result}.subspan(output, destination_size));
	}
	destination = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::backend::arm64
