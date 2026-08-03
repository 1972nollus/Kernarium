// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/instruction_helpers.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::cpu::arm64 {

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
		return {StopReason::illegal_instruction, address, instruction, 0,
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
	const bool q = (instruction & 0x4000'0000U) != 0;
	const unsigned size = (instruction >> 22) & 0x3U;
	const unsigned opcode = (instruction >> 12) & 0x7U;
	const bool allocated_operation = opcode == 1U || opcode == 2U ||
		opcode == 3U || opcode == 5U || opcode == 6U || opcode == 7U;
	if (!allocated_operation || (!q && size == 3U))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};

	const std::size_t vector_size = q ? 16 : 8;
	const std::size_t element_size = std::size_t{1} << size;
	const std::size_t element_count = vector_size / element_size;
	const auto left = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	const auto copy_element = [&](std::size_t destination,
					  const auto &source, std::size_t source_index) {
		std::copy_n(source.begin() + source_index * element_size,
			element_size, result.begin() + destination * element_size);
	};
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
	/* Snapshot every architectural input before producing any result. This
	 * makes destructive aliases and wrapped table lists explicit in the safe
	 * interpreter path.
	 */
	const auto indices = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	const auto old_destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::array<std::byte, 16>, 4> tables{};
	for (unsigned table = 0; table < table_count; ++table)
		tables[table] =
			cpu.vectors[(first_table + table) & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t output = 0; output < vector_size; ++output) {
		const auto index = std::to_integer<unsigned>(indices[output]);
		if (index < table_count * 16U) {
			result[output] =
				tables[index / 16U][index & 0xfU];
		} else if (extend) {
			result[output] = old_destination[output];
		}
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_lut_lookup(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned op2 = (instruction >> 22) & 3U;
	const unsigned len = (instruction >> 13) & 3U;
	const unsigned op = (instruction >> 12) & 1U;
	unsigned index_bits;
	unsigned element_bytes;
	unsigned part;
	unsigned table_count;
	if (op2 == 1U) {
		/* LUTI4 B requires len[0]=1, while LUTI4 H allocates every len. */
		if (op == 0U && (len & 1U) == 0U)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		index_bits = 4U;
		element_bytes = op == 0U ? 1U : 2U;
		part = op == 0U ? len >> 1 : len;
		table_count = op == 0U ? 1U : 2U;
	} else if (op2 == 2U) {
		if (op == 0U)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		index_bits = 2U;
		element_bytes = 1U;
		part = len;
		table_count = 1U;
	} else if (op2 == 3U) {
		index_bits = 2U;
		element_bytes = 2U;
		part = (len << 1) | op;
		table_count = 1U;
	} else {
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	}

	const unsigned destination = instruction & 31U;
	const unsigned table_register = (instruction >> 5) & 31U;
	const unsigned indices_register = (instruction >> 16) & 31U;
	const auto indices = cpu.vectors[indices_register].bytes;
	const auto table1 = cpu.vectors[table_register].bytes;
	const auto table2 = cpu.vectors[(table_register + 1U) & 31U].bytes;
	std::array<std::byte, 16> result{};
	const unsigned elements = 16U / element_bytes;
	const unsigned index_mask = (1U << index_bits) - 1U;
	const unsigned first_index = elements * part;
	for (unsigned element = 0; element < elements; ++element) {
		const unsigned bit = (first_index + element) * index_bits;
		const unsigned index =
			(std::to_integer<unsigned>(indices[bit / 8U]) >> (bit & 7U)) &
			index_mask;
		const unsigned table_elements = 16U / element_bytes;
		const bool second = index >= table_elements;
		if (second && table_count == 1U)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		const auto &table = second ? table2 : table1;
		const unsigned table_index = second ? index - table_elements : index;
		std::copy_n(table.begin() + table_index * element_bytes,
			element_bytes, result.begin() + element * element_bytes);
	}
	cpu.vectors[destination].bytes = result;
	cpu.pc += 4U;
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
		const auto lane = detail::decode_simd_lane_index(immediate);
		const bool is_64_bit = (instruction & 0x4000'0000U) != 0;
		const bool valid = lane && (signed_move ?
			(lane->element_shift < (is_64_bit ? 3U : 2U)) :
			(is_64_bit ? lane->element_shift == 3U :
			 lane->element_shift < 3U));
		if (!valid)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		auto value = detail::decode_little_endian(
			std::span{cpu.vectors[(instruction >> 5) & 0x1fU].bytes}
				.subspan(lane->offset, lane->element_size));
		if (signed_move)
			value = static_cast<std::uint64_t>(detail::sign_extend(value,
				lane->element_size * 8U));
		if (!is_64_bit)
			value &= 0xffff'ffffU;
		cpu.write_register(instruction & 0x1fU, value, false);
		cpu.pc += 4;
		return continue_after(address, instruction);
	}
	const auto opcode = instruction & 0xffff'fc00U;
	const unsigned source = (instruction >> 5) & 0x1fU;
	const unsigned destination = instruction & 0x1fU;
	switch (opcode) {
	case 0x1ee6'0000U:
	case 0x9ee6'0000U:
		cpu.write_register(destination,
			vector_half(cpu.vectors[source], 0) & 0xffffU, false);
		break;
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
	case 0x1ee7'0000U:
	case 0x9ee7'0000U:
		set_vector_half(cpu.vectors[destination], 0,
			cpu.read_register(source, false) & 0xffffU);
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
	const auto lane = detail::decode_simd_lane_index(immediate);
	if (!lane)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const auto source = cpu.read_register((instruction >> 5) & 0x1fU, false);
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	encode_little_endian(source,
		std::span{destination}.subspan(lane->offset, lane->element_size));
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_insert_element(CpuState &cpu, GuestMemory &,
					   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned destination_immediate = (instruction >> 16) & 0x1fU;
	const auto destination_lane =
		detail::decode_simd_lane_index(destination_immediate);
	if (!destination_lane)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned source_immediate = (instruction >> 11) & 0xfU;
	const std::size_t source_offset = source_immediate &
		~(destination_lane->element_size - 1U);

	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	std::array<std::byte, 8> element{};
	std::copy_n(source.begin() + source_offset, destination_lane->element_size,
		element.begin());
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::copy_n(element.begin(), destination_lane->element_size,
		destination.begin() + destination_lane->offset);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_duplicate_general(CpuState &cpu, GuestMemory &,
					     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned immediate = (instruction >> 16) & 0x1fU;
	const auto lane = detail::decode_simd_lane_index(immediate);
	const std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ?
		16 : 8;
	if (!lane || (vector_size == 8 && lane->element_shift == 3U))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const auto source = cpu.read_register((instruction >> 5) & 0x1fU, false);
	auto &destination = cpu.vectors[instruction & 0x1fU].bytes;
	destination.fill(std::byte{});
	for (std::size_t offset = 0; offset < vector_size;
	     offset += lane->element_size)
		encode_little_endian(source,
			std::span{destination}.subspan(offset, lane->element_size));
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_duplicate_element(CpuState &cpu, GuestMemory &,
					     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned immediate = (instruction >> 16) & 0x1fU;
	const auto lane = detail::decode_simd_lane_index(immediate);
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	if (!lane || (!scalar && (instruction & 0x4000'0000U) == 0 &&
		lane->element_shift == 3U))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const std::size_t vector_size = scalar ? lane->element_size :
		(instruction & 0x4000'0000U) != 0 ? 16 : 8;
	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t offset = 0; offset < vector_size;
	     offset += lane->element_size)
		std::copy_n(source.begin() + lane->offset, lane->element_size,
			result.begin() + offset);
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::cpu::arm64
