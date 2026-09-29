// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_semantic_instructions.hpp"

#include "cpu/arm64/detail/pointer_auth.hpp"

#include <cstdint>

namespace uml::cpu::arm64::jit::semantic {

RunResult pointer_auth_strip(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction)
{
	const auto address = cpu.pc;
	const bool link_register = instruction == 0xd503'20ffU;
	if (!cpu.pointer_authentication_enabled) {
		if (!link_register)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		cpu.pc += 4;
		return continue_after(address, instruction);
	}
	if (!link_register && ((instruction >> 5) & 31U) != 31U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = link_register ? 30U : instruction & 31U;
	const bool data = !link_register && (instruction & 0x400U) != 0;
	const auto source = cpu.read_register(destination, false);
	const auto operation = data ?
		arm64::detail::PointerAuthOperation::strip_data :
		arm64::detail::PointerAuthOperation::strip_instruction;
	const auto result =
		arm64::detail::execute_pointer_auth(cpu, source, 0, operation);
	cpu.write_register(destination, result, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult pointer_auth_code(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction)
{
	const auto address = cpu.pc;
	const bool hint_encoding =
		(instruction & 0xffff'f01fU) == 0xd503'201fU;
	if (!cpu.pointer_authentication_enabled) {
		if (!hint_encoding)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		cpu.pc += 4;
		return continue_after(address, instruction);
	}
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
	value = arm64::detail::execute_pointer_auth(
		cpu, value, context,
		static_cast<arm64::detail::PointerAuthOperation>(operation));
	cpu.write_register(destination, value, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult pointer_auth_generic(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction)
{
	const auto address = cpu.pc;
	if (!cpu.pointer_authentication_enabled)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = instruction & 31U;
	const auto source = cpu.read_register((instruction >> 5) & 31U, false);
	const auto modifier = cpu.read_register((instruction >> 16) & 31U, true);
	if (destination < 31U)
		cpu.write_register(destination,
			arm64::detail::execute_pointer_auth(
				cpu, source, modifier,
				arm64::detail::PointerAuthOperation::generic),
			false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult branch_authenticated(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction)
{
	const auto address = cpu.pc;
	if (!cpu.pointer_authentication_enabled)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const bool return_form =
		(instruction & 0xffff'fbffU) == 0xd65f'0bffU;
	const bool key_b = (instruction & 0x400U) != 0;
	const bool link = !return_form && (instruction & 0x0020'0000U) != 0;
	unsigned target_register;
	std::uint64_t modifier;
	bool zero_modifier;
	if (return_form) {
		target_register = 30;
		modifier = cpu.sp;
		zero_modifier = false;
	} else {
		target_register = (instruction >> 5) & 31U;
		const unsigned modifier_register = instruction & 31U;
		zero_modifier = (instruction & 0x0100'0000U) == 0;
		if (zero_modifier && modifier_register != 31U)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		modifier = zero_modifier ? 0 :
			cpu.read_register(modifier_register, true);
	}
	auto target = cpu.read_register(target_register, false);
	target = arm64::detail::execute_pointer_auth(
		cpu, target, modifier,
		key_b ? arm64::detail::PointerAuthOperation::authenticate_instruction_b :
			arm64::detail::PointerAuthOperation::authenticate_instruction_a);
	if (link)
		cpu.registers[30] = address + 4;
	cpu.pc = target;
	return continue_after(address, instruction);
}

} // namespace uml::cpu::arm64::jit::semantic
