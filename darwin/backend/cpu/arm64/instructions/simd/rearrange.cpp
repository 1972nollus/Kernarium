// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/instruction_helpers.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::backend::arm64 {

using detail::continue_after;
using detail::encode_little_endian;
using detail::set_vector_half;
using detail::vector_half;

RunResult Dispatcher::simd_extract(CpuState &cpu, GuestMemory &,
				   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ?
		16 : 8;
	const std::size_t offset = (instruction >> 11) & 0xfU;
	if (offset >= vector_size)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto &left = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto &right = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t index = 0; index < vector_size; ++index) {
		const std::size_t source = offset + index;
		result[index] = source < vector_size ? left[source] :
			right[source - vector_size];
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_permute(CpuState &cpu, GuestMemory &,
				   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ?
		16 : 8;
	const std::size_t element_size =
		std::size_t{1} << ((instruction >> 22) & 0x3U);
	const std::size_t element_count = vector_size / element_size;
	if (element_count < 2)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};

	const auto left = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	const auto copy_element = [&](std::size_t destination,
					  const auto &source, std::size_t source_index) {
		std::copy_n(source.begin() + source_index * element_size,
			element_size, result.begin() + destination * element_size);
	};
	const unsigned opcode = (instruction >> 12) & 0x7U;
	for (std::size_t index = 0; index < element_count / 2; ++index) {
		switch (opcode) {
		case 1: // UZP1
		case 5: // UZP2
			copy_element(index, left, index * 2 + (opcode == 5));
			copy_element(index + element_count / 2, right,
				index * 2 + (opcode == 5));
			break;
		case 2: // TRN1
		case 6: // TRN2
			copy_element(index * 2, left,
				index * 2 + (opcode == 6));
			copy_element(index * 2 + 1, right,
				index * 2 + (opcode == 6));
			break;
		case 3: // ZIP1
		case 7: // ZIP2
			copy_element(index * 2, left,
				index + (opcode == 7 ? element_count / 2 : 0));
			copy_element(index * 2 + 1, right,
				index + (opcode == 7 ? element_count / 2 : 0));
			break;
		default:
			return {StopReason::unsupported_instruction, address,
				instruction, 0, false};
		}
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_table_lookup(CpuState &cpu, GuestMemory &,
					std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ?
		16 : 8;
	const unsigned first_table = (instruction >> 5) & 0x1fU;
	const unsigned table_count = ((instruction >> 13) & 0x3U) + 1;
	const bool extend = (instruction & 0x1000U) != 0;
	const auto &indices = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	const auto &old_destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t output = 0; output < vector_size; ++output) {
		const auto index = std::to_integer<unsigned>(indices[output]);
		if (index < table_count * 16U) {
			const unsigned table = (first_table + index / 16) & 0x1fU;
			result[output] = cpu.vectors[table].bytes[index & 0xfU];
		} else if (extend) {
			result[output] = old_destination[output];
		}
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_general_move(CpuState &cpu, GuestMemory &,
					std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	if ((instruction & 0xbfe0'fc00U) == 0x0e00'3c00U ||
	    (instruction & 0xbfe0'fc00U) == 0x0e00'2c00U) {
		const bool signed_move = (instruction & 0x1000U) == 0;
		const unsigned immediate = (instruction >> 16) & 0x1fU;
		if (immediate == 0)
			return {StopReason::unsupported_instruction, address, instruction,
				0, false};
		const unsigned element_shift = std::countr_zero(immediate);
		if (element_shift > 3 ||
		    (element_shift == 3 && (instruction & 0x4000'0000U) == 0))
			return {StopReason::unsupported_instruction, address, instruction,
				0, false};
		const std::size_t element_size = std::size_t{1} << element_shift;
		const std::size_t lane = immediate >> (element_shift + 1);
		const std::size_t offset = lane * element_size;
		if (offset + element_size > 16)
			return {StopReason::unsupported_instruction, address, instruction,
				0, false};
		auto value = detail::decode_little_endian(
			std::span{cpu.vectors[(instruction >> 5) & 0x1fU].bytes}
				.subspan(offset, element_size));
		if (signed_move)
			value = static_cast<std::uint64_t>(detail::sign_extend(value,
				static_cast<unsigned>(element_size * 8)));
		if ((instruction & 0x4000'0000U) == 0)
			value &= 0xffff'ffffU;
		cpu.write_register(instruction & 0x1fU, value, false);
		cpu.pc += 4;
		return continue_after(address, instruction);
	}
	const auto opcode = instruction & 0xffff'fc00U;
	const unsigned source = (instruction >> 5) & 0x1fU;
	const unsigned destination = instruction & 0x1fU;
	switch (opcode) {
	case 0x9e66'0000U:
		cpu.write_register(destination, vector_half(cpu.vectors[source], 0),
			false);
		break;
	case 0x1e26'0000U:
		cpu.write_register(destination,
			vector_half(cpu.vectors[source], 0) & 0xffff'ffffU, false);
		break;
	case 0x9eae'0000U:
		cpu.write_register(destination, vector_half(cpu.vectors[source], 1),
			false);
		break;
	case 0x9e67'0000U:
		set_vector_half(cpu.vectors[destination], 0,
			cpu.read_register(source, false));
		set_vector_half(cpu.vectors[destination], 1, 0);
		break;
	case 0x1e27'0000U:
		set_vector_half(cpu.vectors[destination], 0,
			cpu.read_register(source, false) & 0xffff'ffffU);
		set_vector_half(cpu.vectors[destination], 1, 0);
		break;
	case 0x9eaf'0000U:
		set_vector_half(cpu.vectors[destination], 1,
			cpu.read_register(source, false));
		break;
	default:
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	}
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_insert_general(CpuState &cpu, GuestMemory &,
					   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned immediate = (instruction >> 16) & 0x1fU;
	if (immediate == 0)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const unsigned element_shift = std::countr_zero(immediate);
	if (element_shift > 3)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const std::size_t element_size = std::size_t{1} << element_shift;
	const std::size_t lane = immediate >> (element_shift + 1);
	const std::size_t offset = lane * element_size;
	if (offset + element_size > 16)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto source = cpu.read_register((instruction >> 5) & 0x1fU, false);
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	encode_little_endian(source,
		std::span{destination}.subspan(offset, element_size));
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_insert_element(CpuState &cpu, GuestMemory &,
					   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned destination_immediate = (instruction >> 16) & 0x1fU;
	if (destination_immediate == 0)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const unsigned element_shift = std::countr_zero(destination_immediate);
	if (element_shift > 3)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const std::size_t element_size = std::size_t{1} << element_shift;
	const std::size_t destination_lane =
		destination_immediate >> (element_shift + 1);
	const unsigned source_immediate = (instruction >> 11) & 0xfU;
	if ((source_immediate & (element_size - 1)) != 0)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const std::size_t source_lane = source_immediate >> element_shift;
	const std::size_t destination_offset = destination_lane * element_size;
	const std::size_t source_offset = source_lane * element_size;
	if (destination_offset + element_size > 16 ||
	    source_offset + element_size > 16)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};

	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	std::array<std::byte, 8> element{};
	std::copy_n(source.begin() + source_offset, element_size, element.begin());
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::copy_n(element.begin(), element_size,
		destination.begin() + destination_offset);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_duplicate_general(CpuState &cpu, GuestMemory &,
					     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned immediate = (instruction >> 16) & 0x1fU;
	const unsigned element_shift = immediate & 1U ? 0 :
		immediate & 2U ? 1 : immediate & 4U ? 2 :
		immediate & 8U ? 3 : 4;
	if (element_shift == 4)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const std::size_t element_size = std::size_t{1} << element_shift;
	const std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ?
		16 : 8;
	const auto source = cpu.read_register((instruction >> 5) & 0x1fU, false);
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	destination.fill(std::byte{});
	for (std::size_t offset = 0; offset < vector_size; offset += element_size)
		encode_little_endian(source,
			std::span{destination}.subspan(offset, element_size));
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_duplicate_element(CpuState &cpu, GuestMemory &,
					     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned immediate = (instruction >> 16) & 0x1fU;
	if (immediate == 0)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const unsigned element_shift = std::countr_zero(immediate);
	if (element_shift > 3)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const std::size_t element_size = std::size_t{1} << element_shift;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const std::size_t vector_size = scalar ? element_size :
		(instruction & 0x4000'0000U) != 0 ? 16 : 8;
	const std::size_t lane = immediate >> (element_shift + 1);
	const std::size_t source_offset = lane * element_size;
	if (source_offset + element_size > 16 || element_size > vector_size)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t offset = 0; offset < vector_size; offset += element_size)
		std::copy_n(source.begin() + source_offset, element_size,
			result.begin() + offset);
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::backend::arm64
