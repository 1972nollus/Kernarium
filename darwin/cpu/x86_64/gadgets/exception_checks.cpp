// SPDX-License-Identifier: GPL-2.0-or-later
#include "exception_checks.hpp"

namespace uml::cpu::x86_64::gadgets {
namespace {

[[nodiscard]] constexpr unsigned modrm_reg(std::uint8_t modrm) noexcept
{
	return (modrm >> 3U) & 7U;
}

[[nodiscard]] constexpr bool modrm_register(std::uint8_t modrm) noexcept
{
	return (modrm >> 6U) == 3U;
}

[[nodiscard]] bool fwait(const Instruction &instruction) noexcept
{
	return instruction.operation == Operation::nop &&
		instruction.vector_opcode == 0x9b;
}

[[nodiscard]] bool uses_mmx(const Instruction &instruction) noexcept
{
	return instruction.destination.kind == OperandKind::mmx_reg ||
		instruction.source.kind == OperandKind::mmx_reg ||
		instruction.auxiliary.kind == OperandKind::mmx_reg;
}

/* An x87 escape, D8 to DF, rather than EMMS or FEMMS. */
[[nodiscard]] bool x87_escape(const Instruction &instruction) noexcept
{
	return instruction.operation == Operation::x87 &&
		instruction.vector_opcode >= 0xd8;
}

/* The bytes an x87 memory form reads or writes, which is also the
 * alignment #AC asks of it: the environment and state images want four,
 * or two in their 16-bit form, and an 80-bit operand wants eight. */
[[nodiscard]] std::size_t x87_memory_alignment(
	const Instruction &instruction) noexcept
{
	const auto reg = modrm_reg(instruction.vector_control);
	const std::size_t image = instruction.vector_prefix != 0 ? 2U : 4U;
	switch (instruction.vector_opcode) {
	case 0xd8:
	case 0xda:
		return 4;
	case 0xdc:
		return 8;
	case 0xde:
		return 2;
	case 0xd9:
		switch (reg) {
		case 4:
		case 6: return image;
		case 5:
		case 7: return 2;
		default: return 4;
		}
	case 0xdb:
		return reg == 5 || reg == 7 ? 8U : 4U;
	case 0xdd:
		switch (reg) {
		case 4:
		case 6: return image;
		case 7: return 2;
		default: return 8;
		}
	case 0xdf:
		return reg >= 4 ? 8U : 2U;
	default:
		return 0;
	}
}

[[nodiscard]] std::size_t stack_bytes(const CpuState &cpu) noexcept
{
	return cpu.execution_mode == ExecutionMode::compat32 ? 4U : 8U;
}

[[nodiscard]] std::uint64_t stack_address(const CpuState &cpu,
	std::uint64_t address) noexcept
{
	return cpu.execution_mode == ExecutionMode::compat32 ?
		static_cast<std::uint32_t>(address) : address;
}

[[nodiscard]] constexpr bool misaligned(std::uint64_t address,
	std::size_t bytes) noexcept
{
	return bytes > 1 && bytes <= 8 && (address & (bytes - 1U)) != 0;
}

/* The count register a repeated string instruction reads, at its address
 * size. */
[[nodiscard]] std::uint64_t repeat_count(const CpuState &cpu,
	const Operand &operand) noexcept
{
	const auto count = cpu.gpr(Register::rcx);
	if (operand.memory.address_16)
		return count & 0xffffU;
	if (operand.memory.address_32)
		return count & 0xffffffffU;
	return count;
}

} // namespace

bool waits_for_x87(const Instruction &instruction) noexcept
{
	if (fwait(instruction) || uses_mmx(instruction))
		return true;
	if (instruction.operation != Operation::x87)
		return false;
	if (!x87_escape(instruction))
		return true;
	const auto modrm = instruction.vector_control;
	const auto reg = modrm_reg(modrm);
	switch (instruction.vector_opcode) {
	case 0xd9:
		/* FNSTENV and FNSTCW. */
		return modrm_register(modrm) || (reg != 6 && reg != 7);
	case 0xdb:
		/* FNENI, FNDISI, FNCLEX, FNINIT and FNSETPM. */
		return !(modrm == 0xe0 || modrm == 0xe1 || modrm == 0xe2 ||
			modrm == 0xe3 || modrm == 0xe4);
	case 0xdd:
		/* FNSAVE and FNSTSW. */
		return modrm_register(modrm) || (reg != 6 && reg != 7);
	case 0xdf:
		/* FNSTSW AX. */
		return modrm != 0xe0;
	default:
		return true;
	}
}

bool x87_loads_state(const Instruction &instruction) noexcept
{
	if (!x87_escape(instruction) ||
		modrm_register(instruction.vector_control))
		return false;
	const auto reg = modrm_reg(instruction.vector_control);
	return (instruction.vector_opcode == 0xd9 && (reg == 4 || reg == 5)) ||
		(instruction.vector_opcode == 0xdd && reg == 4);
}

bool x87_computes(const Instruction &instruction) noexcept
{
	return x87_escape(instruction) && waits_for_x87(instruction) &&
		!x87_loads_state(instruction);
}

std::size_t x87_store_bytes(const Instruction &instruction) noexcept
{
	if (!x87_escape(instruction) ||
		modrm_register(instruction.vector_control))
		return 0;
	const auto reg = modrm_reg(instruction.vector_control);
	switch (instruction.vector_opcode) {
	case 0xd9:
		return reg == 2 || reg == 3 ? 4U : 0U;
	case 0xdb:
		return reg == 7 ? 10U : reg >= 1 && reg <= 3 ? 4U : 0U;
	case 0xdd:
		return reg >= 1 && reg <= 3 ? 8U : 0U;
	case 0xdf:
		return reg == 6 ? 10U : reg == 7 ? 8U :
			reg >= 1 && reg <= 3 ? 2U : 0U;
	default:
		return 0;
	}
}

bool simd_floating(const Instruction &instruction) noexcept
{
	const auto opcode = instruction.vector_opcode;
	switch (instruction.operation) {
	case Operation::vector_convert:
	case Operation::vector_scalar_convert:
	case Operation::vector_compare_flags:
	case Operation::vector_fma:
		return true;
	case Operation::vector_alu:
		return opcode == 0x51 || opcode == 0x58 || opcode == 0x59 ||
			(opcode >= 0x5c && opcode <= 0x5f) || opcode == 0x7c ||
			opcode == 0x7d || opcode == 0xc2 || opcode == 0xd0;
	case Operation::vector_map2_misc:
		/* VCVTPH2PS. */
		return opcode == 0x13;
	case Operation::vector_integer_map3:
		/* ROUNDPS to ROUNDSD, DPPS, DPPD and VCVTPS2PH. */
		return (opcode >= 0x08 && opcode <= 0x0b) || opcode == 0x40 ||
			opcode == 0x41 || opcode == 0x1d;
	default:
		return false;
	}
}

bool needs_check(const Instruction &instruction, unsigned checked) noexcept
{
	if ((checked & check_x87) != 0 &&
		(instruction.operation == Operation::x87 ||
			fwait(instruction) || uses_mmx(instruction)))
		return true;
	if ((checked & check_simd) != 0 && simd_floating(instruction))
		return true;
	if ((checked & check_alignment) == 0)
		return false;
	switch (instruction.operation) {
	case Operation::nop:
	case Operation::lea:
		return false;
	case Operation::push:
	case Operation::pop:
	case Operation::push_all:
	case Operation::pop_all:
	case Operation::push_flags:
	case Operation::pop_flags:
	case Operation::leave:
	case Operation::enter:
	case Operation::call:
	case Operation::call_indirect:
	case Operation::ret:
	case Operation::interrupt_ret:
	case Operation::segment_stack:
		return true;
	default:
		return instruction.destination.kind == OperandKind::memory ||
			instruction.source.kind == OperandKind::memory ||
			instruction.auxiliary.kind == OperandKind::memory;
	}
}

std::uint64_t effective_address(const CpuState &cpu, const Gadget &gadget,
	const Operand &operand) noexcept
{
	const auto &memory = operand.memory;
	std::uint64_t address{};
	if (memory.absolute_address) {
		address = memory.absolute;
	} else if (memory.rip_relative) {
		address = gadget.address + gadget.instruction.length +
			static_cast<std::uint64_t>(
				static_cast<std::int64_t>(memory.displacement));
	} else {
		address = static_cast<std::uint64_t>(
			static_cast<std::int64_t>(memory.displacement));
		if (memory.base != MemoryOperand::no_register)
			address += cpu.registers[memory.base];
		if (memory.index != MemoryOperand::no_register &&
			!memory.vector_index)
			address += cpu.registers[memory.index] * memory.scale;
	}
	if (memory.address_16)
		address &= 0xffffU;
	else if (memory.address_32)
		address &= 0xffffffffU;
	switch (memory.segment) {
	case Segment::none:
		return address;
	case Segment::fs:
		address += cpu.fs_base;
		break;
	case Segment::gs:
		address += cpu.gs_base;
		break;
	}
	return cpu.execution_mode == ExecutionMode::compat32 ?
		static_cast<std::uint32_t>(address) : address;
}

bool misaligned_access(const CpuState &cpu, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto rsp = cpu.gpr(Register::rsp);
	const auto operand_bytes = [](const Operand &operand) noexcept {
		return static_cast<std::size_t>(operand.width);
	};
	switch (instruction.operation) {
	case Operation::nop:
	case Operation::lea:
	case Operation::extended_state:
	case Operation::vector_gather:
	case Operation::cache_flush:
	case Operation::cache_zero:
	case Operation::far_call:
	case Operation::far_jump:
		return false;
	case Operation::push:
	case Operation::push_flags:
	case Operation::segment_stack: {
		const bool pushing = instruction.operation != Operation::segment_stack ||
			instruction.vector_control != 0;
		const auto &stack = pushing ? instruction.source :
			instruction.destination;
		const auto bytes = operand_bytes(stack);
		if (instruction.source.kind == OperandKind::memory &&
			misaligned(effective_address(cpu, gadget, instruction.source),
				bytes))
			return true;
		return misaligned(stack_address(cpu, pushing ? rsp - bytes : rsp),
			bytes);
	}
	case Operation::pop:
	case Operation::pop_flags:
	case Operation::pop_all: {
		const auto bytes = operand_bytes(instruction.destination);
		if (misaligned(stack_address(cpu, rsp), bytes))
			return true;
		return instruction.destination.kind == OperandKind::memory &&
			misaligned(effective_address(cpu, gadget,
				instruction.destination), bytes);
	}
	case Operation::push_all: {
		const auto bytes = operand_bytes(instruction.destination);
		return misaligned(stack_address(cpu, rsp - bytes), bytes);
	}
	case Operation::call:
	case Operation::call_indirect:
	case Operation::enter: {
		if (instruction.operation == Operation::call_indirect &&
			instruction.source.kind == OperandKind::memory &&
			misaligned(effective_address(cpu, gadget, instruction.source),
				operand_bytes(instruction.source)))
			return true;
		const auto bytes = stack_bytes(cpu);
		return misaligned(stack_address(cpu, rsp - bytes), bytes);
	}
	case Operation::ret:
	case Operation::interrupt_ret:
		return misaligned(stack_address(cpu, rsp), stack_bytes(cpu));
	case Operation::leave:
		return misaligned(stack_address(cpu, cpu.gpr(Register::rbp)),
			stack_bytes(cpu));
	case Operation::string_move:
	case Operation::string_compare:
	case Operation::string_store:
	case Operation::string_load:
	case Operation::string_scan: {
		const Operand *first = instruction.source.kind ==
			OperandKind::memory ? &instruction.source :
			&instruction.destination;
		if (instruction.repeat != 0 &&
			repeat_count(cpu, *first) == 0)
			return false;
		for (const auto *operand :
			{&instruction.source, &instruction.destination}) {
			if (operand->kind == OperandKind::memory &&
				misaligned(effective_address(cpu, gadget, *operand),
					operand_bytes(*operand)))
				return true;
		}
		return false;
	}
	case Operation::x87:
		return instruction.source.kind == OperandKind::memory &&
			misaligned(effective_address(cpu, gadget, instruction.source),
				x87_memory_alignment(instruction));
	default:
		break;
	}
	for (const auto *operand : {&instruction.destination,
		&instruction.source, &instruction.auxiliary}) {
		if (operand->kind == OperandKind::memory &&
			!operand->memory.vector_index &&
			misaligned(effective_address(cpu, gadget, *operand),
				operand_bytes(*operand)))
			return true;
	}
	return false;
}

void clear_host_floating_exceptions() noexcept
{
	__asm__ volatile("msr fpsr, xzr" ::: "memory");
}

std::uint32_t host_floating_exceptions() noexcept
{
	std::uint64_t status{};
	__asm__ volatile("mrs %0, fpsr" : "=r"(status) :: "memory");
	/* FPSR's IOC, DZC, OFC, UFC, IXC and IDC, as MXCSR's IE, ZE, OE, UE,
	 * PE and DE. */
	std::uint32_t flags{};
	if ((status & 0x01U) != 0)
		flags |= 0x01U;
	if ((status & 0x02U) != 0)
		flags |= 0x04U;
	if ((status & 0x04U) != 0)
		flags |= 0x08U;
	if ((status & 0x08U) != 0)
		flags |= 0x10U;
	if ((status & 0x10U) != 0)
		flags |= 0x20U;
	if ((status & 0x80U) != 0)
		flags |= 0x02U;
	return flags;
}

} // namespace uml::cpu::x86_64::gadgets
