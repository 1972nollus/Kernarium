// SPDX-License-Identifier: GPL-2.0-or-later
#include "../dispatcher.hpp"

#include "../detail/instruction_helpers.hpp"

#include <bit>
#include <cstdint>

namespace uml::backend::arm64 {

using detail::continue_after;
using detail::decode_bit_masks;
using detail::decode_logical_immediate;
using detail::rotate_right_width;
using detail::width_mask;

RunResult Dispatcher::move_wide(CpuState &cpu, GuestMemory &,
				std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	const unsigned operation = (instruction >> 29) & 0x3U;
	const unsigned shift = ((instruction >> 21) & 0x3U) * 16;
	if ((!is_64_bit && shift >= 32) || operation == 1)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto mask = width_mask(is_64_bit);
	const auto immediate = static_cast<std::uint64_t>(
		(instruction >> 5) & 0xffffU) << shift;
	const unsigned destination = instruction & 0x1fU;
	std::uint64_t result;
	if (operation == 0)
		result = ~immediate & mask;
	else if (operation == 2)
		result = immediate;
	else {
		const auto old = cpu.read_register(destination, false);
		result = (old & ~(0xffffULL << shift)) | immediate;
		result &= mask;
	}
	cpu.write_register(destination, result, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::logical_shifted(CpuState &cpu, GuestMemory &,
				      std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	const unsigned width = is_64_bit ? 64 : 32;
	const unsigned shift = (instruction >> 10) & 0x3fU;
	if (shift >= width)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto mask = width_mask(is_64_bit);
	const auto left = cpu.read_register((instruction >> 5) & 0x1fU, false) &
		mask;
	auto right = cpu.read_register((instruction >> 16) & 0x1fU, false) & mask;
	switch ((instruction >> 22) & 0x3U) {
	case 0: right = (right << shift) & mask; break;
	case 1: right >>= shift; break;
	case 2:
		if (is_64_bit)
			right = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(right) >> shift);
		else
			right = static_cast<std::uint32_t>(
				static_cast<std::int32_t>(right) >> shift);
		break;
	case 3:
		right = is_64_bit ? std::rotr(right, shift) :
			std::rotr(static_cast<std::uint32_t>(right), shift);
		break;
	}
	if ((instruction & 0x0020'0000U) != 0)
		right = ~right & mask;
	const unsigned operation = (instruction >> 29) & 0x3U;
	std::uint64_t result = 0;
	if (operation == 0 || operation == 3)
		result = left & right;
	else if (operation == 1)
		result = left | right;
	else
		result = left ^ right;
	result &= mask;
	if (operation == 3) {
		cpu.nzcv = ((result >> (width - 1)) != 0 ? 0x8000'0000U : 0) |
			(result == 0 ? 0x4000'0000U : 0);
	}
	cpu.write_register(instruction & 0x1fU, result, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::logical_immediate(CpuState &cpu, GuestMemory &,
					std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	const auto immediate = decode_logical_immediate(
		is_64_bit, (instruction >> 22) & 1U,
		(instruction >> 16) & 0x3fU, (instruction >> 10) & 0x3fU);
	if (!immediate)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto left = cpu.read_register((instruction >> 5) & 0x1fU, false);
	const unsigned operation = (instruction >> 29) & 0x3U;
	std::uint64_t result = operation == 0 || operation == 3 ?
		left & *immediate : operation == 1 ? left | *immediate :
		left ^ *immediate;
	result &= width_mask(is_64_bit);
	if (operation == 3) {
		const unsigned width = is_64_bit ? 64 : 32;
		cpu.nzcv = ((result >> (width - 1)) != 0 ? 0x8000'0000U : 0) |
			(result == 0 ? 0x4000'0000U : 0);
	}
	cpu.write_register(instruction & 0x1fU, result, operation != 3);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::bitfield(CpuState &cpu, GuestMemory &,
			       std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	const unsigned n = (instruction >> 22) & 1U;
	const unsigned rotate = (instruction >> 16) & 0x3fU;
	const unsigned imms = (instruction >> 10) & 0x3fU;
	const unsigned operation = (instruction >> 29) & 0x3U;
	if (operation == 3 || n != static_cast<unsigned>(is_64_bit) ||
	    (!is_64_bit && ((rotate | imms) & 0x20U) != 0))
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto masks = decode_bit_masks(
		is_64_bit, n, rotate, imms, false);
	if (!masks)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const unsigned width = is_64_bit ? 64 : 32;
	const auto width_bits = width_mask(is_64_bit);
	const auto source = cpu.read_register((instruction >> 5) & 0x1fU, false) &
		width_bits;
	const auto rotated = rotate_right_width(source, rotate, width);
	const auto destination = cpu.read_register(instruction & 0x1fU, false) &
		width_bits;
	std::uint64_t bottom = rotated & masks->write;
	std::uint64_t top = 0;
	if (operation == 0 && ((source >> imms) & 1U) != 0)
		top = width_bits;
	else if (operation == 1) {
		bottom |= destination & ~masks->write;
		top = destination;
	}
	const auto result = ((top & ~masks->test) |
		(bottom & masks->test)) & width_bits;
	cpu.write_register(instruction & 0x1fU, result, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::extract_register(CpuState &cpu, GuestMemory &,
				       std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool wide = (instruction & 0x8000'0000U) != 0;
	const unsigned width = wide ? 64 : 32;
	const unsigned shift = (instruction >> 10) & 0x3fU;
	if (shift >= width)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto mask = wide ? ~std::uint64_t{0} : 0xffff'ffffULL;
	const auto low = cpu.read_register((instruction >> 16) & 0x1fU, false) &
		mask;
	const auto high = cpu.read_register((instruction >> 5) & 0x1fU, false) &
		mask;
	const auto value = shift == 0 ? low :
		(low >> shift) | ((high << (width - shift)) & mask);
	cpu.write_register(instruction & 0x1fU, value & mask, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::variable_shift(CpuState &cpu, GuestMemory &,
				     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	const unsigned width = is_64_bit ? 64 : 32;
	const auto width_mask = is_64_bit ? ~std::uint64_t{0} :
		std::uint64_t{0xffff'ffffU};
	const auto value = cpu.read_register((instruction >> 5) & 0x1fU, false) &
		width_mask;
	const unsigned shift = static_cast<unsigned>(cpu.read_register(
		(instruction >> 16) & 0x1fU, false)) & (width - 1);
	const unsigned operation = (instruction >> 10) & 0x3U;
	std::uint64_t result;
	if (operation == 0)
		result = (value << shift) & width_mask;
	else if (operation == 1)
		result = value >> shift;
	else if (operation == 2) {
		if (is_64_bit)
			result = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(value) >> shift);
		else
			result = static_cast<std::uint32_t>(
				static_cast<std::int32_t>(value) >> shift);
	} else {
		result = shift == 0 ? value :
			((value >> shift) | (value << (width - shift))) & width_mask;
	}
	cpu.write_register(instruction & 0x1fU, result & width_mask, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::bit_reverse_count(CpuState &cpu, GuestMemory &,
					std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	const unsigned width = is_64_bit ? 64 : 32;
	const auto mask = is_64_bit ? ~std::uint64_t{0} :
		std::uint64_t{0xffff'ffffU};
	const auto value = cpu.read_register((instruction >> 5) & 0x1fU, false) &
		mask;
	const unsigned operation = (instruction >> 10) & 0x1fU;
	std::uint64_t result = 0;
	if (operation == 0) {
		for (unsigned bit = 0; bit < width; ++bit)
			result |= ((value >> bit) & 1U) << (width - 1 - bit);
	} else if (operation >= 1 && operation <= 3) {
		const unsigned group_bytes = 1U << operation;
		const unsigned total_bytes = width / 8;
		if (group_bytes > total_bytes)
			return {StopReason::unsupported_instruction, address,
				instruction, 0, false};
		for (unsigned group = 0; group < total_bytes;
		     group += group_bytes)
			for (unsigned byte = 0; byte < group_bytes; ++byte) {
				const auto source = group + byte;
				const auto destination = group + group_bytes - 1 - byte;
				result |= ((value >> (source * 8)) & 0xffU) <<
					(destination * 8);
			}
	} else if (operation == 4 || operation == 5) {
		if (is_64_bit) {
			const auto count_value = operation == 5 && (value >> 63) != 0 ?
				~value : value;
			result = std::countl_zero(count_value);
		} else {
			const auto narrowed = static_cast<std::uint32_t>(value);
			const auto count_value = operation == 5 && (narrowed >> 31) != 0 ?
				~narrowed : narrowed;
			result = std::countl_zero(count_value);
		}
		if (operation == 5)
			--result;
	} else {
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	}
	cpu.write_register(instruction & 0x1fU, result & mask, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::backend::arm64
