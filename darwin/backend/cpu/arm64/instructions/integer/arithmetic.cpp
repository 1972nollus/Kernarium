// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/instruction_helpers.hpp"

#include <cstdint>

namespace uml::backend::arm64 {

using detail::condition_holds;
using detail::continue_after;
using detail::set_add_sub_flags;
using detail::width_mask;

RunResult Dispatcher::add_sub_immediate(CpuState &cpu, GuestMemory &,
					std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	const bool subtract = (instruction & 0x4000'0000U) != 0;
	const bool set_flags = (instruction & 0x2000'0000U) != 0;
	const auto mask = width_mask(is_64_bit);
	const auto left = cpu.read_register((instruction >> 5) & 0x1fU, true) &
		mask;
	const auto right = static_cast<std::uint64_t>(
		(instruction >> 10) & 0xfffU) <<
		(((instruction >> 22) & 1U) != 0 ? 12 : 0);
	const auto result = (subtract ? left - right : left + right) & mask;
	if (set_flags)
		set_add_sub_flags(cpu, left, right, result, is_64_bit, subtract);
	cpu.write_register(instruction & 0x1fU, result, !set_flags);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::add_sub_register(CpuState &cpu, GuestMemory &,
				       std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	const bool subtract = (instruction & 0x4000'0000U) != 0;
	const bool set_flags = (instruction & 0x2000'0000U) != 0;
	const bool extended = (instruction & 0x0020'0000U) != 0;
	const auto mask = width_mask(is_64_bit);
	const auto left = cpu.read_register((instruction >> 5) & 0x1fU,
		extended) & mask;
	std::uint64_t right = cpu.read_register(
		(instruction >> 16) & 0x1fU, false);
	if (extended) {
		const unsigned option = (instruction >> 13) & 0x7U;
		const unsigned shift = (instruction >> 10) & 0x7U;
		if (shift > 4)
			return {StopReason::unsupported_instruction, address,
				instruction, 0, false};
		const unsigned source_width = 8U << (option & 0x3U);
		const auto source_mask = source_width == 64 ? ~std::uint64_t{0} :
			(std::uint64_t{1} << source_width) - 1;
		right &= source_mask;
		if ((option & 0x4U) != 0 &&
		    (right & (std::uint64_t{1} << (source_width - 1))) != 0)
			right |= ~source_mask;
		right = (right << shift) & mask;
	} else {
		const unsigned shift_type = (instruction >> 22) & 0x3U;
		const unsigned shift = (instruction >> 10) & 0x3fU;
		const unsigned width = is_64_bit ? 64 : 32;
		if (shift_type == 3 || shift >= width)
			return {StopReason::unsupported_instruction, address,
				instruction, 0, false};
		right &= mask;
		if (shift_type == 0)
			right = (right << shift) & mask;
		else if (shift_type == 1)
			right >>= shift;
		else if (is_64_bit)
			right = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(right) >> shift);
		else
			right = static_cast<std::uint32_t>(
				static_cast<std::int32_t>(right) >> shift);
	}
	const auto result = (subtract ? left - right : left + right) & mask;
	if (set_flags)
		set_add_sub_flags(cpu, left, right, result, is_64_bit, subtract);
	cpu.write_register(instruction & 0x1fU, result, extended && !set_flags);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::add_sub_carry(CpuState &cpu, GuestMemory &,
				    std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction & 0x8000'0000U) != 0;
	const bool subtract = (instruction & 0x4000'0000U) != 0;
	const bool set_flags = (instruction & 0x2000'0000U) != 0;
	const unsigned width = is_64_bit ? 64 : 32;
	const auto mask = width_mask(is_64_bit);
	const auto left = cpu.read_register((instruction >> 5) & 0x1fU, false) &
		mask;
	const auto right = cpu.read_register((instruction >> 16) & 0x1fU, false) &
		mask;
	const std::uint64_t carry = (cpu.nzcv >> 29) & 1U;
	const unsigned __int128 unsigned_value = subtract ?
		static_cast<unsigned __int128>(left) + (mask ^ right) + carry :
		static_cast<unsigned __int128>(left) + right + carry;
	const auto result = static_cast<std::uint64_t>(unsigned_value) & mask;
	if (set_flags) {
		const auto sign_bit = std::uint64_t{1} << (width - 1);
		const bool negative = (result & sign_bit) != 0;
		const bool zero = result == 0;
		const bool carry_out = (unsigned_value >> width) != 0;
		const bool left_negative = (left & sign_bit) != 0;
		const bool right_negative = (right & sign_bit) != 0;
		const bool overflow = subtract ?
			(left_negative != right_negative && negative != left_negative) :
			(left_negative == right_negative && negative != left_negative);
		cpu.nzcv = (negative ? 0x8000'0000U : 0) |
			(zero ? 0x4000'0000U : 0) |
			(carry_out ? 0x2000'0000U : 0) |
			(overflow ? 0x1000'0000U : 0);
	}
	cpu.write_register(instruction & 0x1fU, result, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::conditional_select(CpuState &cpu, GuestMemory &,
					 std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	const auto mask = width_mask(is_64_bit);
	const unsigned condition = (instruction >> 12) & 0xfU;
	std::uint64_t result;
	if (condition_holds(cpu.nzcv, condition)) {
		result = cpu.read_register((instruction >> 5) & 0x1fU, false);
	} else {
		result = cpu.read_register((instruction >> 16) & 0x1fU, false);
		const unsigned operation = ((instruction >> 29) & 0x2U) |
			((instruction >> 10) & 1U);
		if (operation == 1)
			++result;
		else if (operation == 2)
			result = ~result;
		else if (operation == 3)
			result = -result;
	}
	cpu.write_register(instruction & 0x1fU, result & mask, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::conditional_compare(CpuState &cpu, GuestMemory &,
					  std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	if (condition_holds(cpu.nzcv, (instruction >> 12) & 0xfU)) {
		const auto mask = width_mask(is_64_bit);
		const auto left = cpu.read_register((instruction >> 5) & 0x1fU,
			false) & mask;
		const auto right = (instruction & 0x0000'0800U) != 0 ?
			static_cast<std::uint64_t>((instruction >> 16) & 0x1fU) :
			cpu.read_register((instruction >> 16) & 0x1fU, false) & mask;
		const bool subtract = (instruction & 0x4000'0000U) != 0;
		const auto result = (subtract ? left - right : left + right) & mask;
		set_add_sub_flags(cpu, left, right, result, is_64_bit, subtract);
	} else {
		cpu.nzcv = (instruction & 0xfU) << 28;
	}
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::multiply_add(CpuState &cpu, GuestMemory &,
				   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	const unsigned operation = (instruction >> 21) & 0x7U;
	if (((instruction >> 29) & 0x3U) != 0 ||
	    (operation != 0 && !is_64_bit) ||
	    (operation != 0 && operation != 1 && operation != 2 &&
	     operation != 5 && operation != 6))
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto mask = width_mask(is_64_bit);
	const auto left = cpu.read_register((instruction >> 5) & 0x1fU, false) &
		mask;
	const auto right = cpu.read_register((instruction >> 16) & 0x1fU, false) &
		mask;
	const auto addend = cpu.read_register((instruction >> 10) & 0x1fU, false) &
		mask;
	std::uint64_t result;
	if (operation == 0) {
		const auto product = (left * right) & mask;
		result = (instruction & 0x0000'8000U) != 0 ?
			addend - product : addend + product;
		result &= mask;
	} else if (operation == 1 || operation == 5) {
		const std::uint64_t product = operation == 1 ?
			static_cast<std::uint64_t>(
				static_cast<std::int64_t>(static_cast<std::int32_t>(left)) *
				static_cast<std::int64_t>(static_cast<std::int32_t>(right))) :
			static_cast<std::uint64_t>(static_cast<std::uint32_t>(left)) *
				static_cast<std::uint64_t>(static_cast<std::uint32_t>(right));
		result = (instruction & 0x0000'8000U) != 0 ?
			addend - product : addend + product;
	} else {
		if ((instruction & 0x0000'8000U) != 0)
			return {StopReason::unsupported_instruction, address,
				instruction, 0, false};
		if (operation == 2) {
			const __int128 product = static_cast<__int128>(
				static_cast<std::int64_t>(left)) *
				static_cast<__int128>(static_cast<std::int64_t>(right));
			result = static_cast<std::uint64_t>(
				static_cast<unsigned __int128>(product) >> 64);
		} else {
			const unsigned __int128 product =
				static_cast<unsigned __int128>(left) * right;
			result = static_cast<std::uint64_t>(product >> 64);
		}
	}
	cpu.write_register(instruction & 0x1fU, result, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::divide(CpuState &cpu, GuestMemory &,
			     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	const bool signed_divide = (instruction & 0x400U) != 0;
	const auto mask = width_mask(is_64_bit);
	const auto dividend = cpu.read_register(
		(instruction >> 5) & 0x1fU, false) & mask;
	const auto divisor = cpu.read_register(
		(instruction >> 16) & 0x1fU, false) & mask;
	std::uint64_t result = 0;
	if (divisor != 0) {
		if (!signed_divide) {
			result = dividend / divisor;
		} else if (is_64_bit) {
			constexpr auto minimum = std::uint64_t{1} << 63;
			if (dividend == minimum && divisor == mask)
				result = minimum;
			else
				result = static_cast<std::uint64_t>(
					static_cast<std::int64_t>(dividend) /
					static_cast<std::int64_t>(divisor));
		} else {
			constexpr auto minimum = std::uint32_t{1} << 31;
			if (dividend == minimum && divisor == 0xffff'ffffU)
				result = minimum;
			else
				result = static_cast<std::uint32_t>(
					static_cast<std::int32_t>(dividend) /
					static_cast<std::int32_t>(divisor));
		}
	}
	cpu.write_register(instruction & 0x1fU, result & mask, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::backend::arm64
