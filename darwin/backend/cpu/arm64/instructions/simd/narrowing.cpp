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
		return {StopReason::unsupported_instruction, address, instruction, 0,
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
	if ((!multiply && operation > 3) || size == 3)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const bool unsigned_values = (instruction & 0x2000'0000U) != 0;
	const bool subtract = operation >= 2;
	const bool wide_left = (operation & 1U) != 0;
	const std::size_t narrow_size = std::size_t{1} << size;
	const std::size_t wide_size = narrow_size * 2;
	const unsigned narrow_bits = static_cast<unsigned>(narrow_size * 8);
	const unsigned wide_bits = narrow_bits * 2;
	const auto wide_mask = wide_bits == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << wide_bits) - 1;
	const std::size_t narrow_base =
		(instruction & 0x4000'0000U) != 0 ? 8 : 0;
	const auto left = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	const auto old_destination =
		cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t output = 0; output < 16; output += wide_size) {
		const std::size_t lane = output / wide_size;
		const auto decode_narrow = [&](const auto &bytes) {
			const auto raw = decode_little_endian(std::span{bytes}.subspan(
				narrow_base + lane * narrow_size, narrow_size));
			return unsigned_values ? raw : static_cast<std::uint64_t>(
				detail::sign_extend(raw, narrow_bits));
		};
		std::uint64_t value;
		if (multiply) {
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
		return {StopReason::unsupported_instruction, address, instruction, 0,
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
