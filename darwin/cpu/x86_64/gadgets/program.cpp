// SPDX-License-Identifier: GPL-2.0-or-later
#include "program.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
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

[[nodiscard]] constexpr unsigned width_bits(OperandWidth width) noexcept
{
	return width_bytes(width) * 8U;
}

[[nodiscard]] constexpr bool even_parity(std::uint8_t value) noexcept
{
	value ^= static_cast<std::uint8_t>(value >> 4U);
	value &= 0xfU;
	return ((0x9669U >> value) & 1U) != 0;
}

[[nodiscard]] constexpr std::uint64_t sign_extend_width(
	std::uint64_t value, OperandWidth width) noexcept
{
	const auto bits = width_bits(width);
	if (bits == 64)
		return value;
	const auto sign = std::uint64_t{1} << (bits - 1U);
	return ((value & width_mask(width)) ^ sign) - sign;
}

[[nodiscard]] constexpr __int128 signed_value(
	std::uint64_t value, OperandWidth width) noexcept
{
	return static_cast<__int128>(
		static_cast<std::int64_t>(sign_extend_width(value, width)));
}

void set_szp_flags(CpuState &cpu, std::uint64_t result,
	OperandWidth width) noexcept
{
	constexpr auto mask = rflags_parity | rflags_zero | rflags_sign;
	cpu.rflags &= ~mask;
	result &= width_mask(width);
	if (result == 0)
		cpu.rflags |= rflags_zero;
	if ((result & (std::uint64_t{1} << (width_bits(width) - 1U))) != 0)
		cpu.rflags |= rflags_sign;
	if (even_parity(static_cast<std::uint8_t>(result)))
		cpu.rflags |= rflags_parity;
	cpu.rflags |= rflags_reserved;
	cpu.deferred_flags.operation = DeferredFlagOperation::none;
}

void set_arithmetic_flags(CpuState &cpu, std::uint64_t left,
	std::uint64_t right, std::uint64_t result, OperandWidth width,
	bool subtract, std::uint64_t carry) noexcept
{
	const auto mask = width_mask(width);
	left &= mask;
	right &= mask;
	result &= mask;
	constexpr auto arithmetic = rflags_carry | rflags_parity |
		rflags_auxiliary | rflags_zero | rflags_sign | rflags_overflow;
	cpu.rflags &= ~arithmetic;
	set_szp_flags(cpu, result, width);

	const auto bits = width_bits(width);
	const auto signed_min = -(static_cast<__int128>(1) << (bits - 1U));
	const auto signed_max = (static_cast<__int128>(1) << (bits - 1U)) - 1;
	__int128 signed_result{};
	if (subtract) {
		const auto subtrahend = static_cast<unsigned __int128>(right) + carry;
		if (static_cast<unsigned __int128>(left) < subtrahend)
			cpu.rflags |= rflags_carry;
		signed_result = signed_value(left, width) -
			signed_value(right, width) - static_cast<__int128>(carry);
	} else {
		const auto full = static_cast<unsigned __int128>(left) + right + carry;
		if (full > mask)
			cpu.rflags |= rflags_carry;
		signed_result = signed_value(left, width) +
			signed_value(right, width) + static_cast<__int128>(carry);
	}
	if (signed_result < signed_min || signed_result > signed_max)
		cpu.rflags |= rflags_overflow;
	if (((left ^ right ^ result) & 0x10U) != 0)
		cpu.rflags |= rflags_auxiliary;
}

void set_multiply_flags(CpuState &cpu, bool overflow) noexcept
{
	materialize_flags(cpu);
	cpu.rflags &= ~(rflags_carry | rflags_overflow);
	if (overflow)
		cpu.rflags |= rflags_carry | rflags_overflow;
	cpu.rflags |= rflags_reserved;
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

void write_gpr(CpuState &cpu, std::uint8_t index, OperandWidth width,
	std::uint64_t value) noexcept
{
	Operand operand{};
	operand.kind = OperandKind::reg;
	operand.width = width;
	operand.reg.index = index;
	write_register(cpu, operand, value);
}

[[nodiscard]] std::uint64_t effective_address(const Gadget &gadget,
	const CpuState &cpu, const MemoryOperand &memory,
	bool apply_segment) noexcept
{
	std::uint64_t address = memory.absolute_address ? memory.absolute :
		static_cast<std::uint64_t>(
			static_cast<std::int64_t>(memory.displacement));
	if (!memory.absolute_address) {
		if (memory.rip_relative)
			address += gadget.address + gadget.instruction.length;
		if (memory.base != MemoryOperand::no_register)
			address += memory.address_32 ?
				static_cast<std::uint32_t>(cpu.registers[memory.base]) :
				cpu.registers[memory.base];
		if (memory.index != MemoryOperand::no_register)
			address += (memory.address_32 ?
				static_cast<std::uint32_t>(cpu.registers[memory.index]) :
				cpu.registers[memory.index]) * memory.scale;
		if (memory.address_32)
			address = static_cast<std::uint32_t>(address);
	}
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

[[nodiscard]] bool read_memory_bytes(
	::uml::memory::GuestAddressSpace &memory, std::uint64_t address,
	std::span<std::byte> bytes) noexcept
{
	try {
		return memory.read(address, bytes);
	} catch (...) {
		return false;
	}
}

[[nodiscard]] bool write_memory_bytes(
	::uml::memory::GuestAddressSpace &memory, std::uint64_t address,
	std::span<const std::byte> bytes) noexcept
{
	try {
		return memory.write(address, bytes);
	} catch (...) {
		return false;
	}
}

[[nodiscard]] std::uint64_t next_random() noexcept
{
	static std::atomic_uint64_t state{0x6a09e667f3bcc909ULL};
	auto observed = state.load(std::memory_order_relaxed);
	for (;;) {
		auto value = observed;
		value ^= value << 13U;
		value ^= value >> 7U;
		value ^= value << 17U;
		if (state.compare_exchange_weak(observed, value,
			std::memory_order_relaxed, std::memory_order_relaxed))
			return value;
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
			return {StepAction::continue_program,
				::uml::cpu::EngineEvent::continued, {}, address,
				width_bytes(operand.width)};
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
	return written;
}

[[nodiscard]] StepResult move_extended(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto source = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!source.succeeded)
		return read_failure(source);
	const auto value = gadget.instruction.operation ==
		Operation::move_sign_extend ?
		sign_extend_width(source.value, gadget.instruction.source.width) :
		source.value;
	const auto written = write_operand(gadget, cpu, memory,
		gadget.instruction.destination, value);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult exchange(const Gadget &gadget, CpuState &cpu,
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
	const auto left_written = write_operand(gadget, cpu, memory,
		gadget.instruction.destination, right.value);
	if (left_written.action == StepAction::exit)
		return left_written;
	const auto right_written = write_operand(gadget, cpu, memory,
		gadget.instruction.source, left.value);
	if (right_written.action == StepAction::exit)
		return right_written;
	advance(gadget, cpu);
	return left_written.write_size != 0 ? left_written : right_written;
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

	StepResult written{};
	if (gadget.instruction.operation != Operation::compare &&
		gadget.instruction.operation != Operation::test) {
		written = write_operand(gadget, cpu, memory,
			gadget.instruction.destination, result);
		if (written.action == StepAction::exit)
			return written;
	}
	cpu.deferred_flags = {
		left.value, right.value, result, flags,
		width_bytes(gadget.instruction.destination.width)};
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult arithmetic_with_carry(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto left = read_operand(
		gadget, cpu, memory, gadget.instruction.destination);
	if (!left.succeeded)
		return read_failure(left);
	const auto right = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!right.succeeded)
		return read_failure(right);
	materialize_flags(cpu);
	const auto carry = (cpu.rflags & rflags_carry) != 0 ? 1U : 0U;
	const bool subtract = gadget.instruction.operation ==
		Operation::subtract_borrow;
	const auto result = subtract ? left.value - right.value - carry :
		left.value + right.value + carry;
	const auto written = write_operand(gadget, cpu, memory,
		gadget.instruction.destination, result);
	if (written.action == StepAction::exit)
		return written;
	set_arithmetic_flags(cpu, left.value, right.value, result,
		gadget.instruction.destination.width, subtract, carry);
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult unary(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto operand = read_operand(
		gadget, cpu, memory, gadget.instruction.destination);
	if (!operand.succeeded)
		return read_failure(operand);
	const auto width = gadget.instruction.destination.width;
	materialize_flags(cpu);
	const auto old_carry = cpu.rflags & rflags_carry;
	std::uint64_t result{};
	switch (gadget.instruction.operation) {
	case Operation::increment:
		result = operand.value + 1U;
		break;
	case Operation::decrement:
		result = operand.value - 1U;
		break;
	case Operation::negate:
		result = 0 - operand.value;
		break;
	case Operation::bit_not:
		result = ~operand.value;
		break;
	default:
		return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
	}
	const auto written = write_operand(gadget, cpu, memory,
		gadget.instruction.destination, result);
	if (written.action == StepAction::exit)
		return written;
	if (gadget.instruction.operation == Operation::increment ||
		gadget.instruction.operation == Operation::decrement) {
		const bool decrement = gadget.instruction.operation ==
			Operation::decrement;
		set_arithmetic_flags(cpu, operand.value, 1, result, width,
			decrement, 0);
		cpu.rflags = (cpu.rflags & ~rflags_carry) | old_carry;
	} else if (gadget.instruction.operation == Operation::negate) {
		set_arithmetic_flags(cpu, 0, operand.value, result, width, true, 0);
	}
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult shift_rotate(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto operand = read_operand(
		gadget, cpu, memory, gadget.instruction.destination);
	if (!operand.succeeded)
		return read_failure(operand);
	const auto count_operand = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!count_operand.succeeded)
		return read_failure(count_operand);
	const auto width = gadget.instruction.destination.width;
	const auto bits = width_bits(width);
	auto count = static_cast<unsigned>(count_operand.value &
		(bits == 64 ? 0x3fU : 0x1fU));
	if (count == 0) {
		const auto written = write_operand(gadget, cpu, memory,
			gadget.instruction.destination, operand.value);
		if (written.action == StepAction::exit)
			return written;
		advance(gadget, cpu);
		return written;
	}
	materialize_flags(cpu);
	const auto original = operand.value & width_mask(width);
	auto result = original;
	bool carry = (cpu.rflags & rflags_carry) != 0;
	const bool rotate = gadget.instruction.operation == Operation::rotate_left ||
		gadget.instruction.operation == Operation::rotate_right ||
		gadget.instruction.operation == Operation::rotate_carry_left ||
		gadget.instruction.operation == Operation::rotate_carry_right;
	const bool through_carry = gadget.instruction.operation ==
		Operation::rotate_carry_left || gadget.instruction.operation ==
		Operation::rotate_carry_right;
	if (rotate) {
		count %= through_carry && bits != 64 ? bits + 1U : bits;
		if (count == 0) {
			const auto written = write_operand(gadget, cpu, memory,
				gadget.instruction.destination, result);
			if (written.action == StepAction::exit)
				return written;
			if (!through_carry) {
				const bool wrapped_carry =
					gadget.instruction.operation == Operation::rotate_left ?
					(result & 1U) != 0 :
					((result >> (bits - 1U)) & 1U) != 0;
				cpu.rflags = wrapped_carry ?
					cpu.rflags | rflags_carry : cpu.rflags & ~rflags_carry;
			}
			advance(gadget, cpu);
			return written;
		}
	}
	for (unsigned index = 0; index < count; ++index) {
		switch (gadget.instruction.operation) {
		case Operation::shift_left:
			carry = ((result >> (bits - 1U)) & 1U) != 0;
			result = (result << 1U) & width_mask(width);
			break;
		case Operation::shift_right:
			carry = (result & 1U) != 0;
			result >>= 1U;
			break;
		case Operation::shift_arithmetic_right:
			carry = (result & 1U) != 0;
			result = (result >> 1U) |
				(result & (std::uint64_t{1} << (bits - 1U)));
			break;
		case Operation::rotate_left:
			carry = ((result >> (bits - 1U)) & 1U) != 0;
			result = ((result << 1U) | (carry ? 1U : 0U)) &
				width_mask(width);
			break;
		case Operation::rotate_right:
			carry = (result & 1U) != 0;
			result = (result >> 1U) |
				((carry ? std::uint64_t{1} : 0U) << (bits - 1U));
			break;
		case Operation::rotate_carry_left: {
			const auto old_carry = carry;
			carry = ((result >> (bits - 1U)) & 1U) != 0;
			result = ((result << 1U) | (old_carry ? 1U : 0U)) &
				width_mask(width);
			break;
		}
		case Operation::rotate_carry_right: {
			const auto old_carry = carry;
			carry = (result & 1U) != 0;
			result = (result >> 1U) |
				((old_carry ? std::uint64_t{1} : 0U) << (bits - 1U));
			break;
		}
		default:
			return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
		}
	}
	const auto written = write_operand(gadget, cpu, memory,
		gadget.instruction.destination, result);
	if (written.action == StepAction::exit)
		return written;
	cpu.rflags = carry ? cpu.rflags | rflags_carry :
		cpu.rflags & ~rflags_carry;
	if (!rotate)
		set_szp_flags(cpu, result, width);
	if (count == 1) {
		bool overflow{};
		if (gadget.instruction.operation == Operation::shift_left ||
			gadget.instruction.operation == Operation::rotate_left ||
			gadget.instruction.operation == Operation::rotate_carry_left) {
			overflow = ((result >> (bits - 1U)) & 1U) != carry;
		} else if (gadget.instruction.operation == Operation::shift_right) {
			overflow = ((original >> (bits - 1U)) & 1U) != 0;
		} else if (gadget.instruction.operation == Operation::rotate_right ||
			gadget.instruction.operation == Operation::rotate_carry_right) {
			overflow = ((result >> (bits - 1U)) & 1U) !=
				((result >> (bits - 2U)) & 1U);
		}
		cpu.rflags = overflow ? cpu.rflags | rflags_overflow :
			cpu.rflags & ~rflags_overflow;
	}
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult double_shift(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto destination = read_operand(
		gadget, cpu, memory, gadget.instruction.destination);
	if (!destination.succeeded)
		return read_failure(destination);
	const auto source = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!source.succeeded)
		return read_failure(source);
	const auto count_read = read_operand(
		gadget, cpu, memory, gadget.instruction.auxiliary);
	if (!count_read.succeeded)
		return read_failure(count_read);
	const auto width = gadget.instruction.destination.width;
	const auto bits = width_bits(width);
	const auto count = static_cast<unsigned>(count_read.value &
		(bits == 64 ? 0x3fU : 0x1fU));
	if (count == 0) {
		StepResult written{};
		if (gadget.instruction.destination.kind == OperandKind::reg) {
			written = write_operand(gadget, cpu, memory,
				gadget.instruction.destination, destination.value);
			if (written.action == StepAction::exit)
				return written;
		}
		advance(gadget, cpu);
		return written;
	}

	const auto mask = width_mask(width);
	const auto left = static_cast<unsigned __int128>(destination.value & mask);
	const auto right = static_cast<unsigned __int128>(source.value & mask);
	unsigned __int128 combined{};
	std::uint64_t result{};
	bool carry{};
	if (gadget.instruction.operation == Operation::double_shift_left) {
		combined = (left << bits) | right;
		result = static_cast<std::uint64_t>(
			(combined << count) >> bits) & mask;
		carry = ((combined >> (bits * 2U - count)) & 1U) != 0;
	} else {
		combined = (right << bits) | left;
		result = static_cast<std::uint64_t>(combined >> count) & mask;
		carry = ((combined >> (count - 1U)) & 1U) != 0;
	}
	const auto written = write_operand(gadget, cpu, memory,
		gadget.instruction.destination, result);
	if (written.action == StepAction::exit)
		return written;
	materialize_flags(cpu);
	set_szp_flags(cpu, result, width);
	cpu.rflags = carry ? cpu.rflags | rflags_carry :
		cpu.rflags & ~rflags_carry;
	if (count == 1) {
		const bool overflow = gadget.instruction.operation ==
			Operation::double_shift_left ?
			(((result >> (bits - 1U)) & 1U) != carry) :
			(((destination.value >> (bits - 1U)) & 1U) !=
				((result >> (bits - 1U)) & 1U));
		cpu.rflags = overflow ? cpu.rflags | rflags_overflow :
			cpu.rflags & ~rflags_overflow;
	}
	advance(gadget, cpu);
	return written;
}

void write_multiply_pair(CpuState &cpu, OperandWidth width,
	unsigned __int128 product) noexcept
{
	if (width == OperandWidth::byte) {
		write_gpr(cpu, 0, OperandWidth::word,
			static_cast<std::uint16_t>(product));
		return;
	}
	const auto bits = width_bits(width);
	write_gpr(cpu, 0, width, static_cast<std::uint64_t>(product));
	write_gpr(cpu, 2, width,
		static_cast<std::uint64_t>(product >> bits));
}

[[nodiscard]] StepResult multiply(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto width = gadget.instruction.destination.width;
	const bool implicit = gadget.instruction.source.kind == OperandKind::none;
	const bool three_operand = gadget.instruction.auxiliary.kind !=
		OperandKind::none;
	std::uint64_t left{};
	std::uint64_t right{};
	if (implicit) {
		const auto operand = read_operand(
			gadget, cpu, memory, gadget.instruction.destination);
		if (!operand.succeeded)
			return read_failure(operand);
		left = width == OperandWidth::byte ?
			cpu.gpr(Register::rax) & 0xffU :
			cpu.gpr(Register::rax) & width_mask(width);
		right = operand.value;
	} else {
		const auto first = read_operand(gadget, cpu, memory,
			three_operand ? gadget.instruction.source :
				gadget.instruction.destination);
		if (!first.succeeded)
			return read_failure(first);
		const auto second = read_operand(gadget, cpu, memory,
			three_operand ? gadget.instruction.auxiliary :
				gadget.instruction.source);
		if (!second.succeeded)
			return read_failure(second);
		left = first.value;
		right = second.value;
	}

	bool overflow{};
	std::uint64_t low{};
	unsigned __int128 raw_product{};
	if (gadget.instruction.operation == Operation::multiply) {
		raw_product = static_cast<unsigned __int128>(
			left & width_mask(width)) * (right & width_mask(width));
		low = static_cast<std::uint64_t>(raw_product) & width_mask(width);
		overflow = (raw_product >> width_bits(width)) != 0;
	} else {
		const auto signed_product = signed_value(left, width) *
			signed_value(right, width);
		raw_product = static_cast<unsigned __int128>(signed_product);
		low = static_cast<std::uint64_t>(raw_product) & width_mask(width);
		overflow = signed_product != signed_value(low, width);
	}

	if (implicit) {
		write_multiply_pair(cpu, width, raw_product);
	} else {
		const auto written = write_operand(gadget, cpu, memory,
			gadget.instruction.destination, low);
		if (written.action == StepAction::exit)
			return written;
	}
	set_multiply_flags(cpu, overflow);
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] constexpr unsigned __int128 mask_128(unsigned bits) noexcept
{
	return bits == 128 ? ~static_cast<unsigned __int128>(0) :
		(static_cast<unsigned __int128>(1) << bits) - 1U;
}

[[nodiscard]] constexpr __int128 sign_extend_128(
	unsigned __int128 value, unsigned bits) noexcept
{
	if (bits == 128)
		return static_cast<__int128>(value);
	const auto sign = static_cast<unsigned __int128>(1) << (bits - 1U);
	return static_cast<__int128>((value & mask_128(bits)) ^ sign) -
		static_cast<__int128>(sign);
}

[[nodiscard]] StepResult divide(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto divisor_read = read_operand(
		gadget, cpu, memory, gadget.instruction.destination);
	if (!divisor_read.succeeded)
		return read_failure(divisor_read);
	const auto width = gadget.instruction.destination.width;
	const auto bits = width_bits(width);
	const auto divisor = divisor_read.value & width_mask(width);
	unsigned __int128 dividend{};
	if (width == OperandWidth::byte) {
		dividend = cpu.gpr(Register::rax) & 0xffffU;
	} else {
		dividend = (static_cast<unsigned __int128>(
			cpu.gpr(Register::rdx) & width_mask(width)) << bits) |
			(cpu.gpr(Register::rax) & width_mask(width));
	}
	std::uint64_t quotient{};
	std::uint64_t remainder{};
	if (gadget.instruction.operation == Operation::divide) {
		if (divisor == 0)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::floating_point_trap, {}};
		const auto full_quotient = dividend / divisor;
		if (full_quotient > width_mask(width))
			return {StepAction::exit,
				::uml::cpu::EngineEvent::floating_point_trap, {}};
		quotient = static_cast<std::uint64_t>(full_quotient);
		remainder = static_cast<std::uint64_t>(dividend % divisor);
	} else {
		const auto signed_divisor = signed_value(divisor, width);
		if (signed_divisor == 0)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::floating_point_trap, {}};
		const auto signed_dividend = sign_extend_128(dividend, bits * 2U);
		if (bits == 64 &&
			dividend == (static_cast<unsigned __int128>(1) << 127U) &&
			signed_divisor == -1)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::floating_point_trap, {}};
		const auto minimum = -(static_cast<__int128>(1) << (bits - 1U));
		const auto maximum = (static_cast<__int128>(1) << (bits - 1U)) - 1;
		const auto full_quotient = signed_dividend / signed_divisor;
		if (full_quotient < minimum || full_quotient > maximum)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::floating_point_trap, {}};
		quotient = static_cast<std::uint64_t>(full_quotient) &
			width_mask(width);
		remainder = static_cast<std::uint64_t>(
			signed_dividend % signed_divisor) & width_mask(width);
	}
	if (width == OperandWidth::byte) {
		const auto combined = (remainder << 8U) | quotient;
		write_gpr(cpu, 0, OperandWidth::word, combined);
	} else {
		write_gpr(cpu, 0, width, quotient);
		write_gpr(cpu, 2, width, remainder);
	}
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult bit_operation(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto index_read = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!index_read.succeeded)
		return read_failure(index_read);
	const auto width = gadget.instruction.destination.width;
	const auto bits = width_bits(width);
	std::uint64_t value{};
	std::uint64_t address{};
	unsigned bit{};
	if (gadget.instruction.destination.kind == OperandKind::memory) {
		address = effective_address(gadget, cpu,
			gadget.instruction.destination.memory, true);
		if (gadget.instruction.source.kind == OperandKind::immediate) {
			bit = static_cast<unsigned>(index_read.value & (bits - 1U));
		} else {
			const auto index = static_cast<std::int64_t>(
				sign_extend_width(index_read.value, width));
			auto element = index / static_cast<std::int64_t>(bits);
			auto remainder = index % static_cast<std::int64_t>(bits);
			if (remainder < 0) {
				remainder += bits;
				--element;
			}
			address += static_cast<std::uint64_t>(element *
				static_cast<std::int64_t>(width_bytes(width)));
			bit = static_cast<unsigned>(remainder);
		}
		if (!read_memory(memory, address, width, value))
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault,
				memory_fault(memory, address,
					::uml::memory::MemoryAccess::read)};
	} else {
		const auto destination = read_operand(
			gadget, cpu, memory, gadget.instruction.destination);
		if (!destination.succeeded)
			return read_failure(destination);
		value = destination.value;
		bit = static_cast<unsigned>(index_read.value & (bits - 1U));
	}

	const auto bit_mask = std::uint64_t{1} << bit;
	const bool carry = (value & bit_mask) != 0;
	std::uint64_t result = value;
	switch (gadget.instruction.operation) {
	case Operation::bit_test: break;
	case Operation::bit_set: result |= bit_mask; break;
	case Operation::bit_reset: result &= ~bit_mask; break;
	case Operation::bit_complement: result ^= bit_mask; break;
	default:
		return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
	}
	StepResult written{};
	if (gadget.instruction.operation != Operation::bit_test) {
		if (gadget.instruction.destination.kind == OperandKind::memory) {
			if (!write_memory(memory, address, width, result))
				return {StepAction::exit,
					::uml::cpu::EngineEvent::memory_fault,
					memory_fault(memory, address,
						::uml::memory::MemoryAccess::write)};
			written = {StepAction::continue_program,
				::uml::cpu::EngineEvent::continued, {}, address,
				width_bytes(width)};
		} else {
			written = write_operand(gadget, cpu, memory,
				gadget.instruction.destination, result);
			if (written.action == StepAction::exit)
				return written;
		}
	}
	materialize_flags(cpu);
	cpu.rflags = carry ? cpu.rflags | rflags_carry :
		cpu.rflags & ~rflags_carry;
	cpu.rflags |= rflags_reserved;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult bit_count(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto source_read = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!source_read.succeeded)
		return read_failure(source_read);
	const auto width = gadget.instruction.source.width;
	const auto bits = width_bits(width);
	const auto source = source_read.value & width_mask(width);
	std::uint64_t result{};
	bool write = true;
	if (gadget.instruction.operation == Operation::bit_scan_forward) {
		if (source == 0)
			write = false;
		else
			result = std::countr_zero(source);
	} else if (gadget.instruction.operation == Operation::bit_scan_reverse) {
		if (source == 0)
			write = false;
		else
			result = 63U - std::countl_zero(source);
	} else if (gadget.instruction.operation ==
		Operation::count_trailing_zeros) {
		result = source == 0 ? bits : std::countr_zero(source);
	} else if (gadget.instruction.operation ==
		Operation::count_leading_zeros) {
		result = source == 0 ? bits :
			std::countl_zero(source) - (64U - bits);
	} else if (gadget.instruction.operation == Operation::population_count) {
		result = std::popcount(source);
	} else {
		return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
	}
	if (write) {
		const auto written = write_operand(gadget, cpu, memory,
			gadget.instruction.destination, result);
		if (written.action == StepAction::exit)
			return written;
	}
	materialize_flags(cpu);
	if (gadget.instruction.operation == Operation::bit_scan_forward ||
		gadget.instruction.operation == Operation::bit_scan_reverse) {
		cpu.rflags = source == 0 ? cpu.rflags | rflags_zero :
			cpu.rflags & ~rflags_zero;
	} else {
		constexpr auto arithmetic = rflags_carry | rflags_parity |
			rflags_auxiliary | rflags_zero | rflags_sign | rflags_overflow;
		cpu.rflags &= ~arithmetic;
		if (gadget.instruction.operation == Operation::population_count) {
			if (source == 0)
				cpu.rflags |= rflags_zero;
		} else {
			if (source == 0)
				cpu.rflags |= rflags_carry;
			if (result == 0)
				cpu.rflags |= rflags_zero;
		}
	}
	cpu.rflags |= rflags_reserved;
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult compare_exchange(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto destination = read_operand(
		gadget, cpu, memory, gadget.instruction.destination);
	if (!destination.succeeded)
		return read_failure(destination);
	const auto source = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!source.succeeded)
		return read_failure(source);
	const auto width = gadget.instruction.destination.width;
	const auto accumulator = cpu.gpr(Register::rax) & width_mask(width);
	const bool equal = accumulator == destination.value;
	StepResult written{};
	if (equal) {
		written = write_operand(gadget, cpu, memory,
			gadget.instruction.destination, source.value);
		if (written.action == StepAction::exit)
			return written;
	} else {
		write_gpr(cpu, static_cast<std::uint8_t>(Register::rax),
			width, destination.value);
	}
	set_arithmetic_flags(cpu, accumulator, destination.value,
		accumulator - destination.value, width, true, 0);
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult compare_exchange_pair(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto address = effective_address(gadget, cpu,
		gadget.instruction.destination.memory, true);
	const auto size = static_cast<std::size_t>(
		gadget.instruction.auxiliary.immediate);
	std::array<std::byte, 16> bytes{};
	if (!read_memory_bytes(memory, address,
		std::span{bytes}.first(size)))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, address, ::uml::memory::MemoryAccess::read)};
	std::uint64_t lower{};
	std::uint64_t upper{};
	std::memcpy(&lower, bytes.data(), 8);
	if (size == 16)
		std::memcpy(&upper, bytes.data() + 8, 8);
	const auto expected_lower = size == 8 ?
		static_cast<std::uint32_t>(cpu.gpr(Register::rax)) :
		cpu.gpr(Register::rax);
	const auto expected_upper = size == 8 ?
		static_cast<std::uint32_t>(cpu.gpr(Register::rdx)) :
		cpu.gpr(Register::rdx);
	const auto observed_lower = size == 8 ?
		static_cast<std::uint32_t>(lower) : lower;
	const auto observed_upper = size == 8 ?
		static_cast<std::uint32_t>(lower >> 32U) : upper;
	const bool equal = observed_lower == expected_lower &&
		observed_upper == expected_upper;
	StepResult written{};
	if (equal) {
		const auto desired_lower = size == 8 ?
			(static_cast<std::uint64_t>(
				static_cast<std::uint32_t>(cpu.gpr(Register::rcx))) << 32U) |
				static_cast<std::uint32_t>(cpu.gpr(Register::rbx)) :
			cpu.gpr(Register::rbx);
		const auto desired_upper = cpu.gpr(Register::rcx);
		std::memcpy(bytes.data(), &desired_lower, 8);
		if (size == 16)
			std::memcpy(bytes.data() + 8, &desired_upper, 8);
		if (!write_memory_bytes(memory, address,
			std::span<const std::byte>{bytes}.first(size)))
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault,
				memory_fault(memory, address,
					::uml::memory::MemoryAccess::write)};
		written = {StepAction::continue_program,
			::uml::cpu::EngineEvent::continued, {}, address,
			static_cast<std::uint8_t>(size)};
	} else if (size == 8) {
		write_gpr(cpu, static_cast<std::uint8_t>(Register::rax),
			OperandWidth::dword, observed_lower);
		write_gpr(cpu, static_cast<std::uint8_t>(Register::rdx),
			OperandWidth::dword, observed_upper);
	} else {
		cpu.gpr(Register::rax) = observed_lower;
		cpu.gpr(Register::rdx) = observed_upper;
	}
	materialize_flags(cpu);
	cpu.rflags = equal ? cpu.rflags | rflags_zero :
		cpu.rflags & ~rflags_zero;
	cpu.rflags |= rflags_reserved;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult exchange_add(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto destination = read_operand(
		gadget, cpu, memory, gadget.instruction.destination);
	if (!destination.succeeded)
		return read_failure(destination);
	const auto source = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!source.succeeded)
		return read_failure(source);
	const auto result = destination.value + source.value;
	StepResult written{};
	if (gadget.instruction.destination.kind == OperandKind::memory) {
		written = write_operand(gadget, cpu, memory,
			gadget.instruction.destination, result);
		if (written.action == StepAction::exit)
			return written;
		write_register(cpu, gadget.instruction.source, destination.value);
	} else {
		write_register(cpu, gadget.instruction.source, destination.value);
		write_register(cpu, gadget.instruction.destination, result);
	}
	set_arithmetic_flags(cpu, destination.value, source.value, result,
		gadget.instruction.destination.width, false, 0);
	advance(gadget, cpu);
	return written;
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

[[nodiscard]] StepResult set_condition(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto value = condition_holds(cpu, gadget.instruction.condition) ?
		1U : 0U;
	const auto written = write_operand(gadget, cpu, memory,
		gadget.instruction.destination, value);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult conditional_move(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto source = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!source.succeeded)
		return read_failure(source);
	if (condition_holds(cpu, gadget.instruction.condition)) {
		const auto written = write_operand(gadget, cpu, memory,
			gadget.instruction.destination, source.value);
		if (written.action == StepAction::exit)
			return written;
	}
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult byte_swap(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	const auto value = read_register(cpu, gadget.instruction.destination);
	const auto result = gadget.instruction.destination.width ==
		OperandWidth::qword ? __builtin_bswap64(value) :
		static_cast<std::uint64_t>(
			__builtin_bswap32(static_cast<std::uint32_t>(value)));
	write_register(cpu, gadget.instruction.destination, result);
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult sign_extend_registers(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &) noexcept
{
	const auto width = gadget.instruction.destination.width;
	if (gadget.instruction.operation ==
		Operation::sign_extend_accumulator) {
		const auto source_width = width == OperandWidth::word ?
			OperandWidth::byte : width == OperandWidth::dword ?
			OperandWidth::word : OperandWidth::dword;
		write_gpr(cpu, 0, width,
			sign_extend_width(cpu.gpr(Register::rax), source_width));
	} else {
		const auto sign = (cpu.gpr(Register::rax) &
			(std::uint64_t{1} << (width_bits(width) - 1U))) != 0;
		write_gpr(cpu, 2, width, sign ? width_mask(width) : 0);
	}
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult flag_control(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	materialize_flags(cpu);
	switch (gadget.instruction.operation) {
	case Operation::carry_clear: cpu.rflags &= ~rflags_carry; break;
	case Operation::carry_set: cpu.rflags |= rflags_carry; break;
	case Operation::carry_complement: cpu.rflags ^= rflags_carry; break;
	case Operation::direction_clear: cpu.rflags &= ~rflags_direction; break;
	case Operation::direction_set: cpu.rflags |= rflags_direction; break;
	default:
		return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
	}
	cpu.rflags |= rflags_reserved;
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult lahf_sahf(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	materialize_flags(cpu);
	constexpr auto mask = rflags_sign | rflags_zero | rflags_auxiliary |
		rflags_parity | rflags_carry;
	if (gadget.instruction.operation == Operation::lahf) {
		cpu.gpr(Register::rax) = (cpu.gpr(Register::rax) & ~0xff00ULL) |
			(((cpu.rflags & mask) | rflags_reserved) << 8U);
	} else {
		const auto value = (cpu.gpr(Register::rax) >> 8U) & 0xffU;
		cpu.rflags = (cpu.rflags & ~mask) | (value & mask) | rflags_reserved;
	}
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult push_flags(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	materialize_flags(cpu);
	const auto width = gadget.instruction.source.width;
	const auto size = width_bytes(width);
	const auto address = cpu.gpr(Register::rsp) - size;
	if (!write_memory(memory, address, width, cpu.rflags))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, address, ::uml::memory::MemoryAccess::write)};
	cpu.gpr(Register::rsp) = address;
	advance(gadget, cpu);
	return {StepAction::continue_program,
		::uml::cpu::EngineEvent::continued, {}, address,
		width_bytes(width)};
}

[[nodiscard]] StepResult pop_flags(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto width = gadget.instruction.destination.width;
	const auto address = cpu.gpr(Register::rsp);
	std::uint64_t value{};
	if (!read_memory(memory, address, width, value))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, address, ::uml::memory::MemoryAccess::read)};
	materialize_flags(cpu);
	constexpr auto writable = rflags_carry | rflags_parity |
		rflags_auxiliary | rflags_zero | rflags_sign | rflags_direction |
		rflags_overflow | (std::uint64_t{1} << 8U) |
		(std::uint64_t{1} << 14U) | (std::uint64_t{1} << 18U) |
		(std::uint64_t{1} << 21U);
	const auto width_write_mask = width == OperandWidth::word ?
		writable & 0xffffU : writable;
	cpu.rflags = (cpu.rflags & ~width_write_mask) |
		(value & width_write_mask) | rflags_reserved;
	cpu.gpr(Register::rsp) += width_bytes(width);
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult leave(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto width = gadget.instruction.destination.width;
	const auto address = cpu.gpr(Register::rbp);
	std::uint64_t value{};
	if (!read_memory(memory, address, width, value))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, address, ::uml::memory::MemoryAccess::read)};
	cpu.gpr(Register::rsp) = address + width_bytes(width);
	write_gpr(cpu, static_cast<std::uint8_t>(Register::rbp), width, value);
	advance(gadget, cpu);
	return {};
}

void update_string_register(CpuState &cpu, Register reg, bool address_32,
	std::uint8_t size, bool decrement) noexcept
{
	auto &value = cpu.gpr(reg);
	if (address_32) {
		auto narrowed = static_cast<std::uint32_t>(value);
		narrowed = decrement ? narrowed - size : narrowed + size;
		value = narrowed;
	} else {
		value = decrement ? value - size : value + size;
	}
}

[[nodiscard]] StepResult string_operation(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool repeating = instruction.repeat != 0;
	const bool address_32 = instruction.destination.kind == OperandKind::memory ?
		instruction.destination.memory.address_32 :
		instruction.source.memory.address_32;
	auto remaining = address_32 ?
		static_cast<std::uint64_t>(
			static_cast<std::uint32_t>(cpu.gpr(Register::rcx))) :
		cpu.gpr(Register::rcx);
	if (repeating && remaining == 0) {
		advance(gadget, cpu);
		return {};
	}
	const auto width = instruction.destination.kind == OperandKind::none ?
		instruction.source.width : instruction.destination.width;
	const auto size = width_bytes(width);
	bool wrote_memory{};
	StepResult last_write{};
	for (;;) {
		if (instruction.operation == Operation::string_move) {
			const auto source = read_operand(
				gadget, cpu, memory, instruction.source);
			if (!source.succeeded)
				return read_failure(source);
			const auto written = write_operand(gadget, cpu, memory,
				instruction.destination, source.value);
			if (written.action == StepAction::exit)
				return written;
			wrote_memory = true;
			last_write = written;
			memory.invalidate_code(written.write_address);
		} else if (instruction.operation == Operation::string_store) {
			const auto source = read_operand(
				gadget, cpu, memory, instruction.source);
			if (!source.succeeded)
				return read_failure(source);
			const auto written = write_operand(gadget, cpu, memory,
				instruction.destination, source.value);
			if (written.action == StepAction::exit)
				return written;
			wrote_memory = true;
			last_write = written;
			memory.invalidate_code(written.write_address);
		} else if (instruction.operation == Operation::string_load) {
			const auto source = read_operand(
				gadget, cpu, memory, instruction.source);
			if (!source.succeeded)
				return read_failure(source);
			write_register(cpu, instruction.destination, source.value);
		} else {
			const auto left = read_operand(
				gadget, cpu, memory, instruction.destination);
			if (!left.succeeded)
				return read_failure(left);
			const auto right = read_operand(
				gadget, cpu, memory, instruction.source);
			if (!right.succeeded)
				return read_failure(right);
			set_arithmetic_flags(cpu, left.value, right.value,
				left.value - right.value, width, true, 0);
		}

		const bool source_pointer =
			instruction.operation == Operation::string_move ||
			instruction.operation == Operation::string_compare ||
			instruction.operation == Operation::string_load;
		const bool destination_pointer =
			instruction.operation == Operation::string_move ||
			instruction.operation == Operation::string_compare ||
			instruction.operation == Operation::string_store ||
			instruction.operation == Operation::string_scan;
		const bool decrement = (cpu.rflags & rflags_direction) != 0;
		if (source_pointer)
			update_string_register(cpu, Register::rsi,
				address_32, size, decrement);
		if (destination_pointer)
			update_string_register(cpu, Register::rdi,
				address_32, size, decrement);

		if (!repeating)
			break;
		--remaining;
		if (address_32)
			cpu.gpr(Register::rcx) = static_cast<std::uint32_t>(remaining);
		else
			cpu.gpr(Register::rcx) = remaining;
		if (remaining == 0)
			break;
		if (instruction.operation == Operation::string_compare ||
			instruction.operation == Operation::string_scan) {
			const bool equal = (cpu.rflags & rflags_zero) != 0;
			if ((instruction.repeat == 0xf3 && !equal) ||
				(instruction.repeat == 0xf2 && equal))
				break;
		}
	}
	advance(gadget, cpu);
	if (wrote_memory) {
		last_write.action = StepAction::return_to_dispatch;
		return last_write;
	}
	return {};
}

[[nodiscard]] StepResult enter(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto width = gadget.instruction.destination.width;
	const auto size = width_bytes(width);
	const auto nesting = static_cast<unsigned>(
		gadget.instruction.auxiliary.immediate);
	auto frame_pointer = cpu.gpr(Register::rbp);
	auto push_value = [&](std::uint64_t value) -> StepResult {
		const auto address = cpu.gpr(Register::rsp) - size;
		if (!write_memory(memory, address, width, value))
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault,
				memory_fault(memory, address,
					::uml::memory::MemoryAccess::write)};
		cpu.gpr(Register::rsp) = address;
		memory.invalidate_code(address);
		return {StepAction::continue_program,
			::uml::cpu::EngineEvent::continued, {}, address, size};
	};
	auto written = push_value(frame_pointer);
	if (written.action == StepAction::exit)
		return written;
	const auto frame = cpu.gpr(Register::rsp);
	if (nesting != 0) {
		for (unsigned level = 1; level < nesting; ++level) {
			frame_pointer -= size;
			std::uint64_t value{};
			if (!read_memory(memory, frame_pointer, width, value))
				return {StepAction::exit,
					::uml::cpu::EngineEvent::memory_fault,
					memory_fault(memory, frame_pointer,
						::uml::memory::MemoryAccess::read)};
			written = push_value(value);
			if (written.action == StepAction::exit)
				return written;
		}
		written = push_value(frame);
		if (written.action == StepAction::exit)
			return written;
	}
	write_gpr(cpu, static_cast<std::uint8_t>(Register::rbp), width, frame);
	cpu.gpr(Register::rsp) -= gadget.instruction.source.immediate;
	advance(gadget, cpu);
	written.action = StepAction::return_to_dispatch;
	return written;
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

[[nodiscard]] StepResult jump_indirect(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto target = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!target.succeeded)
		return read_failure(target);
	cpu.rip = target.value;
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
	return {StepAction::continue_program,
		::uml::cpu::EngineEvent::continued, {}, address,
		width_bytes(gadget.instruction.source.width)};
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
	const auto written = write_operand(gadget, cpu, memory,
		gadget.instruction.destination, value);
	if (written.action == StepAction::exit) {
		cpu.gpr(Register::rsp) = address;
		return written;
	}
	advance(gadget, cpu);
	return written;
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

[[nodiscard]] StepResult call_indirect(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto target = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!target.succeeded)
		return read_failure(target);
	const auto return_address = gadget.address + gadget.instruction.length;
	const auto stack_address = cpu.gpr(Register::rsp) - 8U;
	if (!write_memory(memory, stack_address, OperandWidth::qword,
		return_address))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, stack_address,
				::uml::memory::MemoryAccess::write)};
	cpu.gpr(Register::rsp) = stack_address;
	cpu.rip = target.value;
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

[[nodiscard]] StepResult cpuid(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	const auto leaf = static_cast<std::uint32_t>(cpu.gpr(Register::rax));
	const auto subleaf = static_cast<std::uint32_t>(cpu.gpr(Register::rcx));
	std::uint32_t eax{};
	std::uint32_t ebx{};
	std::uint32_t ecx{};
	std::uint32_t edx{};
	switch (leaf) {
	case 0:
		eax = 7;
		ebx = 0x756e6547U;
		edx = 0x49656e69U;
		ecx = 0x6c65746eU;
		break;
	case 1:
		eax = 0x0003'06c3U;
		ebx = 8U << 8U;
		ecx = (1U << 23U) | (1U << 26U) | (1U << 27U) |
			(1U << 30U);
		edx = (1U << 0U) | (1U << 4U) | (1U << 8U) |
			(1U << 15U) | (1U << 23U) | (1U << 24U) |
			(1U << 25U) | (1U << 26U);
		break;
	case 7:
		if (subleaf == 0)
			ebx = (1U << 18U);
		break;
	case 0x8000'0000U:
		eax = 0x8000'0008U;
		break;
	case 0x8000'0001U:
		ecx = 1U << 5U;
		edx = (1U << 11U) | (1U << 20U) | (1U << 29U);
		break;
	case 0x8000'0002U:
		eax = 0x20554d4cU;
		ebx = 0x5f363878U;
		ecx = 0x67203436U;
		edx = 0x65676461U;
		break;
	case 0x8000'0003U:
		eax = 0x65207374U;
		ebx = 0x616c756dU;
		ecx = 0x00726f74U;
		break;
	default:
		break;
	}
	write_gpr(cpu, static_cast<std::uint8_t>(Register::rax),
		OperandWidth::dword, eax);
	write_gpr(cpu, static_cast<std::uint8_t>(Register::rbx),
		OperandWidth::dword, ebx);
	write_gpr(cpu, static_cast<std::uint8_t>(Register::rcx),
		OperandWidth::dword, ecx);
	write_gpr(cpu, static_cast<std::uint8_t>(Register::rdx),
		OperandWidth::dword, edx);
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult read_extended_control(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &) noexcept
{
	const auto value = cpu.gpr(Register::rcx) == 0 ? 3U : 0U;
	write_gpr(cpu, static_cast<std::uint8_t>(Register::rax),
		OperandWidth::dword, value);
	write_gpr(cpu, static_cast<std::uint8_t>(Register::rdx),
		OperandWidth::dword, 0);
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult read_timestamp(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	const auto now = std::chrono::steady_clock::now().time_since_epoch();
	const auto ticks = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
	write_gpr(cpu, static_cast<std::uint8_t>(Register::rax),
		OperandWidth::dword, ticks);
	write_gpr(cpu, static_cast<std::uint8_t>(Register::rdx),
		OperandWidth::dword, ticks >> 32U);
	if (gadget.instruction.auxiliary.immediate != 0)
		write_gpr(cpu, static_cast<std::uint8_t>(Register::rcx),
			OperandWidth::dword, 0);
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult read_processor_id(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto written = write_operand(gadget, cpu, memory,
		gadget.instruction.destination, 1);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult random_value(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	auto value = next_random() & width_mask(
		gadget.instruction.destination.width);
	if (value == 0)
		value = 1;
	const auto written = write_operand(gadget, cpu, memory,
		gadget.instruction.destination, value);
	if (written.action == StepAction::exit)
		return written;
	materialize_flags(cpu);
	constexpr auto arithmetic = rflags_carry | rflags_parity |
		rflags_auxiliary | rflags_zero | rflags_sign | rflags_overflow;
	cpu.rflags = (cpu.rflags & ~arithmetic) | rflags_carry | rflags_reserved;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult fs_gs_base(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto operation = gadget.instruction.operation;
	if (operation == Operation::read_fs_base ||
		operation == Operation::read_gs_base) {
		const auto value = operation == Operation::read_fs_base ?
			cpu.fs_base : cpu.gs_base;
		const auto written = write_operand(gadget, cpu, memory,
			gadget.instruction.destination, value);
		if (written.action == StepAction::exit)
			return written;
	} else {
		const auto source = read_operand(
			gadget, cpu, memory, gadget.instruction.source);
		if (!source.succeeded)
			return read_failure(source);
		if (operation == Operation::write_fs_base)
			cpu.fs_base = source.value;
		else
			cpu.gs_base = source.value;
	}
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult store_gdtr(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto address = effective_address(gadget, cpu,
		gadget.instruction.destination.memory, true);
	std::array<std::byte, 10> bytes{};
	const std::uint64_t base = 0xffff'fffffffe'0000ULL;
	std::memcpy(bytes.data() + 2, &base, sizeof(base));
	if (!write_memory_bytes(memory, address, bytes))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, address, ::uml::memory::MemoryAccess::write)};
	memory.invalidate_code(address);
	advance(gadget, cpu);
	return {StepAction::return_to_dispatch,
		::uml::cpu::EngineEvent::continued, {}, address, 10};
}

[[nodiscard]] StepResult store_machine_status(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	StepResult written{};
	if (gadget.instruction.destination.kind == OperandKind::reg &&
		gadget.instruction.destination.width == OperandWidth::dword) {
		auto &value = cpu.registers[
			gadget.instruction.destination.reg.index];
		value = (value & 0xffff'ffff'0000'0000ULL) | 0x8005'0033U;
	} else {
		written = write_operand(gadget, cpu, memory,
			gadget.instruction.destination, 0x8005'0033U);
	}
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult cache_zero(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto address = cpu.gpr(Register::rax) & ~std::uint64_t{63};
	const std::array<std::byte, 64> zeros{};
	if (!write_memory_bytes(memory, address, zeros))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, address, ::uml::memory::MemoryAccess::write)};
	memory.invalidate_code(address);
	advance(gadget, cpu);
	return {StepAction::return_to_dispatch,
		::uml::cpu::EngineEvent::continued, {}, address, 64};
}

[[nodiscard]] StepResult memory_fence(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	std::atomic_thread_fence(std::memory_order_seq_cst);
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult cache_flush(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	std::atomic_thread_fence(std::memory_order_seq_cst);
	advance(gadget, cpu);
	return {};
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
	case Operation::move_zero_extend:
	case Operation::move_sign_extend:
		return move_extended;
	case Operation::lea: return lea;
	case Operation::exchange: return exchange;
	case Operation::add:
	case Operation::subtract:
	case Operation::bit_and:
	case Operation::bit_or:
	case Operation::bit_xor:
	case Operation::compare:
	case Operation::test:
		return arithmetic;
	case Operation::add_carry:
	case Operation::subtract_borrow:
		return arithmetic_with_carry;
	case Operation::increment:
	case Operation::decrement:
	case Operation::negate:
	case Operation::bit_not:
		return unary;
	case Operation::shift_left:
	case Operation::shift_right:
	case Operation::shift_arithmetic_right:
	case Operation::rotate_left:
	case Operation::rotate_right:
	case Operation::rotate_carry_left:
	case Operation::rotate_carry_right:
		return shift_rotate;
	case Operation::double_shift_left:
	case Operation::double_shift_right:
		return double_shift;
	case Operation::multiply:
	case Operation::multiply_signed:
		return multiply;
	case Operation::divide:
	case Operation::divide_signed:
		return divide;
	case Operation::bit_test:
	case Operation::bit_set:
	case Operation::bit_reset:
	case Operation::bit_complement:
		return bit_operation;
	case Operation::bit_scan_forward:
	case Operation::bit_scan_reverse:
	case Operation::count_trailing_zeros:
	case Operation::count_leading_zeros:
	case Operation::population_count:
		return bit_count;
	case Operation::compare_exchange: return compare_exchange;
	case Operation::compare_exchange_pair: return compare_exchange_pair;
	case Operation::exchange_add: return exchange_add;
	case Operation::set_condition: return set_condition;
	case Operation::conditional_move: return conditional_move;
	case Operation::byte_swap: return byte_swap;
	case Operation::sign_extend_accumulator:
	case Operation::sign_extend_data:
		return sign_extend_registers;
	case Operation::push: return push;
	case Operation::pop: return pop;
	case Operation::push_flags: return push_flags;
	case Operation::pop_flags: return pop_flags;
	case Operation::leave: return leave;
	case Operation::enter: return enter;
	case Operation::string_move:
	case Operation::string_compare:
	case Operation::string_store:
	case Operation::string_load:
	case Operation::string_scan:
		return string_operation;
	case Operation::carry_clear:
	case Operation::carry_set:
	case Operation::carry_complement:
	case Operation::direction_clear:
	case Operation::direction_set:
		return flag_control;
	case Operation::lahf:
	case Operation::sahf:
		return lahf_sahf;
	case Operation::jump: return jump;
	case Operation::jump_indirect: return jump_indirect;
	case Operation::jump_conditional: return jump_conditional;
	case Operation::call: return call;
	case Operation::call_indirect: return call_indirect;
	case Operation::ret: return ret;
	case Operation::cpuid: return cpuid;
	case Operation::read_extended_control: return read_extended_control;
	case Operation::read_timestamp: return read_timestamp;
	case Operation::read_processor_id: return read_processor_id;
	case Operation::random: return random_value;
	case Operation::read_fs_base:
	case Operation::read_gs_base:
	case Operation::write_fs_base:
	case Operation::write_gs_base:
		return fs_gs_base;
	case Operation::store_gdtr: return store_gdtr;
	case Operation::store_machine_status: return store_machine_status;
	case Operation::cache_zero: return cache_zero;
	case Operation::memory_fence: return memory_fence;
	case Operation::cache_flush: return cache_flush;
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
		if (step.write_size != 0) {
			const auto code_end = program.address() + program.byte_count();
			const auto write_end = step.write_address + step.write_size;
			if (step.write_address < code_end && write_end > cpu.rip) {
				memory.invalidate_code(step.write_address);
				return result;
			}
		}
		if (step.action == StepAction::return_to_dispatch)
			return result;
	}
	return result;
}

} // namespace uml::cpu::x86_64::gadgets
