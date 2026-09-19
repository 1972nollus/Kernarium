// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_semantic_instructions.hpp"

#include "cpu/arm64/detail/gcs.hpp"
#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/pointer_auth.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::cpu::arm64::jit::semantic {
namespace {

using arm64::detail::GcsOperation;

[[nodiscard]] RunResult illegal(const CpuState &cpu,
	std::uint32_t instruction) noexcept
{
	return {StopReason::illegal_instruction, cpu.pc, instruction, 0, false};
}

[[nodiscard]] RunResult gcs_fault(const CpuState &cpu,
	std::uint32_t instruction) noexcept
{
	return {StopReason::gcs_fault, cpu.pc, instruction, 0, false};
}

[[nodiscard]] RunResult memory_fault(std::uint32_t instruction,
	std::uint64_t address, bool write) noexcept
{
	return {StopReason::memory_fault, address, instruction, 0, write};
}

[[nodiscard]] bool load_u64(GuestMemory &memory, std::uint64_t address,
	std::uint64_t &value)
{
	std::array<std::byte, 8> bytes{};
	if (!memory.read(address, bytes))
		return false;
	value = arm64::detail::decode_little_endian(bytes);
	return true;
}

[[nodiscard]] bool store_u64(GuestMemory &memory, std::uint64_t address,
	std::uint64_t value)
{
	std::array<std::byte, 8> bytes{};
	arm64::detail::encode_little_endian(value, bytes);
	return memory.write(address, bytes);
}

[[nodiscard]] bool compare_exchange_u64(GuestMemory &memory,
	std::uint64_t address, std::uint64_t compare, std::uint64_t replacement,
	std::uint64_t &observed)
{
	if ((address & 7U) != 0)
		return false;

	auto *pointer = memory.atomic_pointer(address, 8, MemoryAccess::write);
	if (pointer != nullptr &&
	    (reinterpret_cast<std::uintptr_t>(pointer) & 7U) == 0) {
		auto &storage = *reinterpret_cast<std::uint64_t *>(pointer);
		auto expected = compare;
		(void)std::atomic_ref<std::uint64_t>{storage}
			.compare_exchange_strong(expected, replacement,
				std::memory_order_relaxed, std::memory_order_relaxed);
		observed = expected;
		return true;
	}

	std::array<std::byte, 8> bytes{};
	if (!memory.read(address, bytes))
		return false;
	observed = arm64::detail::decode_little_endian(bytes);
	if (observed == compare)
		arm64::detail::encode_little_endian(replacement, bytes);
	/* An atomic read-modify-write still validates write permission when its
	 * comparison fails.  Writing the observed value gives fallback memories
	 * that validation without changing the architectural result. */
	return memory.write(address, bytes);
}

[[nodiscard]] RunResult finish(CpuState &cpu, std::uint64_t pc,
	std::uint32_t instruction) noexcept
{
	cpu.pc = pc + 4;
	return continue_after(pc, instruction);
}

} // namespace

RunResult guarded_control_stack(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction)
{
	const auto decoded = arm64::detail::decode_gcs(instruction);
	if (!decoded)
		return illegal(cpu, instruction);

	/* These exception-entry/return aliases are Undefined at EL0.  CpuState is
	 * deliberately an EL0-only model, so diagnose them before observing any
	 * feature bit, register, or memory. */
	if (arm64::detail::gcs_is_privileged_exception_operation(decoded.operation))
		return illegal(cpu, instruction);
	if (!cpu.gcs_implemented)
		return illegal(cpu, instruction);

	const auto pc = cpu.pc;
	switch (decoded.operation) {
	case GcsOperation::pop_main: {
		const auto address = cpu.gcs_pointer;
		std::uint64_t value;
		if (!load_u64(memory, address, value))
			return memory_fault(instruction, address, false);
		if ((value & 3U) != 0)
			return gcs_fault(cpu, instruction);
		cpu.gcs_pointer = (address + 8) & ~std::uint64_t{7};
		cpu.write_register(decoded.transfer, value, false);
		return finish(cpu, pc, instruction);
	}
	case GcsOperation::push_main: {
		if (!cpu.gcs_push_enabled)
			return illegal(cpu, instruction);
		const auto address = cpu.gcs_pointer - 8;
		const auto value = cpu.read_register(decoded.transfer, false);
		if (!store_u64(memory, address, value))
			return memory_fault(instruction, address, true);
		cpu.gcs_pointer = address & ~std::uint64_t{7};
		return finish(cpu, pc, instruction);
	}
	case GcsOperation::switch_stack_1: {
		const auto incoming =
			cpu.read_register(decoded.transfer, false);
		if ((incoming & 7U) != 0)
			return memory_fault(instruction, incoming, true);
		const auto compare =
			(incoming & ~std::uint64_t{0xfff}) | 1U;
		const auto replacement =
			(cpu.gcs_pointer & ~std::uint64_t{7}) | 5U;
		std::uint64_t observed;
		if (!compare_exchange_u64(memory, incoming, compare, replacement,
					 observed))
			return memory_fault(instruction, incoming, true);
		if (observed != compare)
			return gcs_fault(cpu, instruction);
		cpu.gcs_pointer = incoming & ~std::uint64_t{7};
		return finish(cpu, pc, instruction);
	}
	case GcsOperation::switch_stack_2: {
		const auto incoming = cpu.gcs_pointer;
		std::uint64_t outgoing_value;
		if (!load_u64(memory, incoming, outgoing_value))
			return memory_fault(instruction, incoming, false);
		if ((outgoing_value & 7U) != 5U)
			return gcs_fault(cpu, instruction);
		const auto outgoing =
			(outgoing_value & ~std::uint64_t{7}) - 8;
		const auto replacement =
			(outgoing & ~std::uint64_t{0xfff}) | 1U;
		if (!store_u64(memory, outgoing, replacement))
			return memory_fault(instruction, outgoing, true);
		cpu.gcs_pointer = (incoming + 8) & ~std::uint64_t{7};
		std::atomic_thread_fence(std::memory_order_seq_cst);
		cpu.write_register(decoded.transfer, outgoing, false);
		return finish(cpu, pc, instruction);
	}
	case GcsOperation::store:
	case GcsOperation::store_unprivileged: {
		/* CheckGCSSTREnabled precedes address generation and SP alignment. */
		if (!cpu.gcs_store_enabled)
			return gcs_fault(cpu, instruction);
		const auto tagged_address = cpu.read_register(decoded.base, true);
		const auto address = cpu.untag_data_address(
			tagged_address, decoded.base == 31U);
		if (decoded.base == 31U && (address & 15U) != 0)
			return memory_fault(instruction, address, true);
		const auto value = cpu.read_register(decoded.transfer, false);
		if (!store_u64(memory, address, value))
			return memory_fault(instruction, address, true);
		return finish(cpu, pc, instruction);
	}
	case GcsOperation::none:
	case GcsOperation::pop_exception_context:
	case GcsOperation::pop_exception:
	case GcsOperation::push_exception:
		break;
	}
	return illegal(cpu, instruction);
}

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

RunResult branch_authenticated(CpuState &cpu, GuestMemory &memory,
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
	if (cpu.gcs_implemented && cpu.gcs_push_enabled) {
		if (link) {
			const auto record_address =
				cpu.gcs_pointer - sizeof(std::uint64_t);
			std::array<std::byte, sizeof(std::uint64_t)> bytes{};
			arm64::detail::encode_little_endian(address + 4, bytes);
			if (!memory.write(record_address, bytes)) {
				const auto fault = memory.last_fault();
				return {StopReason::memory_fault, fault.address,
					instruction, 0, true};
			}
			cpu.gcs_pointer =
				record_address & ~std::uint64_t{7};
		} else if (return_form) {
			const auto record_address = cpu.gcs_pointer;
			std::array<std::byte, sizeof(std::uint64_t)> bytes{};
			if (!memory.read(record_address, bytes)) {
				const auto fault = memory.last_fault();
				return {StopReason::memory_fault, fault.address,
					instruction, 0, false};
			}
			const auto recorded =
				arm64::detail::decode_little_endian(bytes);
			if (recorded != target)
				return {StopReason::gcs_fault, address,
					instruction, 0, false};
			target = recorded;
			cpu.gcs_pointer =
				(record_address + sizeof(recorded)) &
				~std::uint64_t{7};
		}
	}
	if (link)
		cpu.registers[30] = address + 4;
	cpu.pc = target;
	return continue_after(address, instruction);
}

} // namespace uml::cpu::arm64::jit::semantic
