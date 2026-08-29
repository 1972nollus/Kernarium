// SPDX-License-Identifier: GPL-2.0-or-later
#include "program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace uml::cpu::x86_64::gadgets {
namespace {

[[nodiscard]] constexpr std::uint8_t width_bytes(OperandWidth width) noexcept
{
	return static_cast<std::uint8_t>(width);
}

[[nodiscard]] constexpr std::uint64_t width_mask(
	OperandWidth width) noexcept
{
	return width == OperandWidth::qword ? ~std::uint64_t{0} :
		(std::uint64_t{1} << (width_bytes(width) * 8U)) - 1U;
}

[[nodiscard]] std::uint64_t read_register(
	const CpuState &cpu, const Operand &operand) noexcept
{
	const auto value = cpu.registers[operand.reg.index];
	if (operand.width == OperandWidth::byte && operand.reg.high_byte)
		return (value >> 8U) & 0xffU;
	return value & width_mask(operand.width);
}

void write_register(CpuState &cpu, const Operand &operand,
	std::uint64_t value) noexcept
{
	auto &destination = cpu.registers[operand.reg.index];
	switch (operand.width) {
	case OperandWidth::byte:
		if (operand.reg.high_byte)
			destination = (destination & ~0xff00ULL) |
				((value & 0xffU) << 8U);
		else
			destination = (destination & ~0xffULL) | (value & 0xffU);
		break;
	case OperandWidth::word:
		destination = (destination & ~0xffffULL) | (value & 0xffffU);
		break;
	case OperandWidth::dword:
		destination = static_cast<std::uint32_t>(value);
		break;
	case OperandWidth::qword:
		destination = value;
		break;
	}
}

[[nodiscard]] std::uint64_t effective_address(const Gadget &gadget,
	const CpuState &cpu, const MemoryOperand &memory,
	bool apply_segment) noexcept
{
	std::uint64_t address = static_cast<std::uint64_t>(
		static_cast<std::int64_t>(memory.displacement));
	if (memory.rip_relative)
		address += gadget.address + gadget.instruction.length;
	if (memory.base != MemoryOperand::no_register)
		address += cpu.registers[memory.base];
	if (memory.index != MemoryOperand::no_register)
		address += cpu.registers[memory.index] * memory.scale;
	if (apply_segment) {
		if (memory.segment == Segment::fs)
			address += cpu.fs_base;
		else if (memory.segment == Segment::gs)
			address += cpu.gs_base;
	}
	return address;
}

[[nodiscard]] ::uml::cpu::FaultInfo memory_fault(
	::uml::memory::GuestAddressSpace &memory, std::uint64_t fallback,
	::uml::memory::MemoryAccess access) noexcept
{
	if (memory.has_fault()) {
		const auto fault = memory.last_fault();
		return {fault.address, fault.access};
	}
	return {fallback, access};
}

[[nodiscard]] bool read_memory(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, OperandWidth width, std::uint64_t &value) noexcept
{
	std::array<std::byte, 8> bytes{};
	try {
		if (!memory.read(address,
			std::span{bytes}.first(width_bytes(width))))
			return false;
	} catch (...) {
		return false;
	}
	std::memcpy(&value, bytes.data(), width_bytes(width));
	return true;
}

[[nodiscard]] bool write_memory(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, OperandWidth width, std::uint64_t value) noexcept
{
	std::array<std::byte, 8> bytes{};
	std::memcpy(bytes.data(), &value, width_bytes(width));
	try {
		return memory.write(address,
			std::span<const std::byte>{bytes}.first(width_bytes(width)));
	} catch (...) {
		return false;
	}
}

struct OperandRead final {
	bool succeeded{};
	std::uint64_t value{};
	::uml::cpu::FaultInfo fault{};
};

[[nodiscard]] OperandRead read_operand(const Gadget &gadget,
	const CpuState &cpu, ::uml::memory::GuestAddressSpace &memory,
	const Operand &operand) noexcept
{
	switch (operand.kind) {
	case OperandKind::reg:
		return {true, read_register(cpu, operand), {}};
	case OperandKind::immediate:
		return {true, operand.immediate, {}};
	case OperandKind::memory: {
		const auto address = effective_address(
			gadget, cpu, operand.memory, true);
		std::uint64_t value{};
		if (!read_memory(memory, address, operand.width, value))
			return {false, 0, memory_fault(memory, address,
				::uml::memory::MemoryAccess::read)};
		return {true, value, {}};
	}
	case OperandKind::none:
		break;
	}
	return {};
}

[[nodiscard]] StepResult write_operand(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, const Operand &operand,
	std::uint64_t value) noexcept
{
	if (operand.kind == OperandKind::reg) {
		write_register(cpu, operand, value);
		return {};
	}
	if (operand.kind == OperandKind::memory) {
		const auto address = effective_address(
			gadget, cpu, operand.memory, true);
		if (write_memory(memory, address, operand.width, value))
			return {};
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, address, ::uml::memory::MemoryAccess::write)};
	}
	return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
}

void advance(const Gadget &gadget, CpuState &cpu) noexcept
{
	cpu.rip = gadget.address + gadget.instruction.length;
}

[[nodiscard]] StepResult read_failure(const OperandRead &read) noexcept
{
	return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
		read.fault};
}

[[nodiscard]] StepResult move(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto source = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!source.succeeded)
		return read_failure(source);
	const auto written = write_operand(gadget, cpu, memory,
		gadget.instruction.destination, source.value);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult nop(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult lea(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	const auto value = effective_address(
		gadget, cpu, gadget.instruction.source.memory, false);
	write_register(cpu, gadget.instruction.destination, value);
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult arithmetic(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto left = read_operand(
		gadget, cpu, memory, gadget.instruction.destination);
	if (!left.succeeded)
		return read_failure(left);
	const auto right = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!right.succeeded)
		return read_failure(right);

	std::uint64_t result{};
	DeferredFlagOperation flags{};
	switch (gadget.instruction.operation) {
	case Operation::add:
		result = left.value + right.value;
		flags = DeferredFlagOperation::add;
		break;
	case Operation::subtract:
	case Operation::compare:
		result = left.value - right.value;
		flags = DeferredFlagOperation::subtract;
		break;
	case Operation::bit_and:
	case Operation::test:
		result = left.value & right.value;
		flags = DeferredFlagOperation::logical;
		break;
	case Operation::bit_or:
		result = left.value | right.value;
		flags = DeferredFlagOperation::logical;
		break;
	case Operation::bit_xor:
		result = left.value ^ right.value;
		flags = DeferredFlagOperation::logical;
		break;
	default:
		return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
	}

	if (gadget.instruction.operation != Operation::compare &&
		gadget.instruction.operation != Operation::test) {
		const auto written = write_operand(gadget, cpu, memory,
			gadget.instruction.destination, result);
		if (written.action == StepAction::exit)
			return written;
	}
	cpu.deferred_flags = {
		left.value, right.value, result, flags,
		width_bytes(gadget.instruction.destination.width)};
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] bool condition_holds(CpuState &cpu,
	Condition condition) noexcept
{
	materialize_flags(cpu);
	const bool overflow = (cpu.rflags & rflags_overflow) != 0;
	const bool carry = (cpu.rflags & rflags_carry) != 0;
	const bool zero = (cpu.rflags & rflags_zero) != 0;
	const bool sign = (cpu.rflags & rflags_sign) != 0;
	const bool parity = (cpu.rflags & rflags_parity) != 0;
	switch (condition) {
	case Condition::overflow: return overflow;
	case Condition::not_overflow: return !overflow;
	case Condition::below: return carry;
	case Condition::above_or_equal: return !carry;
	case Condition::equal: return zero;
	case Condition::not_equal: return !zero;
	case Condition::below_or_equal: return carry || zero;
	case Condition::above: return !carry && !zero;
	case Condition::sign: return sign;
	case Condition::not_sign: return !sign;
	case Condition::parity: return parity;
	case Condition::not_parity: return !parity;
	case Condition::less: return sign != overflow;
	case Condition::greater_or_equal: return sign == overflow;
	case Condition::less_or_equal: return zero || sign != overflow;
	case Condition::greater: return !zero && sign == overflow;
	}
	return false;
}

[[nodiscard]] std::uint64_t relative_target(const Gadget &gadget) noexcept
{
	return gadget.address + gadget.instruction.length +
		gadget.instruction.source.immediate;
}

[[nodiscard]] StepResult jump(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	cpu.rip = relative_target(gadget);
	return {StepAction::return_to_dispatch,
		::uml::cpu::EngineEvent::continued, {}};
}

[[nodiscard]] StepResult jump_conditional(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	cpu.rip = condition_holds(cpu, gadget.instruction.condition) ?
		relative_target(gadget) : gadget.address + gadget.instruction.length;
	return {StepAction::return_to_dispatch,
		::uml::cpu::EngineEvent::continued, {}};
}

[[nodiscard]] StepResult push(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto source = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!source.succeeded)
		return read_failure(source);
	const auto size = width_bytes(gadget.instruction.source.width);
	const auto address = cpu.gpr(Register::rsp) - size;
	if (!write_memory(memory, address,
		gadget.instruction.source.width, source.value))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, address, ::uml::memory::MemoryAccess::write)};
	cpu.gpr(Register::rsp) = address;
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult pop(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto width = gadget.instruction.destination.width;
	const auto address = cpu.gpr(Register::rsp);
	std::uint64_t value{};
	if (!read_memory(memory, address, width, value))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, address, ::uml::memory::MemoryAccess::read)};
	cpu.gpr(Register::rsp) += width_bytes(width);
	write_register(cpu, gadget.instruction.destination, value);
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult call(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto return_address = gadget.address + gadget.instruction.length;
	const auto stack_address = cpu.gpr(Register::rsp) - 8U;
	if (!write_memory(memory, stack_address, OperandWidth::qword,
		return_address))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, stack_address,
				::uml::memory::MemoryAccess::write)};
	cpu.gpr(Register::rsp) = stack_address;
	cpu.rip = relative_target(gadget);
	return {StepAction::return_to_dispatch,
		::uml::cpu::EngineEvent::continued, {}};
}

[[nodiscard]] StepResult ret(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto stack_address = cpu.gpr(Register::rsp);
	std::uint64_t target{};
	if (!read_memory(memory, stack_address, OperandWidth::qword, target))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, stack_address,
				::uml::memory::MemoryAccess::read)};
	const auto adjustment = gadget.instruction.source.kind ==
		OperandKind::immediate ? gadget.instruction.source.immediate : 0;
	cpu.gpr(Register::rsp) = stack_address + 8U + adjustment;
	cpu.rip = target;
	return {StepAction::return_to_dispatch,
		::uml::cpu::EngineEvent::continued, {}};
}

[[nodiscard]] StepResult system_call(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	materialize_flags(cpu);
	const auto next = gadget.address + gadget.instruction.length;
	cpu.gpr(Register::rcx) = next;
	cpu.gpr(Register::r11) = cpu.rflags;
	cpu.rip = next;
	return {StepAction::exit, ::uml::cpu::EngineEvent::system_call, {}};
}

[[nodiscard]] StepResult breakpoint(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	advance(gadget, cpu);
	return {StepAction::exit, ::uml::cpu::EngineEvent::breakpoint, {}};
}

[[nodiscard]] GadgetEntry entry_for(Operation operation) noexcept
{
	switch (operation) {
	case Operation::nop: return nop;
	case Operation::move: return move;
	case Operation::lea: return lea;
	case Operation::add:
	case Operation::subtract:
	case Operation::bit_and:
	case Operation::bit_or:
	case Operation::bit_xor:
	case Operation::compare:
	case Operation::test:
		return arithmetic;
	case Operation::push: return push;
	case Operation::pop: return pop;
	case Operation::jump: return jump;
	case Operation::jump_conditional: return jump_conditional;
	case Operation::call: return call;
	case Operation::ret: return ret;
	case Operation::system_call: return system_call;
	case Operation::breakpoint: return breakpoint;
	}
	return nullptr;
}

} // namespace

Program::Program(std::uint64_t address, std::size_t byte_count,
	std::vector<Gadget> gadgets) noexcept
	: address_{address}, byte_count_{byte_count}, gadgets_{std::move(gadgets)}
{}

BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, std::size_t maximum_instructions) noexcept
{
	try {
		std::vector<Gadget> stream;
		stream.reserve(maximum_instructions);
		auto current = address;
		std::size_t bytes{};
		for (std::size_t index = 0; index < maximum_instructions; ++index) {
			const auto decoded = decode(memory, current);
			if (decoded.status != DecodeStatus::decoded) {
				/* Preserve the side effects and retirement order of the valid
				 * prefix. Dispatch will decode the faulting instruction again
				 * after that prefix has executed.
				 */
				if (!stream.empty())
					break;
				if (decoded.status == DecodeStatus::fetch_fault)
					return {BuildStatus::fetch_fault, {},
						decoded.fault_address};
				return {BuildStatus::illegal_instruction, {}, current};
			}
			const auto entry = entry_for(decoded.instruction.operation);
			if (entry == nullptr)
				return {BuildStatus::illegal_instruction, {}, current};
			stream.push_back({entry, current, decoded.instruction});
			current += decoded.instruction.length;
			bytes += decoded.instruction.length;
			if (decoded.instruction.terminal())
				break;
		}
		if (stream.empty())
			return {BuildStatus::illegal_instruction, {}, address};
		return {BuildStatus::built,
			Program{address, bytes, std::move(stream)}, 0};
	} catch (...) {
		return {BuildStatus::allocation_failure, {}, 0};
	}
}

ExecuteResult execute(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt) noexcept
{
	ExecuteResult result{::uml::cpu::EngineEvent::continued, 0, {}, false};
	if (program.stream().empty() || cpu.rip != program.address()) {
		result.event = ::uml::cpu::EngineEvent::fatal;
		return result;
	}
	for (const auto &gadget : program.stream()) {
		if (result.retired_instructions == instruction_budget) {
			result.instruction_budget_exhausted = true;
			return result;
		}
		if (interrupt.pending()) {
			result.event = ::uml::cpu::EngineEvent::interrupt;
			return result;
		}
		if (cpu.rip != gadget.address) {
			result.event = ::uml::cpu::EngineEvent::fatal;
			return result;
		}
		const auto step = gadget.entry(gadget, cpu, memory);
		if (step.action == StepAction::exit) {
			result.event = step.event;
			result.fault = step.fault;
			if (step.event != ::uml::cpu::EngineEvent::memory_fault &&
				step.event != ::uml::cpu::EngineEvent::fatal)
				++result.retired_instructions;
			return result;
		}
		++result.retired_instructions;
		if (step.action == StepAction::return_to_dispatch)
			return result;
	}
	return result;
}

} // namespace uml::cpu::x86_64::gadgets
