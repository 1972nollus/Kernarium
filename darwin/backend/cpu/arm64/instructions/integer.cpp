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
	} else if (operation == 6) {
		result = is_64_bit ? std::countr_zero(value) :
			std::countr_zero(static_cast<std::uint32_t>(value));
	} else if (operation == 7) {
		result = is_64_bit ? std::popcount(value) :
			std::popcount(static_cast<std::uint32_t>(value));
	} else if (operation == 8) {
		const auto sign = value >> (width - 1);
		result = sign == 0 ? value : (~value + 1) & mask;
	} else {
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	}
	cpu.write_register(instruction & 0x1fU, result & mask, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::pointer_auth_strip(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool link_register = instruction == 0xd503'20ffU;
	if (!link_register && ((instruction >> 5) & 31U) != 31U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = link_register ? 30U : instruction & 31U;
	const bool data = !link_register && (instruction & 0x400U) != 0;
	const auto source = cpu.read_register(destination, false);
	const bool upper_range = (source & (std::uint64_t{1} << 55)) != 0;
	/* The Darwin-hosted static Linux profile uses a 47-bit user VA and
	 * instruction TBID. Data TBI is enabled for the lower range (TBI0) and
	 * disabled for the upper range (TBI1), selected by retained address bit 55.
	 */
	std::uint64_t result;
	if (data && !upper_range) {
		result = source & 0xff00'7fff'ffff'ffffULL;
		if (upper_range)
			result |= 0x00ff'8000'0000'0000ULL;
	} else {
		result = source & 0x0000'7fff'ffff'ffffULL;
		if (upper_range)
			result |= 0xffff'8000'0000'0000ULL;
	}
	cpu.write_register(destination, result, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::pointer_auth_code(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	unsigned operation;
	unsigned destination;
	unsigned modifier;
	bool zero_modifier;
	if ((instruction & 0xffff'c000U) == 0xdac1'0000U) {
		const unsigned encoded_operation = (instruction >> 10) & 15U;
		operation = encoded_operation & 7U;
		destination = instruction & 31U;
		modifier = (instruction >> 5) & 31U;
		zero_modifier = encoded_operation >= 8U;
		/* The Z forms have a fixed 11111 field. Validate the complete
		 * encoding before reading or modifying any architectural state. */
		if (zero_modifier && modifier != 31U)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
	} else {
		destination = ((instruction >> 5) & 0x7fU) < 16U ? 17U : 30U;
		switch ((instruction >> 5) & 0x7fU) {
		case 8: operation = 0; modifier = 16; zero_modifier = false; break;
		case 10: operation = 1; modifier = 16; zero_modifier = false; break;
		case 12: operation = 4; modifier = 16; zero_modifier = false; break;
		case 14: operation = 5; modifier = 16; zero_modifier = false; break;
		case 24: operation = 0; modifier = 0; zero_modifier = true; break;
		case 25: operation = 0; modifier = 31; zero_modifier = false; break;
		case 26: operation = 1; modifier = 0; zero_modifier = true; break;
		case 27: operation = 1; modifier = 31; zero_modifier = false; break;
		case 28: operation = 4; modifier = 0; zero_modifier = true; break;
		case 29: operation = 4; modifier = 31; zero_modifier = false; break;
		case 30: operation = 5; modifier = 0; zero_modifier = true; break;
		case 31: operation = 5; modifier = 31; zero_modifier = false; break;
		default:
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		}
	}

	/* XZR discards a valid operation's result. Base FEAT_PAuth does not
	 * include fault-on-authentication-failure, so it has no other state. */
	if (destination == 31U) {
		cpu.pc += 4;
		return continue_after(address, instruction);
	}
	auto value = cpu.read_register(destination, false);
	const auto context = zero_modifier ? 0 :
		cpu.read_register(modifier, true);
	if (zero_modifier) {
		switch (operation) {
		case 0: __asm__ volatile("paciza %0" : "+r"(value)); break;
		case 1: __asm__ volatile("pacizb %0" : "+r"(value)); break;
		case 2: __asm__ volatile("pacdza %0" : "+r"(value)); break;
		case 3: __asm__ volatile("pacdzb %0" : "+r"(value)); break;
		case 4: __asm__ volatile("autiza %0" : "+r"(value)); break;
		case 5: __asm__ volatile("autizb %0" : "+r"(value)); break;
		case 6: __asm__ volatile("autdza %0" : "+r"(value)); break;
		case 7: __asm__ volatile("autdzb %0" : "+r"(value)); break;
		}
	} else {
		switch (operation) {
		case 0: __asm__ volatile("pacia %0, %1" : "+r"(value) : "r"(context)); break;
		case 1: __asm__ volatile("pacib %0, %1" : "+r"(value) : "r"(context)); break;
		case 2: __asm__ volatile("pacda %0, %1" : "+r"(value) : "r"(context)); break;
		case 3: __asm__ volatile("pacdb %0, %1" : "+r"(value) : "r"(context)); break;
		case 4: __asm__ volatile("autia %0, %1" : "+r"(value) : "r"(context)); break;
		case 5: __asm__ volatile("autib %0, %1" : "+r"(value) : "r"(context)); break;
		case 6: __asm__ volatile("autda %0, %1" : "+r"(value) : "r"(context)); break;
		case 7: __asm__ volatile("autdb %0, %1" : "+r"(value) : "r"(context)); break;
		}
	}
	cpu.write_register(destination, value, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::pointer_auth_generic(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned destination = instruction & 31U;
	const auto source = cpu.read_register((instruction >> 5) & 31U, false);
	const auto modifier = cpu.read_register((instruction >> 16) & 31U, true);
	if (destination < 31U) {
		std::uint64_t result;
		__asm__ volatile("pacga %0, %1, %2" : "=&r"(result) :
			"r"(source), "r"(modifier));
		cpu.write_register(destination, result, false);
	}
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::integer_min_max(CpuState &cpu, GuestMemory &,
				      std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	const auto mask = width_mask(is_64_bit);
	const bool immediate =
		(instruction & 0x7ff0'0000U) == 0x11c0'0000U;
	const unsigned operation = immediate ? (instruction >> 18) & 3U :
		(instruction >> 10) & 3U;
	const bool is_unsigned = (operation & 1U) != 0;
	const bool minimum = (operation & 2U) != 0;
	const auto left = cpu.read_register(
		(instruction >> 5) & 0x1fU, false) & mask;
	std::uint64_t right;
	if (immediate) {
		const auto encoded = static_cast<std::uint8_t>(instruction >> 10);
		right = is_unsigned ? encoded : static_cast<std::uint64_t>(
			static_cast<std::int64_t>(static_cast<std::int8_t>(encoded)));
		right &= mask;
	} else {
		right = cpu.read_register(
			(instruction >> 16) & 0x1fU, false) & mask;
	}
	bool left_is_less;
	if (is_unsigned)
		left_is_less = left < right;
	else if (is_64_bit)
		left_is_less = static_cast<std::int64_t>(left) <
			static_cast<std::int64_t>(right);
	else
		left_is_less = static_cast<std::int32_t>(left) <
			static_cast<std::int32_t>(right);
	const auto result = minimum ? (left_is_less ? left : right) :
		(left_is_less ? right : left);
	cpu.write_register(instruction & 0x1fU, result & mask, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::flag_manipulation(CpuState &cpu, GuestMemory &,
					std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	if (instruction == 0xd500'401fU) {
		cpu.nzcv ^= 0x2000'0000U;
	} else if (instruction == 0xd500'405fU) {
		const bool z = (cpu.nzcv & 0x4000'0000U) != 0;
		const bool c = (cpu.nzcv & 0x2000'0000U) != 0;
		const bool v = (cpu.nzcv & 0x1000'0000U) != 0;
		cpu.nzcv = ((z || v) ? 0x4000'0000U : 0) |
			((c && !v) ? 0x2000'0000U : 0);
	} else if (instruction == 0xd500'403fU) {
		const bool z = (cpu.nzcv & 0x4000'0000U) != 0;
		const bool c = (cpu.nzcv & 0x2000'0000U) != 0;
		cpu.nzcv = ((!c && !z) ? 0x8000'0000U : 0) |
			((z && c) ? 0x4000'0000U : 0) |
			((c || z) ? 0x2000'0000U : 0) |
			((!c && z) ? 0x1000'0000U : 0);
	} else if ((instruction & 0xffe0'7c10U) == 0xba00'0400U) {
		const auto value = cpu.read_register(
			(instruction >> 5) & 31U, false);
		const auto flags = rotate_right_width(
			value, (instruction >> 15) & 63U, 64) & 15U;
		const auto mask = static_cast<std::uint32_t>(instruction & 15U) << 28;
		cpu.nzcv = (cpu.nzcv & ~mask) |
			((static_cast<std::uint32_t>(flags) << 28) & mask);
	} else {
		const unsigned size = (instruction & 0x4000U) != 0 ? 16 : 8;
		const auto value = static_cast<std::uint32_t>(cpu.read_register(
			(instruction >> 5) & 31U, false));
		const bool n = ((value >> (size - 1)) & 1U) != 0;
		const bool z = (value & ((1U << size) - 1)) == 0;
		const bool v = ((value >> size) ^ (value >> (size - 1))) & 1U;
		cpu.nzcv = (cpu.nzcv & 0x2000'0000U) |
			(n ? 0x8000'0000U : 0) |
			(z ? 0x4000'0000U : 0) |
			(v ? 0x1000'0000U : 0);
	}
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::pstate_immediate(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned op1 = (instruction >> 16) & 7U;
	const unsigned immediate = (instruction >> 8) & 15U;
	const unsigned op2 = (instruction >> 5) & 7U;
	/* EL0 may only access the op1=011 group. Validate the complete encoding
	 * before changing any field so malformed SME and gated DAIF operations are
	 * side-effect free in the reference interpreter. */
	if (op1 != 3U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const bool value = (immediate & 1U) != 0;
	switch (op2) {
	case 1: cpu.pstate_ssbs = value; break;
	case 2: cpu.pstate_dit = value; break;
	case 3:
		switch ((immediate >> 1) & 3U) {
		case 1: cpu.pstate_sm = value; break;
		case 2: cpu.pstate_za = value; break;
		case 3:
			cpu.pstate_sm = value;
			cpu.pstate_za = value;
			break;
		default:
			return {StopReason::illegal_instruction, address, instruction,
				0, false};
		}
		break;
	case 4: cpu.pstate_tco = value; break;
	case 6:
	case 7:
		if (!cpu.pstate_daif_access)
			return {StopReason::illegal_instruction, address, instruction,
				0, false};
		if (op2 == 6)
			cpu.pstate_daif |= immediate;
		else
			cpu.pstate_daif &= static_cast<std::uint8_t>(~immediate);
		cpu.pstate_daif &= 15U;
		break;
	default:
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	}
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::backend::arm64
