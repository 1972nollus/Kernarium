// SPDX-License-Identifier: GPL-2.0-or-later
#include "program.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <type_traits>
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
	return width_bytes(width) >= 8 ? ~std::uint64_t{0} :
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
	case OperandWidth::xmm:
	case OperandWidth::ymm:
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

struct VectorRead final {
	bool succeeded{};
	std::array<std::byte, 32> value{};
	::uml::cpu::FaultInfo fault{};
};

[[nodiscard]] OperandRead read_operand(const Gadget &gadget,
	const CpuState &cpu, ::uml::memory::GuestAddressSpace &memory,
	const Operand &operand) noexcept
{
	switch (operand.kind) {
	case OperandKind::reg:
		return {true, read_register(cpu, operand), {}};
	case OperandKind::vector_reg:
	case OperandKind::mmx_reg:
		break;
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

[[nodiscard]] VectorRead read_vector_operand(const Gadget &gadget,
	const CpuState &cpu, ::uml::memory::GuestAddressSpace &memory,
	const Operand &operand) noexcept
{
	VectorRead result{};
	const auto size = static_cast<std::size_t>(width_bytes(operand.width));
	if (operand.kind == OperandKind::vector_reg) {
		std::copy_n(cpu.vectors[operand.reg.index].begin(), size,
			result.value.begin());
		result.succeeded = true;
		return result;
	}
	if (operand.kind == OperandKind::mmx_reg) {
		std::memcpy(result.value.data(), &cpu.mmx[operand.reg.index],
			sizeof(std::uint64_t));
		result.succeeded = true;
		return result;
	}
	if (operand.kind == OperandKind::memory) {
		const auto address = effective_address(
			gadget, cpu, operand.memory, true);
		if (!read_memory_bytes(memory, address,
			std::span{result.value}.first(size))) {
			result.fault = memory_fault(memory, address,
				::uml::memory::MemoryAccess::read);
			return result;
		}
		result.succeeded = true;
	}
	return result;
}

[[nodiscard]] StepResult write_vector_operand(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory,
	const Operand &operand, std::span<const std::byte> value,
	bool zero_upper) noexcept
{
	const auto size = static_cast<std::size_t>(width_bytes(operand.width));
	if (operand.kind == OperandKind::vector_reg) {
		std::copy_n(value.begin(), size,
			cpu.vectors[operand.reg.index].begin());
		if (zero_upper && size < cpu.vectors[operand.reg.index].size())
			std::fill(cpu.vectors[operand.reg.index].begin() +
				static_cast<std::ptrdiff_t>(size),
				cpu.vectors[operand.reg.index].end(), std::byte{});
		return {};
	}
	if (operand.kind == OperandKind::mmx_reg) {
		std::memcpy(&cpu.mmx[operand.reg.index], value.data(),
			sizeof(std::uint64_t));
		return {};
	}
	if (operand.kind == OperandKind::memory) {
		const auto address = effective_address(
			gadget, cpu, operand.memory, true);
		if (!write_memory_bytes(memory, address, value.first(size)))
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault,
				memory_fault(memory, address,
					::uml::memory::MemoryAccess::write)};
		return {StepAction::continue_program,
			::uml::cpu::EngineEvent::continued, {}, address,
			static_cast<std::uint8_t>(size)};
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

[[nodiscard]] StepResult vector_move(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto source = read_vector_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!source.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			source.fault};
	const auto written = write_vector_operand(gadget, cpu, memory,
		gadget.instruction.destination, source.value,
		gadget.instruction.vex);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult vector_move_scalar(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto source = read_vector_operand(
		gadget, cpu, memory, instruction.source);
	if (!source.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			source.fault};
	const auto scalar_size = static_cast<std::size_t>(
		width_bytes(instruction.source.width));
	if (instruction.destination.kind == OperandKind::memory) {
		const auto written = write_vector_operand(gadget, cpu, memory,
			instruction.destination,
			std::span<const std::byte>{source.value}.first(scalar_size), false);
		if (written.action == StepAction::exit)
			return written;
		advance(gadget, cpu);
		return written;
	}

	auto result = cpu.vectors[instruction.destination.reg.index];
	std::copy_n(source.value.begin(), scalar_size, result.begin());
	if (instruction.vex) {
		if (instruction.auxiliary.kind == OperandKind::vector_reg) {
			const auto &merge = cpu.vectors[instruction.auxiliary.reg.index];
			std::copy(merge.begin() +
				static_cast<std::ptrdiff_t>(scalar_size),
				merge.begin() + 16, result.begin() +
				static_cast<std::ptrdiff_t>(scalar_size));
		} else {
			std::fill(result.begin() +
				static_cast<std::ptrdiff_t>(scalar_size),
				result.begin() + 16, std::byte{});
		}
		std::fill(result.begin() + 16, result.end(), std::byte{});
	} else if (instruction.source.kind == OperandKind::memory) {
		std::fill(result.begin() +
			static_cast<std::ptrdiff_t>(scalar_size),
			result.begin() + 16, std::byte{});
	}
	cpu.vectors[instruction.destination.reg.index] = result;
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult vector_zero(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	const bool all = gadget.instruction.auxiliary.immediate != 0;
	for (auto &vector : cpu.vectors) {
		auto begin = all ? vector.begin() : vector.begin() + 16;
		std::fill(begin, vector.end(), std::byte{});
	}
	advance(gadget, cpu);
	return {};
}

template<typename Value>
[[nodiscard]] Value vector_element(
	const std::array<std::byte, 32> &vector, std::size_t offset) noexcept
{
	Value value{};
	std::memcpy(&value, vector.data() + offset, sizeof(value));
	return value;
}

template<typename Value>
void set_vector_element(std::array<std::byte, 32> &vector,
	std::size_t offset, Value value) noexcept
{
	std::memcpy(vector.data() + offset, &value, sizeof(value));
}

template<typename Float>
[[nodiscard]] bool vector_compare(
	Float left, Float right, std::uint8_t control) noexcept
{
	const bool unordered = std::isnan(left) || std::isnan(right);
	const bool ordered = !unordered;
	switch (control & 0x1fU) {
	case 0:
	case 16: return ordered && left == right;
	case 8:
	case 24: return unordered || left == right;
	case 1:
	case 17: return ordered && left < right;
	case 2:
	case 18: return ordered && left <= right;
	case 3:
	case 19: return unordered;
	case 4:
	case 20: return unordered || left != right;
	case 5:
	case 21: return unordered || left >= right;
	case 6:
	case 22: return unordered || left > right;
	case 7:
	case 23: return ordered;
	case 9:
	case 25: return unordered || left < right;
	case 10:
	case 26: return unordered || left <= right;
	case 11:
	case 27: return false;
	case 12:
	case 28: return ordered && left != right;
	case 13:
	case 29: return ordered && left >= right;
	case 14:
	case 30: return ordered && left > right;
	case 15:
	case 31: return true;
	}
	return false;
}

template<typename Float>
void vector_float_alu(std::array<std::byte, 32> &result,
	const std::array<std::byte, 32> &left,
	const std::array<std::byte, 32> &right, std::uint8_t opcode,
	std::uint8_t control, std::size_t lane_count,
	std::uint32_t mxcsr) noexcept
{
	for (std::size_t lane = 0; lane < lane_count; ++lane) {
		const auto offset = lane * sizeof(Float);
		auto first = vector_element<Float>(left, offset);
		auto second = vector_element<Float>(right, offset);
		if ((mxcsr & (1U << 6U)) != 0) {
			if (std::fpclassify(first) == FP_SUBNORMAL)
				first = std::copysign(Float{0}, first);
			if (std::fpclassify(second) == FP_SUBNORMAL)
				second = std::copysign(Float{0}, second);
		}
		Float value{};
		switch (opcode) {
		case 0x51: value = std::sqrt(second); break;
		case 0x52: value = Float{1} / std::sqrt(second); break;
		case 0x53: value = Float{1} / second; break;
		case 0x58: value = first + second; break;
		case 0x59: value = first * second; break;
		case 0x5c: value = first - second; break;
		case 0x5d:
			value = !std::isnan(first) && first < second ? first : second;
			break;
		case 0x5e: value = first / second; break;
		case 0x5f:
			value = !std::isnan(first) && first > second ? first : second;
			break;
		case 0xc2: {
			using Integer = std::conditional_t<sizeof(Float) == 4,
				std::uint32_t, std::uint64_t>;
			const Integer mask = vector_compare(first, second, control) ?
				~Integer{0} : Integer{0};
			set_vector_element(result, offset, mask);
			continue;
		}
		default:
			continue;
		}
		if ((mxcsr & (1U << 15U)) != 0 &&
			std::fpclassify(value) == FP_SUBNORMAL)
			value = std::copysign(Float{0}, value);
		set_vector_element(result, offset, value);
	}
}

[[nodiscard]] StepResult vector_alu(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto left = read_vector_operand(
		gadget, cpu, memory, instruction.source);
	if (!left.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			left.fault};
	const auto right = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!right.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			right.fault};
	auto result = left.value;
	const auto size = static_cast<std::size_t>(
		width_bytes(instruction.destination.width));
	const bool scalar = instruction.vector_prefix >= 2;
	if (instruction.vector_opcode == 0x14 ||
		instruction.vector_opcode == 0x15) {
		const auto element_size = instruction.vector_prefix == 1 ? 8U : 4U;
		for (std::size_t lane = 0; lane < size; lane += 16) {
			const auto half = 8U / element_size;
			const auto base = instruction.vector_opcode == 0x15 ? half : 0U;
			for (std::size_t index = 0; index < half; ++index) {
				std::copy_n(left.value.begin() +
					static_cast<std::ptrdiff_t>(lane +
						(base + index) * element_size),
					element_size, result.begin() +
					static_cast<std::ptrdiff_t>(lane +
						index * element_size * 2U));
				std::copy_n(right.value.begin() +
					static_cast<std::ptrdiff_t>(lane +
						(base + index) * element_size),
					element_size, result.begin() +
					static_cast<std::ptrdiff_t>(lane +
						(index * 2U + 1U) * element_size));
			}
		}
	} else if (instruction.vector_opcode == 0xc6) {
		const auto element_size = instruction.vector_prefix == 1 ? 8U : 4U;
		for (std::size_t lane = 0; lane < size; lane += 16) {
			if (element_size == 4) {
				for (std::size_t index = 0; index < 2; ++index) {
					const auto first_index =
						(instruction.vector_control >> (index * 2U)) & 3U;
					const auto second_index =
						(instruction.vector_control >> ((index + 2U) * 2U)) & 3U;
					set_vector_element(result, lane + index * 4U,
						vector_element<std::uint32_t>(left.value,
							lane + first_index * 4U));
					set_vector_element(result, lane + (index + 2U) * 4U,
						vector_element<std::uint32_t>(right.value,
							lane + second_index * 4U));
				}
			} else {
				const auto control = instruction.vector_control >>
					((lane / 16U) * 2U);
				set_vector_element(result, lane,
					vector_element<std::uint64_t>(left.value,
						lane + (control & 1U) * 8U));
				set_vector_element(result, lane + 8,
					vector_element<std::uint64_t>(right.value,
						lane + ((control >> 1U) & 1U) * 8U));
			}
		}
	} else if (instruction.vector_opcode == 0x7c ||
		instruction.vector_opcode == 0x7d) {
		const bool subtract = instruction.vector_opcode == 0x7d;
		for (std::size_t lane = 0; lane < size; lane += 16) {
			if (instruction.vector_prefix == 1) {
				const auto a0 = vector_element<double>(left.value, lane);
				const auto a1 = vector_element<double>(left.value, lane + 8);
				const auto b0 = vector_element<double>(right.value, lane);
				const auto b1 = vector_element<double>(right.value, lane + 8);
				set_vector_element(result, lane, subtract ? a0 - a1 : a0 + a1);
				set_vector_element(result, lane + 8,
					subtract ? b0 - b1 : b0 + b1);
			} else {
				for (std::size_t pair = 0; pair < 2; ++pair) {
					const auto a0 = vector_element<float>(left.value,
						lane + pair * 8U);
					const auto a1 = vector_element<float>(left.value,
						lane + pair * 8U + 4U);
					const auto b0 = vector_element<float>(right.value,
						lane + pair * 8U);
					const auto b1 = vector_element<float>(right.value,
						lane + pair * 8U + 4U);
					set_vector_element(result, lane + pair * 4U,
						subtract ? a0 - a1 : a0 + a1);
					set_vector_element(result, lane + (pair + 2U) * 4U,
						subtract ? b0 - b1 : b0 + b1);
				}
			}
		}
	} else if (instruction.vector_opcode == 0xd0) {
		if (instruction.vector_prefix == 1) {
			for (std::size_t offset = 0; offset < size; offset += 8) {
				const auto a = vector_element<double>(left.value, offset);
				const auto b = vector_element<double>(right.value, offset);
				set_vector_element(result, offset,
					((offset / 8U) & 1U) == 0 ? a - b : a + b);
			}
		} else {
			for (std::size_t offset = 0; offset < size; offset += 4) {
				const auto a = vector_element<float>(left.value, offset);
				const auto b = vector_element<float>(right.value, offset);
				set_vector_element(result, offset,
					((offset / 4U) & 1U) == 0 ? a - b : a + b);
			}
		}
	} else if (instruction.vector_opcode >= 0x54 &&
		instruction.vector_opcode <= 0x57) {
		for (std::size_t offset = 0; offset < size; offset += 8) {
			const auto first = vector_element<std::uint64_t>(left.value, offset);
			const auto second = vector_element<std::uint64_t>(right.value, offset);
			std::uint64_t value{};
			switch (instruction.vector_opcode) {
			case 0x54: value = first & second; break;
			case 0x55: value = ~first & second; break;
			case 0x56: value = first | second; break;
			case 0x57: value = first ^ second; break;
			default: break;
			}
			set_vector_element(result, offset, value);
		}
	} else if (instruction.vector_prefix == 1 ||
		instruction.vector_prefix == 3) {
		vector_float_alu<double>(result, left.value, right.value,
			instruction.vector_opcode, instruction.vector_control,
			scalar ? 1U : size / sizeof(double), cpu.mxcsr);
	} else {
		vector_float_alu<float>(result, left.value, right.value,
			instruction.vector_opcode, instruction.vector_control,
			scalar ? 1U : size / sizeof(float), cpu.mxcsr);
	}
	const auto written = write_vector_operand(gadget, cpu, memory,
		instruction.destination, result, instruction.vex);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult vector_lane_move(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.vector_opcode == 0x13 ||
		instruction.vector_opcode == 0x17) {
		const auto source = read_vector_operand(
			gadget, cpu, memory, instruction.source);
		if (!source.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, source.fault};
		auto value = source.value;
		if (instruction.vector_opcode == 0x17)
			std::copy_n(source.value.begin() + 8, 8, value.begin());
		const auto written = write_vector_operand(gadget, cpu, memory,
			instruction.destination, value, false);
		if (written.action == StepAction::exit)
			return written;
		advance(gadget, cpu);
		return written;
	}

	const auto incoming = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!incoming.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			incoming.fault};
	const bool duplicate = (instruction.vector_opcode == 0x12 &&
		(instruction.vector_prefix == 2 ||
			instruction.vector_prefix == 3)) ||
		(instruction.vector_opcode == 0x16 &&
			instruction.vector_prefix == 2);
	std::array<std::byte, 32> result{};
	if (duplicate) {
		const auto size = static_cast<std::size_t>(
			width_bytes(instruction.destination.width));
		if (instruction.vector_prefix == 3) {
			for (std::size_t lane = 0; lane < size / 8; ++lane)
				std::copy_n(incoming.value.begin() +
					static_cast<std::ptrdiff_t>((lane / 2) * 16),
					8, result.begin() +
					static_cast<std::ptrdiff_t>(lane * 8));
		} else {
			for (std::size_t lane = 0; lane < size / 4; ++lane) {
				const auto source_lane = (lane / 2) * 2 +
					(instruction.vector_opcode == 0x16 ? 1U : 0U);
				std::copy_n(incoming.value.begin() +
					static_cast<std::ptrdiff_t>(source_lane * 4),
					4, result.begin() +
					static_cast<std::ptrdiff_t>(lane * 4));
			}
		}
	} else {
		const auto merge = read_vector_operand(
			gadget, cpu, memory, instruction.source);
		if (!merge.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, merge.fault};
		result = merge.value;
		if (instruction.vector_opcode == 0x12) {
			const auto offset = instruction.auxiliary.kind ==
				OperandKind::vector_reg ? 8U : 0U;
			std::copy_n(incoming.value.begin() + offset, 8, result.begin());
		} else {
			std::copy_n(incoming.value.begin(), 8, result.begin() + 8);
		}
	}
	const auto written = write_vector_operand(gadget, cpu, memory,
		instruction.destination, result, instruction.vex);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

template<typename Float>
[[nodiscard]] std::int32_t convert_to_i32(
	Float value, std::uint32_t mxcsr, bool truncate) noexcept
{
	long double rounded{};
	if (truncate) {
		rounded = std::trunc(static_cast<long double>(value));
	} else {
		switch ((mxcsr >> 13U) & 3U) {
		case 0: rounded = std::nearbyint(static_cast<long double>(value)); break;
		case 1: rounded = std::floor(static_cast<long double>(value)); break;
		case 2: rounded = std::ceil(static_cast<long double>(value)); break;
		case 3: rounded = std::trunc(static_cast<long double>(value)); break;
		default: rounded = value; break;
		}
	}
	if (!std::isfinite(value) ||
		rounded < std::numeric_limits<std::int32_t>::min() ||
		rounded > std::numeric_limits<std::int32_t>::max())
		return std::numeric_limits<std::int32_t>::min();
	return static_cast<std::int32_t>(rounded);
}

template<typename Float>
[[nodiscard]] std::uint64_t convert_to_integer(Float value,
	OperandWidth width, std::uint32_t mxcsr, bool truncate) noexcept
{
	long double rounded{};
	if (truncate) {
		rounded = std::trunc(static_cast<long double>(value));
	} else {
		switch ((mxcsr >> 13U) & 3U) {
		case 0: rounded = std::nearbyint(static_cast<long double>(value)); break;
		case 1: rounded = std::floor(static_cast<long double>(value)); break;
		case 2: rounded = std::ceil(static_cast<long double>(value)); break;
		case 3: rounded = std::trunc(static_cast<long double>(value)); break;
		default: rounded = value; break;
		}
	}
	if (width == OperandWidth::qword) {
		if (!std::isfinite(value) || rounded < -0x1p63L ||
			rounded >= 0x1p63L)
			return std::uint64_t{1} << 63U;
		return static_cast<std::uint64_t>(
			static_cast<std::int64_t>(rounded));
	}
	if (!std::isfinite(value) || rounded < -0x1p31L || rounded >= 0x1p31L)
		return std::uint64_t{1} << 31U;
	return static_cast<std::uint32_t>(
		static_cast<std::int32_t>(rounded));
}

[[nodiscard]] StepResult vector_convert(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto input = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!input.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			input.fault};
	std::array<std::byte, 32> result{};
	const bool scalar = instruction.vector_opcode == 0x5a &&
		instruction.vector_prefix >= 2;
	if (scalar) {
		const auto merge = read_vector_operand(
			gadget, cpu, memory, instruction.source);
		if (!merge.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, merge.fault};
		result = merge.value;
	}
	const auto destination_size = static_cast<std::size_t>(
		width_bytes(instruction.destination.width));
	const auto source_size = static_cast<std::size_t>(
		width_bytes(instruction.auxiliary.width));
	if (instruction.vector_opcode == 0x5a) {
		if (instruction.vector_prefix == 0) {
			for (std::size_t lane = 0;
				lane < destination_size / sizeof(double); ++lane) {
				const auto value = vector_element<float>(
					input.value, lane * sizeof(float));
				set_vector_element(result, lane * sizeof(double),
					static_cast<double>(value));
			}
		} else if (instruction.vector_prefix == 1) {
			for (std::size_t lane = 0;
				lane < source_size / sizeof(double); ++lane) {
				const auto value = vector_element<double>(
					input.value, lane * sizeof(double));
				set_vector_element(result, lane * sizeof(float),
					static_cast<float>(value));
			}
		} else if (instruction.vector_prefix == 2) {
			set_vector_element(result, 0,
				static_cast<double>(vector_element<float>(input.value, 0)));
		} else {
			set_vector_element(result, 0,
				static_cast<float>(vector_element<double>(input.value, 0)));
		}
	} else if (instruction.vector_opcode == 0x5b) {
		for (std::size_t lane = 0;
			lane < destination_size / sizeof(std::uint32_t); ++lane) {
			const auto offset = lane * sizeof(std::uint32_t);
			if (instruction.vector_prefix == 0) {
				const auto value = vector_element<std::int32_t>(
					input.value, offset);
				set_vector_element(result, offset, static_cast<float>(value));
			} else {
				const auto value = vector_element<float>(input.value, offset);
				set_vector_element(result, offset, convert_to_i32(
					value, cpu.mxcsr, instruction.vector_prefix == 2));
			}
		}
	} else if (instruction.vector_prefix == 2) {
		for (std::size_t lane = 0;
			lane < destination_size / sizeof(double); ++lane) {
			const auto value = vector_element<std::int32_t>(
				input.value, lane * sizeof(std::int32_t));
			set_vector_element(result, lane * sizeof(double),
				static_cast<double>(value));
		}
	} else {
		for (std::size_t lane = 0;
			lane < source_size / sizeof(double); ++lane) {
			const auto value = vector_element<double>(
				input.value, lane * sizeof(double));
			set_vector_element(result, lane * sizeof(std::int32_t),
				convert_to_i32(value, cpu.mxcsr,
					instruction.vector_prefix == 1));
		}
	}
	const auto written = write_vector_operand(gadget, cpu, memory,
		instruction.destination, result, instruction.vex);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult vector_scalar_convert(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.vector_opcode == 0x2a) {
		const auto merge = read_vector_operand(
			gadget, cpu, memory, instruction.source);
		if (!merge.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, merge.fault};
		auto result = merge.value;
		if (instruction.vector_prefix >= 2) {
			const auto input = read_operand(
				gadget, cpu, memory, instruction.auxiliary);
			if (!input.succeeded)
				return read_failure(input);
			const auto signed_input = instruction.auxiliary.width ==
				OperandWidth::qword ?
				static_cast<long double>(static_cast<std::int64_t>(input.value)) :
				static_cast<long double>(static_cast<std::int32_t>(input.value));
			if (instruction.vector_prefix == 2)
				set_vector_element(result, 0,
					static_cast<float>(signed_input));
			else
				set_vector_element(result, 0,
					static_cast<double>(signed_input));
		} else {
			const auto input = read_vector_operand(
				gadget, cpu, memory, instruction.auxiliary);
			if (!input.succeeded)
				return {StepAction::exit,
					::uml::cpu::EngineEvent::memory_fault, input.fault};
			for (std::size_t lane = 0; lane < 2; ++lane) {
				const auto value = vector_element<std::int32_t>(
					input.value, lane * sizeof(std::int32_t));
				if (instruction.vector_prefix == 0)
					set_vector_element(result, lane * sizeof(float),
						static_cast<float>(value));
				else
					set_vector_element(result, lane * sizeof(double),
						static_cast<double>(value));
			}
		}
		const auto written = write_vector_operand(gadget, cpu, memory,
			instruction.destination, result, instruction.vex);
		if (written.action == StepAction::exit)
			return written;
		advance(gadget, cpu);
		return written;
	}

	const auto input = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!input.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			input.fault};
	const bool truncate = instruction.vector_opcode == 0x2c;
	if (instruction.vector_prefix >= 2) {
		const auto value = instruction.vector_prefix == 2 ?
			convert_to_integer(vector_element<float>(input.value, 0),
				instruction.destination.width, cpu.mxcsr, truncate) :
			convert_to_integer(vector_element<double>(input.value, 0),
				instruction.destination.width, cpu.mxcsr, truncate);
		const auto written = write_operand(gadget, cpu, memory,
			instruction.destination, value);
		if (written.action == StepAction::exit)
			return written;
		advance(gadget, cpu);
		return written;
	}
	std::array<std::byte, 32> result{};
	for (std::size_t lane = 0; lane < 2; ++lane) {
		const auto value = instruction.vector_prefix == 0 ?
			convert_to_integer(vector_element<float>(input.value,
				lane * sizeof(float)), OperandWidth::dword,
				cpu.mxcsr, truncate) :
			convert_to_integer(vector_element<double>(input.value,
				lane * sizeof(double)), OperandWidth::dword,
				cpu.mxcsr, truncate);
		set_vector_element(result, lane * sizeof(std::uint32_t),
			static_cast<std::uint32_t>(value));
	}
	const auto written = write_vector_operand(gadget, cpu, memory,
		instruction.destination, result, false);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult vector_compare_flags(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto left = read_vector_operand(
		gadget, cpu, memory, instruction.source);
	if (!left.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			left.fault};
	const auto right = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!right.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			right.fault};
	bool unordered{};
	bool equal{};
	bool less{};
	if (instruction.vector_prefix == 1) {
		const auto first = vector_element<double>(left.value, 0);
		const auto second = vector_element<double>(right.value, 0);
		unordered = std::isnan(first) || std::isnan(second);
		equal = first == second;
		less = first < second;
	} else {
		const auto first = vector_element<float>(left.value, 0);
		const auto second = vector_element<float>(right.value, 0);
		unordered = std::isnan(first) || std::isnan(second);
		equal = first == second;
		less = first < second;
	}
	materialize_flags(cpu);
	constexpr auto affected = rflags_carry | rflags_parity |
		rflags_auxiliary | rflags_zero | rflags_sign | rflags_overflow;
	cpu.rflags &= ~affected;
	if (unordered)
		cpu.rflags |= rflags_carry | rflags_parity | rflags_zero;
	else if (equal)
		cpu.rflags |= rflags_zero;
	else if (less)
		cpu.rflags |= rflags_carry;
	cpu.rflags |= rflags_reserved;
	cpu.deferred_flags.operation = DeferredFlagOperation::none;
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult vector_mask_store(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto data = read_vector_operand(
		gadget, cpu, memory, instruction.source);
	if (!data.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			data.fault};
	const auto mask = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!mask.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			mask.fault};
	const auto size = static_cast<std::size_t>(
		width_bytes(instruction.source.width));
	const auto address = cpu.gpr(Register::rdi);
	std::array<std::byte, 16> bytes{};
	if (!read_memory_bytes(memory, address,
		std::span{bytes}.first(size)))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, address, ::uml::memory::MemoryAccess::read)};
	for (std::size_t index = 0; index < size; ++index) {
		if ((std::to_integer<std::uint8_t>(mask.value[index]) & 0x80U) != 0)
			bytes[index] = data.value[index];
	}
	if (!write_memory_bytes(memory, address,
		std::span<const std::byte>{bytes}.first(size)))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, address, ::uml::memory::MemoryAccess::write)};
	memory.invalidate_code(address);
	advance(gadget, cpu);
	return {StepAction::return_to_dispatch,
		::uml::cpu::EngineEvent::continued, {}, address,
		static_cast<std::uint8_t>(size)};
}

[[nodiscard]] StepResult vector_gpr_move(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.vector_opcode == 0x7e &&
		instruction.vector_prefix == 2) {
		const auto source = read_vector_operand(
			gadget, cpu, memory, instruction.source);
		if (!source.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, source.fault};
		std::array<std::byte, 32> value{};
		std::copy_n(source.value.begin(), 8, value.begin());
		const auto written = write_vector_operand(gadget, cpu, memory,
			instruction.destination, value, instruction.vex);
		if (written.action == StepAction::exit)
			return written;
		advance(gadget, cpu);
		return written;
	}
	if (instruction.vector_opcode == 0x6e) {
		const auto source = read_operand(
			gadget, cpu, memory, instruction.source);
		if (!source.succeeded)
			return read_failure(source);
		std::array<std::byte, 32> value{};
		std::memcpy(value.data(), &source.value,
			width_bytes(instruction.source.width));
		const auto written = write_vector_operand(gadget, cpu, memory,
			instruction.destination, value, instruction.vex);
		if (written.action == StepAction::exit)
			return written;
		advance(gadget, cpu);
		return written;
	}
	const auto source = read_vector_operand(
		gadget, cpu, memory, instruction.source);
	if (!source.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			source.fault};
	std::uint64_t value{};
	std::memcpy(&value, source.value.data(),
		width_bytes(instruction.destination.width));
	const auto written = write_operand(gadget, cpu, memory,
		instruction.destination, value);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

template<typename Value, typename Function>
void vector_binary_elements(std::array<std::byte, 32> &result,
	const std::array<std::byte, 32> &left,
	const std::array<std::byte, 32> &right, std::size_t size,
	Function function) noexcept
{
	for (std::size_t offset = 0; offset < size; offset += sizeof(Value))
		set_vector_element(result, offset, function(
			vector_element<Value>(left, offset),
			vector_element<Value>(right, offset)));
}

template<typename Output, typename Input>
[[nodiscard]] constexpr Output saturate_signed(Input value) noexcept
{
	const auto minimum = static_cast<Input>(
		std::numeric_limits<Output>::min());
	const auto maximum = static_cast<Input>(
		std::numeric_limits<Output>::max());
	return static_cast<Output>(std::clamp(value, minimum, maximum));
}

template<typename Output, typename Input>
[[nodiscard]] constexpr Output saturate_unsigned(Input value) noexcept
{
	const auto maximum = static_cast<Input>(
		std::numeric_limits<Output>::max());
	return static_cast<Output>(std::clamp<Input>(value, 0, maximum));
}

template<typename Input, typename Output>
void vector_pack(std::array<std::byte, 32> &result,
	const std::array<std::byte, 32> &left,
	const std::array<std::byte, 32> &right, std::size_t size,
	bool unsigned_output) noexcept
{
	const auto lane_size = size == 8 ? 8U : 16U;
	const auto input_count = lane_size / sizeof(Input);
	for (std::size_t lane = 0; lane < size; lane += lane_size) {
		for (std::size_t index = 0; index < input_count; ++index) {
			const auto first = vector_element<Input>(
				left, lane + index * sizeof(Input));
			const auto second = vector_element<Input>(
				right, lane + index * sizeof(Input));
			const auto first_output = unsigned_output ?
				static_cast<Output>(saturate_unsigned<
					std::make_unsigned_t<Output>>(first)) :
				saturate_signed<Output>(first);
			const auto second_output = unsigned_output ?
				static_cast<Output>(saturate_unsigned<
					std::make_unsigned_t<Output>>(second)) :
				saturate_signed<Output>(second);
			set_vector_element(result,
				lane + index * sizeof(Output), first_output);
			set_vector_element(result,
				lane + (input_count + index) * sizeof(Output), second_output);
		}
	}
}

template<typename Value>
void vector_unpack(std::array<std::byte, 32> &result,
	const std::array<std::byte, 32> &left,
	const std::array<std::byte, 32> &right, std::size_t size,
	bool high) noexcept
{
	const auto lane_size = size == 8 ? 8U : 16U;
	const auto half_count = lane_size / sizeof(Value) / 2U;
	for (std::size_t lane = 0; lane < size; lane += lane_size) {
		const auto source = high ? half_count : 0U;
		for (std::size_t index = 0; index < half_count; ++index) {
			const auto output = lane + index * sizeof(Value) * 2U;
			set_vector_element(result, output,
				vector_element<Value>(left,
					lane + (source + index) * sizeof(Value)));
			set_vector_element(result, output + sizeof(Value),
				vector_element<Value>(right,
					lane + (source + index) * sizeof(Value)));
		}
	}
}

template<typename Value>
void vector_logical_shift(std::array<std::byte, 32> &result,
	const std::array<std::byte, 32> &left, std::size_t size,
	std::uint64_t count, bool left_shift) noexcept
{
	constexpr auto bits = sizeof(Value) * 8U;
	for (std::size_t offset = 0; offset < size; offset += sizeof(Value)) {
		const auto value = vector_element<Value>(left, offset);
		const auto shifted = count >= bits ? Value{0} :
			static_cast<Value>(left_shift ? value << count : value >> count);
		set_vector_element(result, offset, shifted);
	}
}

template<typename Value>
void vector_arithmetic_shift_right(std::array<std::byte, 32> &result,
	const std::array<std::byte, 32> &left, std::size_t size,
	std::uint64_t count) noexcept
{
	constexpr auto bits = sizeof(Value) * 8U;
	for (std::size_t offset = 0; offset < size; offset += sizeof(Value)) {
		const auto value = vector_element<Value>(left, offset);
		const auto shifted = count >= bits ?
			(value < 0 ? Value{-1} : Value{0}) :
			static_cast<Value>(value >> count);
		set_vector_element(result, offset, shifted);
	}
}

template<typename Value>
void vector_horizontal(std::array<std::byte, 32> &result,
	const std::array<std::byte, 32> &left,
	const std::array<std::byte, 32> &right, std::size_t size,
	bool subtract, bool saturated) noexcept
{
	using Unsigned = std::make_unsigned_t<Value>;
	const auto lane_size = size == 8 ? 8U : 16U;
	const auto count = lane_size / sizeof(Value);
	const auto half = count / 2U;
	for (std::size_t lane = 0; lane < size; lane += lane_size) {
		for (std::size_t source_index = 0; source_index < 2; ++source_index) {
			const auto &source = source_index == 0 ? left : right;
			for (std::size_t index = 0; index < half; ++index) {
				const auto first = vector_element<Value>(source,
					lane + index * 2U * sizeof(Value));
				const auto second = vector_element<Value>(source,
					lane + (index * 2U + 1U) * sizeof(Value));
				Value value{};
				if (saturated) {
					const auto wide = subtract ?
						static_cast<std::int64_t>(first) - second :
						static_cast<std::int64_t>(first) + second;
					value = saturate_signed<Value>(wide);
				} else {
					const auto first_unsigned = static_cast<Unsigned>(first);
					const auto second_unsigned = static_cast<Unsigned>(second);
					value = static_cast<Value>(subtract ?
						first_unsigned - second_unsigned :
						first_unsigned + second_unsigned);
				}
				set_vector_element(result,
					lane + (source_index * half + index) * sizeof(Value),
					value);
			}
		}
	}
}

template<typename Value>
void vector_sign(std::array<std::byte, 32> &result,
	const std::array<std::byte, 32> &left,
	const std::array<std::byte, 32> &right, std::size_t size) noexcept
{
	using Unsigned = std::make_unsigned_t<Value>;
	for (std::size_t offset = 0; offset < size; offset += sizeof(Value)) {
		const auto value = vector_element<Value>(left, offset);
		const auto sign = vector_element<Value>(right, offset);
		const auto output = sign == 0 ? Value{0} :
			(sign < 0 ? static_cast<Value>(Unsigned{0} -
				static_cast<Unsigned>(value)) : value);
		set_vector_element(result, offset, output);
	}
}

template<typename Value>
void vector_absolute(std::array<std::byte, 32> &result,
	const std::array<std::byte, 32> &input, std::size_t size) noexcept
{
	using Unsigned = std::make_unsigned_t<Value>;
	for (std::size_t offset = 0; offset < size; offset += sizeof(Value)) {
		const auto value = vector_element<Value>(input, offset);
		const auto output = value < 0 ? Unsigned{0} -
			static_cast<Unsigned>(value) : static_cast<Unsigned>(value);
		set_vector_element(result, offset, output);
	}
}

template<typename Output, typename Input>
void vector_extend(std::array<std::byte, 32> &result,
	const std::array<std::byte, 32> &input, std::size_t size) noexcept
{
	for (std::size_t index = 0; index < size / sizeof(Output); ++index) {
		const auto value = vector_element<Input>(
			input, index * sizeof(Input));
		set_vector_element(result, index * sizeof(Output),
			static_cast<Output>(value));
	}
}

template<typename Value, typename Compare>
void vector_minmax(std::array<std::byte, 32> &result,
	const std::array<std::byte, 32> &left,
	const std::array<std::byte, 32> &right, std::size_t size,
	Compare compare) noexcept
{
	vector_binary_elements<Value>(result, left, right, size,
		[compare](Value first, Value second) {
			return compare(first, second) ? first : second;
	});
}

[[nodiscard]] constexpr std::uint8_t aes_multiply(
	std::uint8_t left, std::uint8_t right) noexcept
{
	std::uint8_t result{};
	for (unsigned bit = 0; bit < 8; ++bit) {
		if ((right & 1U) != 0)
			result ^= left;
		const bool high = (left & 0x80U) != 0;
		left = static_cast<std::uint8_t>(left << 1U);
		if (high)
			left ^= 0x1bU;
		right = static_cast<std::uint8_t>(right >> 1U);
	}
	return result;
}

[[nodiscard]] constexpr std::uint8_t aes_rotate(
	std::uint8_t value, unsigned count) noexcept
{
	return static_cast<std::uint8_t>((value << count) |
		(value >> (8U - count)));
}

[[nodiscard]] constexpr std::uint8_t aes_sbox(std::uint8_t value) noexcept
{
	std::uint8_t inverse = value == 0 ? 0 : 1;
	if (value != 0) {
		std::uint8_t base = value;
		unsigned exponent = 254;
		while (exponent != 0) {
			if ((exponent & 1U) != 0)
				inverse = aes_multiply(inverse, base);
			base = aes_multiply(base, base);
			exponent >>= 1U;
		}
	}
	return static_cast<std::uint8_t>(inverse ^ aes_rotate(inverse, 1) ^
		aes_rotate(inverse, 2) ^ aes_rotate(inverse, 3) ^
		aes_rotate(inverse, 4) ^ 0x63U);
}

[[nodiscard]] constexpr std::uint8_t aes_inverse_sbox(
	std::uint8_t value) noexcept
{
	for (unsigned candidate = 0; candidate < 256; ++candidate) {
		if (aes_sbox(static_cast<std::uint8_t>(candidate)) == value)
			return static_cast<std::uint8_t>(candidate);
	}
	return 0;
}

void aes_mix_columns(std::array<std::byte, 16> &state,
	bool inverse) noexcept
{
	for (std::size_t column = 0; column < 4; ++column) {
		std::array<std::uint8_t, 4> input{};
		for (std::size_t row = 0; row < 4; ++row)
			input[row] = std::to_integer<std::uint8_t>(
				state[column * 4U + row]);
		std::array<std::uint8_t, 4> output{};
		if (inverse) {
			output[0] = aes_multiply(input[0], 14) ^
				aes_multiply(input[1], 11) ^
				aes_multiply(input[2], 13) ^ aes_multiply(input[3], 9);
			output[1] = aes_multiply(input[0], 9) ^
				aes_multiply(input[1], 14) ^
				aes_multiply(input[2], 11) ^ aes_multiply(input[3], 13);
			output[2] = aes_multiply(input[0], 13) ^
				aes_multiply(input[1], 9) ^
				aes_multiply(input[2], 14) ^ aes_multiply(input[3], 11);
			output[3] = aes_multiply(input[0], 11) ^
				aes_multiply(input[1], 13) ^
				aes_multiply(input[2], 9) ^ aes_multiply(input[3], 14);
		} else {
			output[0] = aes_multiply(input[0], 2) ^
				aes_multiply(input[1], 3) ^ input[2] ^ input[3];
			output[1] = input[0] ^ aes_multiply(input[1], 2) ^
				aes_multiply(input[2], 3) ^ input[3];
			output[2] = input[0] ^ input[1] ^
				aes_multiply(input[2], 2) ^ aes_multiply(input[3], 3);
			output[3] = aes_multiply(input[0], 3) ^ input[1] ^
				input[2] ^ aes_multiply(input[3], 2);
		}
		for (std::size_t row = 0; row < 4; ++row)
			state[column * 4U + row] = static_cast<std::byte>(output[row]);
	}
}

[[nodiscard]] StepResult vector_aes(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	VectorRead state{true, {}, {}};
	if (instruction.source.kind != OperandKind::none) {
		state = read_vector_operand(gadget, cpu, memory, instruction.source);
		if (!state.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, state.fault};
	}
	const auto key = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!key.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			key.fault};
	std::array<std::byte, 32> result{};
	const auto size = static_cast<std::size_t>(
		width_bytes(instruction.destination.width));
	for (std::size_t lane = 0; lane < size; lane += 16) {
		std::array<std::byte, 16> block{};
		if (instruction.vector_opcode == 0xdb) {
			std::copy_n(key.value.begin() +
				static_cast<std::ptrdiff_t>(lane), 16, block.begin());
			aes_mix_columns(block, true);
		} else {
			const bool decrypt = instruction.vector_opcode >= 0xde;
			for (std::size_t column = 0; column < 4; ++column) {
				for (std::size_t row = 0; row < 4; ++row) {
					const auto source_column = decrypt ?
						(column + 4U - row) & 3U :
						(column + row) & 3U;
					const auto value = std::to_integer<std::uint8_t>(
						state.value[lane + source_column * 4U + row]);
					block[column * 4U + row] = static_cast<std::byte>(
						decrypt ? aes_inverse_sbox(value) : aes_sbox(value));
				}
			}
			const bool last = instruction.vector_opcode == 0xdd ||
				instruction.vector_opcode == 0xdf;
			if (!last)
				aes_mix_columns(block, decrypt);
			for (std::size_t index = 0; index < 16; ++index)
				block[index] ^= key.value[lane + index];
		}
		std::copy(block.begin(), block.end(), result.begin() +
			static_cast<std::ptrdiff_t>(lane));
	}
	const auto written = write_vector_operand(gadget, cpu, memory,
		instruction.destination, result, instruction.vex);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] constexpr std::uint32_t sha256_sigma0(
	std::uint32_t value) noexcept
{
	return std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3U);
}

[[nodiscard]] constexpr std::uint32_t sha256_sigma1(
	std::uint32_t value) noexcept
{
	return std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10U);
}

[[nodiscard]] constexpr std::uint32_t sha256_sum0(
	std::uint32_t value) noexcept
{
	return std::rotr(value, 2) ^ std::rotr(value, 13) ^
		std::rotr(value, 22);
}

[[nodiscard]] constexpr std::uint32_t sha256_sum1(
	std::uint32_t value) noexcept
{
	return std::rotr(value, 6) ^ std::rotr(value, 11) ^
		std::rotr(value, 25);
}

[[nodiscard]] StepResult vector_sha(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto first = read_vector_operand(
		gadget, cpu, memory, instruction.source);
	if (!first.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			first.fault};
	const auto second = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!second.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			second.fault};
	std::array<std::uint32_t, 4> left{};
	std::array<std::uint32_t, 4> right{};
	std::array<std::uint32_t, 4> output{};
	for (std::size_t index = 0; index < 4; ++index) {
		left[index] = vector_element<std::uint32_t>(
			first.value, index * sizeof(std::uint32_t));
		right[index] = vector_element<std::uint32_t>(
			second.value, index * sizeof(std::uint32_t));
	}
	switch (instruction.vector_opcode) {
	case 0xc8:
		output = right;
		output[3] += std::rotl(left[3], 30);
		break;
	case 0xc9:
		output[3] = left[1] ^ left[3];
		output[2] = left[0] ^ left[2];
		output[1] = right[3] ^ left[1];
		output[0] = right[2] ^ left[0];
		break;
	case 0xca:
		output[3] = std::rotl(left[3] ^ right[2], 1);
		output[2] = std::rotl(left[2] ^ right[1], 1);
		output[1] = std::rotl(left[1] ^ right[0], 1);
		output[0] = std::rotl(left[0] ^ output[3], 1);
		break;
	case 0xcb: {
		auto a = right[3];
		auto b = right[2];
		auto c = left[3];
		auto d = left[2];
		auto e = right[1];
		auto f = right[0];
		auto g = left[1];
		auto h = left[0];
		for (std::size_t round = 0; round < 2; ++round) {
			const auto choose = (e & f) ^ (~e & g);
			const auto majority = (a & b) ^ (a & c) ^ (b & c);
			const auto temporary = choose + sha256_sum1(e) +
				vector_element<std::uint32_t>(cpu.vectors[0],
					round * sizeof(std::uint32_t)) + h;
			const auto next_a = temporary + majority + sha256_sum0(a);
			const auto next_e = temporary + d;
			h = g;
			g = f;
			f = e;
			e = next_e;
			d = c;
			c = b;
			b = a;
			a = next_a;
		}
		output[3] = a;
		output[2] = b;
		output[1] = e;
		output[0] = f;
		break;
	}
	case 0xcc:
		output[3] = left[3] + sha256_sigma0(right[0]);
		output[2] = left[2] + sha256_sigma0(left[3]);
		output[1] = left[1] + sha256_sigma0(left[2]);
		output[0] = left[0] + sha256_sigma0(left[1]);
		break;
	case 0xcd:
		output[0] = left[0] + sha256_sigma1(right[2]);
		output[1] = left[1] + sha256_sigma1(right[3]);
		output[2] = left[2] + sha256_sigma1(output[0]);
		output[3] = left[3] + sha256_sigma1(output[1]);
		break;
	default:
		return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
	}
	std::array<std::byte, 32> result{};
	for (std::size_t index = 0; index < 4; ++index)
		set_vector_element(result, index * sizeof(std::uint32_t),
			output[index]);
	const auto written = write_vector_operand(gadget, cpu, memory,
		instruction.destination, result, false);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult vector_integer_map2(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	VectorRead left{true, {}, {}};
	if (instruction.source.kind != OperandKind::none) {
		left = read_vector_operand(gadget, cpu, memory, instruction.source);
		if (!left.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, left.fault};
	}
	const auto right = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!right.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			right.fault};
	std::array<std::byte, 32> result{};
	const auto size = static_cast<std::size_t>(
		width_bytes(instruction.destination.width));
	const auto opcode = instruction.vector_opcode;
	switch (opcode) {
	case 0x00: {
		const auto lane_size = size == 8 ? 8U : 16U;
		for (std::size_t lane = 0; lane < size; lane += lane_size) {
			for (std::size_t index = 0; index < lane_size; ++index) {
				const auto selector = std::to_integer<std::uint8_t>(
					right.value[lane + index]);
				result[lane + index] = (selector & 0x80U) != 0 ?
					std::byte{} : left.value[lane +
						(selector & static_cast<std::uint8_t>(lane_size - 1U))];
			}
		}
		break;
	}
	case 0x01: vector_horizontal<std::int16_t>(result,
		left.value, right.value, size, false, false); break;
	case 0x02: vector_horizontal<std::int32_t>(result,
		left.value, right.value, size, false, false); break;
	case 0x03: vector_horizontal<std::int16_t>(result,
		left.value, right.value, size, false, true); break;
	case 0x04:
		for (std::size_t offset = 0; offset < size; offset += 2) {
			const auto first =
				static_cast<std::int32_t>(vector_element<std::uint8_t>(
					left.value, offset)) *
				vector_element<std::int8_t>(right.value, offset);
			const auto second =
				static_cast<std::int32_t>(vector_element<std::uint8_t>(
					left.value, offset + 1)) *
				vector_element<std::int8_t>(right.value, offset + 1);
			set_vector_element(result, offset,
				saturate_signed<std::int16_t>(first + second));
		}
		break;
	case 0x05: vector_horizontal<std::int16_t>(result,
		left.value, right.value, size, true, false); break;
	case 0x06: vector_horizontal<std::int32_t>(result,
		left.value, right.value, size, true, false); break;
	case 0x07: vector_horizontal<std::int16_t>(result,
		left.value, right.value, size, true, true); break;
	case 0x08: vector_sign<std::int8_t>(result,
		left.value, right.value, size); break;
	case 0x09: vector_sign<std::int16_t>(result,
		left.value, right.value, size); break;
	case 0x0a: vector_sign<std::int32_t>(result,
		left.value, right.value, size); break;
	case 0x0b:
		for (std::size_t offset = 0; offset < size; offset += 2) {
			const auto product =
				static_cast<std::int32_t>(vector_element<std::int16_t>(
					left.value, offset)) *
				vector_element<std::int16_t>(right.value, offset);
			set_vector_element(result, offset,
				static_cast<std::int16_t>((product + 0x4000) >> 15));
		}
		break;
	case 0x10:
	case 0x14:
	case 0x15: {
		const auto &mask = cpu.vectors[0];
		const auto element_size = opcode == 0x10 ? 1U :
			(opcode == 0x14 ? 4U : 8U);
		for (std::size_t offset = 0; offset < size; offset += element_size) {
			const auto selected = (std::to_integer<std::uint8_t>(
				mask[offset + element_size - 1U]) & 0x80U) != 0;
			std::copy_n((selected ? right.value : left.value).begin() +
				static_cast<std::ptrdiff_t>(offset), element_size,
				result.begin() + static_cast<std::ptrdiff_t>(offset));
		}
		break;
	}
	case 0x1c: vector_absolute<std::int8_t>(result, right.value, size); break;
	case 0x1d: vector_absolute<std::int16_t>(result, right.value, size); break;
	case 0x1e: vector_absolute<std::int32_t>(result, right.value, size); break;
	case 0x20: vector_extend<std::int16_t, std::int8_t>(
		result, right.value, size); break;
	case 0x21: vector_extend<std::int32_t, std::int8_t>(
		result, right.value, size); break;
	case 0x22: vector_extend<std::int64_t, std::int8_t>(
		result, right.value, size); break;
	case 0x23: vector_extend<std::int32_t, std::int16_t>(
		result, right.value, size); break;
	case 0x24: vector_extend<std::int64_t, std::int16_t>(
		result, right.value, size); break;
	case 0x25: vector_extend<std::int64_t, std::int32_t>(
		result, right.value, size); break;
	case 0x28:
		for (std::size_t lane = 0; lane < size; lane += 16) {
			for (std::size_t index = 0; index < 2; ++index) {
				const auto offset = lane + index * 8U;
				const auto first = vector_element<std::int32_t>(
					left.value, offset);
				const auto second = vector_element<std::int32_t>(
					right.value, offset);
				set_vector_element(result, offset,
					static_cast<std::int64_t>(first) * second);
			}
		}
		break;
	case 0x29: vector_binary_elements<std::uint64_t>(result,
		left.value, right.value, size, [](auto first, auto second) {
			return first == second ? ~std::uint64_t{0} : 0; }); break;
	case 0x2b: vector_pack<std::int32_t, std::int16_t>(result,
		left.value, right.value, size, true); break;
	case 0x30: vector_extend<std::uint16_t, std::uint8_t>(
		result, right.value, size); break;
	case 0x31: vector_extend<std::uint32_t, std::uint8_t>(
		result, right.value, size); break;
	case 0x32: vector_extend<std::uint64_t, std::uint8_t>(
		result, right.value, size); break;
	case 0x33: vector_extend<std::uint32_t, std::uint16_t>(
		result, right.value, size); break;
	case 0x34: vector_extend<std::uint64_t, std::uint16_t>(
		result, right.value, size); break;
	case 0x35: vector_extend<std::uint64_t, std::uint32_t>(
		result, right.value, size); break;
	case 0x37: vector_binary_elements<std::int64_t>(result,
		left.value, right.value, size, [](auto first, auto second) {
			return first > second ? std::int64_t{-1} : 0; }); break;
	case 0x38: vector_minmax<std::int8_t>(result,
		left.value, right.value, size, std::less<>{}); break;
	case 0x39: vector_minmax<std::int32_t>(result,
		left.value, right.value, size, std::less<>{}); break;
	case 0x3a: vector_minmax<std::uint16_t>(result,
		left.value, right.value, size, std::less<>{}); break;
	case 0x3b: vector_minmax<std::uint32_t>(result,
		left.value, right.value, size, std::less<>{}); break;
	case 0x3c: vector_minmax<std::int8_t>(result,
		left.value, right.value, size, std::greater<>{}); break;
	case 0x3d: vector_minmax<std::int32_t>(result,
		left.value, right.value, size, std::greater<>{}); break;
	case 0x3e: vector_minmax<std::uint16_t>(result,
		left.value, right.value, size, std::greater<>{}); break;
	case 0x3f: vector_minmax<std::uint32_t>(result,
		left.value, right.value, size, std::greater<>{}); break;
	case 0x40:
		vector_binary_elements<std::uint32_t>(result,
			left.value, right.value, size, [](auto first, auto second) {
				return static_cast<std::uint32_t>(
					static_cast<std::uint64_t>(first) * second); });
		break;
	case 0x41: {
		auto minimum = std::numeric_limits<std::uint16_t>::max();
		std::uint16_t minimum_index{};
		for (std::uint16_t index = 0; index < 8; ++index) {
			const auto value = vector_element<std::uint16_t>(
				right.value, index * sizeof(std::uint16_t));
			if (value < minimum) {
				minimum = value;
				minimum_index = index;
			}
		}
		set_vector_element(result, 0, minimum);
		set_vector_element(result, 2, minimum_index);
		break;
	}
	default:
		return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
	}
	const auto written = write_vector_operand(gadget, cpu, memory,
		instruction.destination, result, instruction.vex);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

template<typename Float>
[[nodiscard]] Float round_vector_element(Float value,
	std::uint8_t control, std::uint32_t mxcsr) noexcept
{
	if (!std::isfinite(value))
		return value;
	const auto mode = (control & 4U) != 0 ?
		static_cast<std::uint8_t>((mxcsr >> 13U) & 3U) :
		static_cast<std::uint8_t>(control & 3U);
	Float result{};
	switch (mode) {
	case 0: {
		const auto lower = std::floor(value);
		const auto fraction = value - lower;
		if (fraction < Float{0.5})
			result = lower;
		else if (fraction > Float{0.5})
			result = lower + Float{1};
		else
			result = std::fmod(lower, Float{2}) == Float{0} ?
				lower : lower + Float{1};
		break;
	}
	case 1: result = std::floor(value); break;
	case 2: result = std::ceil(value); break;
	default: result = std::trunc(value); break;
	}
	return result == Float{0} ? std::copysign(Float{0}, value) : result;
}

template<typename Element>
void vector_immediate_blend(std::array<std::byte, 32> &result,
	const std::array<std::byte, 32> &left,
	const std::array<std::byte, 32> &right, std::size_t size,
	std::uint8_t control, bool repeat_mask) noexcept
{
	for (std::size_t offset = 0; offset < size; offset += sizeof(Element)) {
		const auto index = offset / sizeof(Element);
		const auto bit = repeat_mask ? index % (16U / sizeof(Element)) : index;
		const auto &input = ((control >> bit) & 1U) != 0 ? right : left;
		std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset),
			sizeof(Element), result.begin() + static_cast<std::ptrdiff_t>(offset));
	}
}

template<typename Float>
[[nodiscard]] Float strict_float_multiply(Float left, Float right) noexcept
{
	volatile Float result = left * right;
	return result;
}

template<typename Float>
[[nodiscard]] Float strict_float_add(Float left, Float right) noexcept
{
	volatile Float result = left + right;
	return result;
}

[[nodiscard]] std::size_t packed_string_length(std::uint64_t value,
	bool wide_length, std::size_t maximum) noexcept
{
	std::uint64_t magnitude{};
	if (wide_length) {
		const auto signed_value = static_cast<std::int64_t>(value);
		magnitude = signed_value < 0 ?
			static_cast<std::uint64_t>(-(signed_value + 1)) + 1U :
			static_cast<std::uint64_t>(signed_value);
	} else {
		const auto signed_value = static_cast<std::int32_t>(value);
		magnitude = signed_value < 0 ?
			static_cast<std::uint64_t>(-(static_cast<std::int64_t>(signed_value) +
				1)) + 1U : static_cast<std::uint64_t>(signed_value);
	}
	return static_cast<std::size_t>(std::min<std::uint64_t>(magnitude, maximum));
}

[[nodiscard]] std::int64_t packed_string_element(
	const std::array<std::byte, 32> &value, std::size_t index,
	std::size_t element_size, bool signed_elements) noexcept
{
	if (element_size == 1) {
		const auto element = vector_element<std::uint8_t>(value, index);
		return signed_elements ? static_cast<std::int8_t>(element) : element;
	}
	const auto element = vector_element<std::uint16_t>(value, index * 2U);
	return signed_elements ? static_cast<std::int16_t>(element) : element;
}

[[nodiscard]] StepResult vector_string_compare(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto left = read_vector_operand(
		gadget, cpu, memory, instruction.source);
	if (!left.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			left.fault};
	const auto right = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!right.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			right.fault};

	const auto control = instruction.vector_control;
	const auto element_size = (control & 1U) != 0 ? 2U : 1U;
	const auto count = 16U / element_size;
	const bool explicit_lengths = instruction.vector_opcode <= 0x61;
	std::size_t left_length{};
	std::size_t right_length{};
	if (explicit_lengths) {
		const bool wide_length = instruction.vector_group != 0;
		left_length = packed_string_length(
			cpu.gpr(Register::rax), wide_length, count);
		right_length = packed_string_length(
			cpu.gpr(Register::rdx), wide_length, count);
	} else {
		left_length = count;
		right_length = count;
		for (std::size_t index = 0; index < count; ++index) {
			if (left_length == count && packed_string_element(
				left.value, index, element_size, false) == 0)
				left_length = index;
			if (right_length == count && packed_string_element(
				right.value, index, element_size, false) == 0)
				right_length = index;
		}
	}

	const auto aggregation = static_cast<std::uint8_t>((control >> 2U) & 3U);
	const bool signed_elements = (control & 2U) != 0;
	const auto comparison = [&](std::size_t right_index,
		std::size_t left_index) noexcept {
		const bool left_valid = left_index < left_length;
		const bool right_valid = right_index < right_length;
		if (aggregation <= 1)
			return left_valid && right_valid &&
				(aggregation == 0 ?
					packed_string_element(left.value, left_index, element_size,
						signed_elements) ==
						packed_string_element(right.value, right_index,
							element_size, signed_elements) :
					((left_index & 1U) == 0 ?
						packed_string_element(right.value, right_index,
							element_size, signed_elements) >=
							packed_string_element(left.value, left_index,
								element_size, signed_elements) :
						packed_string_element(right.value, right_index,
							element_size, signed_elements) <=
							packed_string_element(left.value, left_index,
								element_size, signed_elements)));
		if (aggregation == 2) {
			if (!left_valid || !right_valid)
				return !left_valid && !right_valid;
		} else {
			if (!left_valid)
				return true;
			if (!right_valid)
				return false;
		}
		return packed_string_element(left.value, left_index, element_size,
			signed_elements) == packed_string_element(right.value, right_index,
				element_size, signed_elements);
	};

	std::uint32_t first_result{};
	if (aggregation == 0) {
		for (std::size_t right_index = 0; right_index < count; ++right_index) {
			for (std::size_t left_index = 0; left_index < count; ++left_index) {
				if (comparison(right_index, left_index)) {
					first_result |= 1U << right_index;
					break;
				}
			}
		}
	} else if (aggregation == 1) {
		for (std::size_t right_index = 0; right_index < count; ++right_index) {
			for (std::size_t left_index = 0; left_index < count;
				left_index += 2) {
				if (comparison(right_index, left_index) &&
					comparison(right_index, left_index + 1U)) {
					first_result |= 1U << right_index;
					break;
				}
			}
		}
	} else if (aggregation == 2) {
		for (std::size_t index = 0; index < count; ++index) {
			if (comparison(index, index))
				first_result |= 1U << index;
		}
	} else {
		for (std::size_t start = 0; start < count; ++start) {
			bool matches = true;
			for (std::size_t left_index = 0;
				left_index + start < count; ++left_index)
				matches = matches && comparison(
					start + left_index, left_index);
			if (matches)
				first_result |= 1U << start;
		}
	}

	const auto result_mask = static_cast<std::uint32_t>((1U << count) - 1U);
	first_result &= result_mask;
	auto second_result = first_result;
	if ((control & 0x10U) != 0) {
		if ((control & 0x20U) == 0) {
			second_result = (~first_result) & result_mask;
		} else {
			const auto valid_mask = right_length == count ? result_mask :
				((1U << right_length) - 1U);
			second_result = first_result ^ valid_mask;
		}
	}

	materialize_flags(cpu);
	constexpr auto affected = rflags_carry | rflags_parity |
		rflags_auxiliary | rflags_zero | rflags_sign | rflags_overflow;
	cpu.rflags &= ~affected;
	if (second_result != 0)
		cpu.rflags |= rflags_carry;
	if (right_length < count)
		cpu.rflags |= rflags_zero;
	if (left_length < count)
		cpu.rflags |= rflags_sign;
	if ((second_result & 1U) != 0)
		cpu.rflags |= rflags_overflow;
	cpu.rflags |= rflags_reserved;
	cpu.deferred_flags.operation = DeferredFlagOperation::none;

	const bool return_mask = (instruction.vector_opcode & 1U) == 0;
	if (return_mask) {
		std::array<std::byte, 32> result{};
		if ((control & 0x40U) == 0) {
			set_vector_element(result, 0,
				static_cast<std::uint16_t>(second_result));
		} else {
			for (std::size_t index = 0; index < count; ++index) {
				if (((second_result >> index) & 1U) == 0)
					continue;
				if (element_size == 1)
					set_vector_element(result, index, std::uint8_t{0xff});
				else
					set_vector_element(result, index * 2U,
						std::uint16_t{0xffff});
			}
		}
		Operand destination{};
		destination.kind = OperandKind::vector_reg;
		destination.width = OperandWidth::xmm;
		destination.reg.index = 0;
		const auto written = write_vector_operand(gadget, cpu, memory,
			destination, result, instruction.vex);
		if (written.action == StepAction::exit)
			return written;
	} else {
		std::uint64_t index = count;
		if (second_result != 0) {
			index = (control & 0x40U) == 0 ?
				std::countr_zero(second_result) :
				31U - std::countl_zero(second_result);
		}
		write_gpr(cpu, static_cast<std::uint8_t>(Register::rcx),
			instruction.vector_group != 0 ? OperandWidth::qword :
				OperandWidth::dword, index);
	}
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] std::uint16_t float_to_half(float value,
	std::uint8_t control, std::uint32_t mxcsr) noexcept
{
	const auto bits = std::bit_cast<std::uint32_t>(value);
	const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
	const auto exponent = static_cast<unsigned>((bits >> 23U) & 0xffU);
	const auto mantissa = bits & 0x7fffffU;
	if (exponent == 0xffU) {
		if (mantissa == 0)
			return static_cast<std::uint16_t>(sign | 0x7c00U);
		auto payload = static_cast<std::uint16_t>(mantissa >> 13U);
		payload = static_cast<std::uint16_t>(payload | 0x0200U);
		return static_cast<std::uint16_t>(sign | 0x7c00U | payload);
	}

	const auto mode = (control & 4U) != 0 ?
		static_cast<std::uint8_t>((mxcsr >> 13U) & 3U) :
		static_cast<std::uint8_t>(control & 3U);
	const bool negative = sign != 0;
	const auto overflow = [&]() noexcept {
		const bool infinity = mode == 0 ||
			(mode == 1 && negative) || (mode == 2 && !negative);
		return static_cast<std::uint16_t>(sign |
			(infinity ? 0x7c00U : 0x7bffU));
	};
	if (exponent == 0)
		return static_cast<std::uint16_t>(sign |
			((mantissa != 0 && ((mode == 1 && negative) ||
				(mode == 2 && !negative))) ? 1U : 0U));

	const auto half_exponent = static_cast<int>(exponent) - 112;
	if (half_exponent >= 31)
		return overflow();

	std::uint32_t base{};
	std::uint32_t remainder{};
	std::uint32_t halfway{};
	if (half_exponent <= 0) {
		if (half_exponent < -10) {
			remainder = mantissa | 0x800000U;
			halfway = std::numeric_limits<std::uint32_t>::max();
		} else {
			const auto shift = static_cast<unsigned>(14 - half_exponent);
			const auto significand = mantissa | 0x800000U;
			base = significand >> shift;
			remainder = significand & ((1U << shift) - 1U);
			halfway = 1U << (shift - 1U);
		}
	} else {
		base = (static_cast<std::uint32_t>(half_exponent) << 10U) |
			(mantissa >> 13U);
		remainder = mantissa & 0x1fffU;
		halfway = 0x1000U;
	}

	bool increment{};
	if (mode == 0)
		increment = remainder > halfway ||
			(remainder == halfway && (base & 1U) != 0);
	else if (mode == 1)
		increment = negative && remainder != 0;
	else if (mode == 2)
		increment = !negative && remainder != 0;
	if (increment)
		++base;
	if (base >= 0x7c00U)
		return overflow();
	return static_cast<std::uint16_t>(sign | base);
}

[[nodiscard]] StepResult vector_integer_map3(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto opcode = instruction.vector_opcode;
	const auto control = instruction.vector_control;

	if (opcode >= 0x14 && opcode <= 0x17) {
		const auto input = read_vector_operand(
			gadget, cpu, memory, instruction.source);
		if (!input.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, input.fault};
		std::size_t element_size = 4;
		if (opcode == 0x14)
			element_size = 1;
		else if (opcode == 0x15)
			element_size = 2;
		else if (opcode == 0x16 &&
			instruction.destination.width == OperandWidth::qword)
			element_size = 8;
		const auto index = static_cast<std::size_t>(control) %
			(16U / element_size);
		std::uint64_t value{};
		std::memcpy(&value, input.value.data() + index * element_size,
			element_size);
		const auto written = write_operand(
			gadget, cpu, memory, instruction.destination, value);
		if (written.action == StepAction::exit)
			return written;
		advance(gadget, cpu);
		return written;
	}

	if (opcode == 0x19 || opcode == 0x39) {
		const auto input = read_vector_operand(
			gadget, cpu, memory, instruction.source);
		if (!input.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, input.fault};
		std::array<std::byte, 32> result{};
		std::copy_n(input.value.begin() +
			static_cast<std::ptrdiff_t>((control & 1U) * 16U), 16,
			result.begin());
		const auto written = write_vector_operand(gadget, cpu, memory,
			instruction.destination, result, instruction.vex);
		if (written.action == StepAction::exit)
			return written;
		advance(gadget, cpu);
		return written;
	}

	if (opcode == 0x1d) {
		const auto input = read_vector_operand(
			gadget, cpu, memory, instruction.source);
		if (!input.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, input.fault};
		std::array<std::byte, 32> result{};
		const auto count = static_cast<std::size_t>(
			width_bytes(instruction.source.width)) / sizeof(float);
		for (std::size_t index = 0; index < count; ++index)
			set_vector_element(result, index * sizeof(std::uint16_t),
				float_to_half(vector_element<float>(input.value,
					index * sizeof(float)), control, cpu.mxcsr));
		const auto written = write_vector_operand(gadget, cpu, memory,
			instruction.destination, result, true);
		if (written.action == StepAction::exit)
			return written;
		advance(gadget, cpu);
		return written;
	}

	if (opcode == 0x20 || opcode == 0x22) {
		const auto merge = read_vector_operand(
			gadget, cpu, memory, instruction.source);
		if (!merge.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, merge.fault};
		const auto input = read_operand(
			gadget, cpu, memory, instruction.auxiliary);
		if (!input.succeeded)
			return read_failure(input);
		auto result = merge.value;
		const auto element_size = opcode == 0x20 ? 1U :
			(instruction.auxiliary.width == OperandWidth::qword ? 8U : 4U);
		const auto index = static_cast<std::size_t>(control) %
			(16U / element_size);
		std::memcpy(result.data() + index * element_size, &input.value,
			element_size);
		const auto written = write_vector_operand(gadget, cpu, memory,
			instruction.destination, result, instruction.vex);
		if (written.action == StepAction::exit)
			return written;
		advance(gadget, cpu);
		return written;
	}

	if (opcode >= 0x60 && opcode <= 0x63)
		return vector_string_compare(gadget, cpu, memory);

	VectorRead left{true, {}, {}};
	if (instruction.source.kind != OperandKind::none) {
		left = read_vector_operand(gadget, cpu, memory, instruction.source);
		if (!left.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, left.fault};
	}
	const auto right = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!right.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			right.fault};
	std::array<std::byte, 32> result{};
	const auto size = static_cast<std::size_t>(
		width_bytes(instruction.destination.width));

	switch (opcode) {
	case 0x00:
	case 0x01:
		for (std::size_t index = 0; index < 4; ++index) {
			const auto selected = (control >> (index * 2U)) & 3U;
			set_vector_element(result, index * 8U,
				vector_element<std::uint64_t>(right.value, selected * 8U));
		}
		break;
	case 0x02:
		vector_immediate_blend<std::uint32_t>(result, left.value,
			right.value, size, control, false);
		break;
	case 0x04:
		for (std::size_t lane = 0; lane < size; lane += 16) {
			for (std::size_t index = 0; index < 4; ++index) {
				const auto selected = (control >> (index * 2U)) & 3U;
				set_vector_element(result, lane + index * 4U,
					vector_element<std::uint32_t>(right.value,
						lane + selected * 4U));
			}
		}
		break;
	case 0x05:
		for (std::size_t lane = 0; lane < size; lane += 16) {
			for (std::size_t index = 0; index < 2; ++index) {
				const auto output_index = lane / 8U + index;
				const auto selected = (control >> output_index) & 1U;
				set_vector_element(result, lane + index * 8U,
					vector_element<std::uint64_t>(right.value,
						lane + selected * 8U));
			}
		}
		break;
	case 0x06:
	case 0x46:
		for (std::size_t lane = 0; lane < 2; ++lane) {
			const auto field = static_cast<std::uint8_t>(
				control >> (lane * 4U));
			if ((field & 8U) != 0)
				continue;
			const auto &input = (field & 2U) != 0 ? right.value : left.value;
			const auto input_lane = static_cast<std::size_t>(field & 1U);
			std::copy_n(input.begin() +
				static_cast<std::ptrdiff_t>(input_lane * 16U), 16,
				result.begin() + static_cast<std::ptrdiff_t>(lane * 16U));
		}
		break;
	case 0x08:
		for (std::size_t offset = 0; offset < size; offset += 4)
			set_vector_element(result, offset, round_vector_element(
				vector_element<float>(right.value, offset), control, cpu.mxcsr));
		break;
	case 0x09:
		for (std::size_t offset = 0; offset < size; offset += 8)
			set_vector_element(result, offset, round_vector_element(
				vector_element<double>(right.value, offset), control, cpu.mxcsr));
		break;
	case 0x0a:
		result = left.value;
		set_vector_element(result, 0, round_vector_element(
			vector_element<float>(right.value, 0), control, cpu.mxcsr));
		break;
	case 0x0b:
		result = left.value;
		set_vector_element(result, 0, round_vector_element(
			vector_element<double>(right.value, 0), control, cpu.mxcsr));
		break;
	case 0x0c:
		vector_immediate_blend<std::uint32_t>(result, left.value,
			right.value, size, control, false);
		break;
	case 0x0d:
		vector_immediate_blend<std::uint64_t>(result, left.value,
			right.value, size, control, false);
		break;
	case 0x0e:
		vector_immediate_blend<std::uint16_t>(result, left.value,
			right.value, size, control, true);
		break;
	case 0x0f: {
		const auto lane_size = size == 8 ? 8U : 16U;
		for (std::size_t lane = 0; lane < size; lane += lane_size) {
			for (std::size_t index = 0; index < lane_size; ++index) {
				const auto source_index = static_cast<std::size_t>(control) + index;
				if (source_index < lane_size)
					result[lane + index] = right.value[lane + source_index];
				else if (source_index < lane_size * 2U)
					result[lane + index] =
						left.value[lane + source_index - lane_size];
			}
		}
		break;
	}
	case 0x18:
	case 0x38:
		result = left.value;
		std::copy_n(right.value.begin(), 16,
			result.begin() + static_cast<std::ptrdiff_t>((control & 1U) * 16U));
		break;
	case 0x21: {
		result = left.value;
		const auto source_lane = instruction.auxiliary.kind ==
			OperandKind::memory ? 0U : (control >> 6U) & 3U;
		const auto destination_lane = (control >> 4U) & 3U;
		set_vector_element(result, destination_lane * 4U,
			vector_element<std::uint32_t>(right.value, source_lane * 4U));
		for (std::size_t lane = 0; lane < 4; ++lane) {
			if (((control >> lane) & 1U) != 0)
				set_vector_element(result, lane * 4U, std::uint32_t{});
		}
		break;
	}
	case 0x40:
		for (std::size_t lane = 0; lane < size; lane += 16) {
			std::array<float, 4> products{};
			for (std::size_t index = 0; index < 4; ++index) {
				if (((control >> (index + 4U)) & 1U) != 0)
					products[index] = strict_float_multiply(
						vector_element<float>(left.value, lane + index * 4U),
						vector_element<float>(right.value, lane + index * 4U));
			}
			const auto sum = strict_float_add(
				strict_float_add(products[0], products[1]),
				strict_float_add(products[2], products[3]));
			for (std::size_t index = 0; index < 4; ++index)
				set_vector_element(result, lane + index * 4U,
					((control >> index) & 1U) != 0 ? sum : 0.0F);
		}
		break;
	case 0x41:
		for (std::size_t lane = 0; lane < size; lane += 16) {
			std::array<double, 2> products{};
			for (std::size_t index = 0; index < 2; ++index) {
				if (((control >> (index + 4U)) & 1U) != 0)
					products[index] = strict_float_multiply(
						vector_element<double>(left.value, lane + index * 8U),
						vector_element<double>(right.value, lane + index * 8U));
			}
			const auto sum = strict_float_add(products[0], products[1]);
			for (std::size_t index = 0; index < 2; ++index)
				set_vector_element(result, lane + index * 8U,
					((control >> index) & 1U) != 0 ? sum : 0.0);
		}
		break;
	case 0x42:
		for (std::size_t lane = 0; lane < size; lane += 16) {
			const auto selector = static_cast<std::uint8_t>(
				(control >> ((lane / 16U) * 3U)) & 7U);
			const auto left_offset = (selector & 4U) != 0 ? 4U : 0U;
			const auto right_offset = static_cast<std::size_t>(selector & 3U) * 4U;
			for (std::size_t output = 0; output < 8; ++output) {
				std::uint16_t sum{};
				for (std::size_t byte = 0; byte < 4; ++byte) {
					const auto first = std::to_integer<unsigned>(
						left.value[lane + left_offset + output + byte]);
					const auto second = std::to_integer<unsigned>(
						right.value[lane + right_offset + byte]);
					sum = static_cast<std::uint16_t>(sum +
						(first > second ? first - second : second - first));
				}
				set_vector_element(result, lane + output * 2U, sum);
			}
		}
		break;
	case 0x44:
		for (std::size_t lane = 0; lane < size; lane += 16) {
			const auto first = vector_element<std::uint64_t>(left.value,
				lane + ((control & 1U) != 0 ? 8U : 0U));
			const auto second = vector_element<std::uint64_t>(right.value,
				lane + ((control & 0x10U) != 0 ? 8U : 0U));
			unsigned __int128 product{};
			for (unsigned bit = 0; bit < 64; ++bit) {
				if (((second >> bit) & 1U) != 0)
					product ^= static_cast<unsigned __int128>(first) << bit;
			}
			set_vector_element(result, lane,
				static_cast<std::uint64_t>(product));
			set_vector_element(result, lane + 8,
				static_cast<std::uint64_t>(product >> 64U));
		}
		break;
	case 0x4a:
		vector_immediate_blend<std::uint32_t>(result, left.value,
			right.value, size, 0, false);
		for (std::size_t offset = 0; offset < size; offset += 4) {
			if ((std::to_integer<std::uint8_t>(
				cpu.vectors[control >> 4U][offset + 3U]) & 0x80U) != 0)
				std::copy_n(right.value.begin() +
					static_cast<std::ptrdiff_t>(offset), 4,
					result.begin() + static_cast<std::ptrdiff_t>(offset));
		}
		break;
	case 0x4b:
		result = left.value;
		for (std::size_t offset = 0; offset < size; offset += 8) {
			if ((std::to_integer<std::uint8_t>(
				cpu.vectors[control >> 4U][offset + 7U]) & 0x80U) != 0)
				std::copy_n(right.value.begin() +
					static_cast<std::ptrdiff_t>(offset), 8,
					result.begin() + static_cast<std::ptrdiff_t>(offset));
		}
		break;
	case 0x4c:
		result = left.value;
		for (std::size_t offset = 0; offset < size; ++offset) {
			if ((std::to_integer<std::uint8_t>(
				cpu.vectors[control >> 4U][offset]) & 0x80U) != 0)
				result[offset] = right.value[offset];
		}
		break;
	case 0xcc: {
		auto a = vector_element<std::uint32_t>(left.value, 12);
		auto b = vector_element<std::uint32_t>(left.value, 8);
		auto c = vector_element<std::uint32_t>(left.value, 4);
		auto d = vector_element<std::uint32_t>(left.value, 0);
		auto e = vector_element<std::uint32_t>(right.value, 12);
		constexpr std::array<std::uint32_t, 4> constants{
			0x5a827999U, 0x6ed9eba1U, 0x8f1bbcdcU, 0xca62c1d6U};
		for (std::size_t round = 0; round < 4; ++round) {
			std::uint32_t function{};
			switch (control & 3U) {
			case 0: function = (b & c) ^ (~b & d); break;
			case 1:
			case 3: function = b ^ c ^ d; break;
			default: function = (b & c) ^ (b & d) ^ (c & d); break;
			}
			const auto word = round == 0 ? e : e +
				vector_element<std::uint32_t>(right.value, (3U - round) * 4U);
			const auto next = function + std::rotl(a, 5) + word +
				constants[control & 3U];
			e = d;
			d = c;
			c = std::rotl(b, 30);
			b = a;
			a = next;
		}
		set_vector_element(result, 12, a);
		set_vector_element(result, 8, b);
		set_vector_element(result, 4, c);
		set_vector_element(result, 0, d);
		break;
	}
	case 0xdf:
		for (std::size_t pair = 0; pair < 2; ++pair) {
			const auto source = vector_element<std::uint32_t>(
				right.value, (pair * 2U + 1U) * 4U);
			std::array<std::uint8_t, 4> substituted{};
			for (std::size_t byte = 0; byte < 4; ++byte)
				substituted[byte] = aes_sbox(static_cast<std::uint8_t>(
					source >> (byte * 8U)));
			std::uint32_t word{};
			for (std::size_t byte = 0; byte < 4; ++byte)
				word |= static_cast<std::uint32_t>(substituted[byte]) <<
					(byte * 8U);
			set_vector_element(result, pair * 8U, word);
			const auto rotated = std::rotr(word, 8);
			set_vector_element(result, pair * 8U + 4U,
				rotated ^ control);
		}
		break;
	default:
		return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
	}
	const auto written = write_vector_operand(gadget, cpu, memory,
		instruction.destination, result, instruction.vex);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

void set_bmi_flags(CpuState &cpu, std::uint64_t result,
	OperandWidth width, bool carry) noexcept
{
	materialize_flags(cpu);
	constexpr auto affected = rflags_carry | rflags_zero |
		rflags_sign | rflags_overflow;
	cpu.rflags &= ~affected;
	if (carry)
		cpu.rflags |= rflags_carry;
	if ((result & width_mask(width)) == 0)
		cpu.rflags |= rflags_zero;
	if ((result & (std::uint64_t{1} << (width_bits(width) - 1U))) != 0)
		cpu.rflags |= rflags_sign;
	cpu.rflags |= rflags_reserved;
	cpu.deferred_flags.operation = DeferredFlagOperation::none;
}

[[nodiscard]] StepResult scalar_bmi(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto width = instruction.destination.width;
	const auto mask = width_mask(width);
	if (instruction.vector_opcode == 0xf3) {
		const auto input = read_operand(
			gadget, cpu, memory, instruction.source);
		if (!input.succeeded)
			return read_failure(input);
		const auto value = input.value & mask;
		std::uint64_t result{};
		bool carry{};
		switch (instruction.vector_group) {
		case 1:
			result = value & (value - 1U);
			carry = value == 0;
			break;
		case 2:
			result = value ^ (value - 1U);
			carry = value == 0;
			break;
		default:
			result = value & (0U - value);
			carry = value != 0;
			break;
		}
		result &= mask;
		set_bmi_flags(cpu, result, width, carry);
		const auto written = write_operand(
			gadget, cpu, memory, instruction.destination, result);
		if (written.action == StepAction::exit)
			return written;
		advance(gadget, cpu);
		return written;
	}

	const auto first = read_operand(
		gadget, cpu, memory, instruction.source);
	if (!first.succeeded)
		return read_failure(first);
	const auto second = instruction.auxiliary.kind == OperandKind::none ?
		OperandRead{true, 0, {}} : read_operand(
			gadget, cpu, memory, instruction.auxiliary);
	if (!second.succeeded)
		return read_failure(second);
	std::uint64_t result{};
	bool update_flags{};
	bool carry{};
	switch (instruction.vector_opcode) {
	case 0xf0: {
		const auto value = first.value & mask;
		const auto count = instruction.vector_control &
			(width_bits(width) - 1U);
		result = count == 0 ? value :
			((value >> count) | (value << (width_bits(width) - count))) & mask;
		break;
	}
	case 0xf2:
		result = (~first.value) & second.value;
		update_flags = true;
		break;
	case 0xf5:
		if (instruction.vector_prefix == 2 ||
			instruction.vector_prefix == 3) {
			const auto source = first.value & mask;
			const auto bit_mask = second.value & mask;
			if (instruction.vector_prefix == 3) {
				std::uint64_t source_bit{1};
				for (std::uint64_t bit = 1; bit != 0; bit <<= 1U) {
					if ((bit_mask & bit) != 0) {
						if ((source & source_bit) != 0)
							result |= bit;
						source_bit <<= 1U;
					}
				}
			} else {
				std::uint64_t output_bit{1};
				for (std::uint64_t bit = 1; bit != 0; bit <<= 1U) {
					if ((bit_mask & bit) != 0) {
						if ((source & bit) != 0)
							result |= output_bit;
						output_bit <<= 1U;
					}
				}
			}
		} else {
			const auto index = static_cast<unsigned>(
				first.value & 0xffU);
			carry = index >= width_bits(width);
			result = carry ? second.value : second.value &
				(index == 0 ? 0 : (std::uint64_t{1} << index) - 1U);
			update_flags = true;
		}
		break;
	case 0xf6: {
		const auto product = static_cast<unsigned __int128>(
			cpu.gpr(Register::rdx) & mask) * (second.value & mask);
		const auto low = static_cast<std::uint64_t>(product) & mask;
		result = static_cast<std::uint64_t>(
			product >> width_bits(width)) & mask;
		const auto low_written = write_operand(
			gadget, cpu, memory, instruction.source, low);
		if (low_written.action == StepAction::exit)
			return low_written;
		break;
	}
	case 0xf7: {
		const auto count = static_cast<unsigned>(first.value & 0xffU) &
			(width_bits(width) - 1U);
		if (instruction.vector_prefix == 0) {
			const auto start = static_cast<unsigned>(first.value & 0xffU);
			const auto length = static_cast<unsigned>((first.value >> 8U) & 0xffU);
			if (start >= width_bits(width) || length == 0)
				result = 0;
			else {
				const auto available = width_bits(width) - start;
				const auto extracted = std::min(length, available);
				result = (second.value >> start) &
					(extracted == 64 ? mask :
						(std::uint64_t{1} << extracted) - 1U);
			}
			update_flags = true;
		} else if (instruction.vector_prefix == 1) {
			result = (second.value << count) & mask;
		} else if (instruction.vector_prefix == 2) {
			result = static_cast<std::uint64_t>(
				signed_value(second.value, width) >> count) & mask;
		} else {
			result = (second.value & mask) >> count;
		}
		break;
	}
	default:
		return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
	}
	result &= mask;
	if (update_flags)
		set_bmi_flags(cpu, result, width, carry);
	const auto written = write_operand(
		gadget, cpu, memory, instruction.destination, result);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

template<typename Float>
void vector_fma_elements(std::array<std::byte, 32> &result,
	const std::array<std::byte, 32> &destination,
	const std::array<std::byte, 32> &vvvv,
	const std::array<std::byte, 32> &rm, std::uint8_t opcode,
	std::size_t count) noexcept
{
	const auto form = static_cast<std::uint8_t>(opcode & 0xf0U);
	const auto operation = static_cast<std::uint8_t>(opcode & 0x0fU);
	for (std::size_t index = 0; index < count; ++index) {
		const auto offset = index * sizeof(Float);
		const auto old_destination = vector_element<Float>(destination, offset);
		const auto second = vector_element<Float>(vvvv, offset);
		const auto third = vector_element<Float>(rm, offset);
		Float multiplicand{};
		Float multiplier{};
		Float addend{};
		if (form == 0x90U) {
			multiplicand = old_destination;
			multiplier = third;
			addend = second;
		} else if (form == 0xa0U) {
			multiplicand = second;
			multiplier = old_destination;
			addend = third;
		} else {
			multiplicand = second;
			multiplier = third;
			addend = old_destination;
		}
		bool negate_product = operation >= 0xcU;
		bool subtract_addend = operation == 0xaU || operation == 0xbU ||
			operation == 0xeU || operation == 0xfU;
		if (operation == 0x6U)
			subtract_addend = (index & 1U) == 0;
		else if (operation == 0x7U)
			subtract_addend = (index & 1U) != 0;
		if (negate_product)
			multiplicand = -multiplicand;
		if (subtract_addend)
			addend = -addend;
		set_vector_element(result, offset,
			std::fma(multiplicand, multiplier, addend));
	}
}

[[nodiscard]] StepResult vector_fma(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto destination = read_vector_operand(
		gadget, cpu, memory, instruction.destination);
	if (!destination.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			destination.fault};
	const auto vvvv = read_vector_operand(
		gadget, cpu, memory, instruction.source);
	if (!vvvv.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			vvvv.fault};
	const auto rm = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!rm.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			rm.fault};
	auto result = destination.value;
	const auto operation = static_cast<std::uint8_t>(
		instruction.vector_opcode & 0x0fU);
	const bool scalar = operation >= 9U && (operation & 1U) != 0;
	const auto size = static_cast<std::size_t>(
		width_bytes(instruction.destination.width));
	if (instruction.vector_group != 0)
		vector_fma_elements<double>(result, destination.value, vvvv.value,
			rm.value, instruction.vector_opcode,
			scalar ? 1U : size / sizeof(double));
	else
		vector_fma_elements<float>(result, destination.value, vvvv.value,
			rm.value, instruction.vector_opcode,
			scalar ? 1U : size / sizeof(float));
	const auto written = write_vector_operand(gadget, cpu, memory,
		instruction.destination, result, true);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] float half_to_float(std::uint16_t value) noexcept
{
	const auto sign = static_cast<std::uint32_t>(value & 0x8000U) << 16U;
	const auto exponent = static_cast<unsigned>((value >> 10U) & 0x1fU);
	const auto mantissa = static_cast<std::uint32_t>(value & 0x03ffU);
	std::uint32_t bits{};
	if (exponent == 0) {
		if (mantissa == 0)
			bits = sign;
		else
			return std::copysign(std::ldexp(
				static_cast<float>(mantissa), -24), sign != 0 ? -1.0F : 1.0F);
	} else if (exponent == 0x1fU) {
		bits = sign | 0x7f800000U | (mantissa << 13U);
		if (mantissa != 0)
			bits |= 0x00400000U;
	} else {
		bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
	}
	return std::bit_cast<float>(bits);
}

[[nodiscard]] StepResult vector_map2_misc(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto opcode = instruction.vector_opcode;
	const auto size = static_cast<std::size_t>(
		width_bytes(instruction.destination.width));
	if (opcode == 0x2c || opcode == 0x2d || opcode == 0x2e ||
		opcode == 0x2f || opcode == 0x8c || opcode == 0x8e) {
		const auto mask = read_vector_operand(
			gadget, cpu, memory, instruction.source);
		if (!mask.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, mask.fault};
		const bool store = opcode == 0x2e || opcode == 0x2f || opcode == 0x8e;
		const auto element_size = static_cast<std::size_t>(
			instruction.vector_group);
		if (store) {
			const auto data = read_vector_operand(
				gadget, cpu, memory, instruction.auxiliary);
			if (!data.succeeded)
				return {StepAction::exit,
					::uml::cpu::EngineEvent::memory_fault, data.fault};
			const auto address = effective_address(
				gadget, cpu, instruction.destination.memory, true);
			for (std::size_t offset = 0; offset < size; offset += element_size) {
				if ((std::to_integer<std::uint8_t>(
					mask.value[offset + element_size - 1U]) & 0x80U) == 0)
					continue;
				if (!write_memory_bytes(memory, address + offset,
					std::span{data.value}.subspan(offset, element_size)))
					return {StepAction::exit,
						::uml::cpu::EngineEvent::memory_fault,
						memory_fault(memory, address + offset,
							::uml::memory::MemoryAccess::write)};
			}
			advance(gadget, cpu);
			return {StepAction::continue_program,
				::uml::cpu::EngineEvent::continued, {}, address,
				static_cast<std::uint8_t>(size)};
		}
		std::array<std::byte, 32> result{};
		const auto address = effective_address(
			gadget, cpu, instruction.auxiliary.memory, true);
		for (std::size_t offset = 0; offset < size; offset += element_size) {
			if ((std::to_integer<std::uint8_t>(
				mask.value[offset + element_size - 1U]) & 0x80U) == 0)
				continue;
			if (!read_memory_bytes(memory, address + offset,
				std::span{result}.subspan(offset, element_size)))
				return {StepAction::exit,
					::uml::cpu::EngineEvent::memory_fault,
					memory_fault(memory, address + offset,
						::uml::memory::MemoryAccess::read)};
		}
		const auto written = write_vector_operand(gadget, cpu, memory,
			instruction.destination, result, true);
		if (written.action == StepAction::exit)
			return written;
		advance(gadget, cpu);
		return written;
	}

	VectorRead left{true, {}, {}};
	if (instruction.source.kind != OperandKind::none) {
		left = read_vector_operand(gadget, cpu, memory, instruction.source);
		if (!left.succeeded)
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault, left.fault};
	}
	const auto right = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!right.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			right.fault};
	std::array<std::byte, 32> result{};
	switch (opcode) {
	case 0x0c:
		for (std::size_t lane = 0; lane < size; lane += 16) {
			for (std::size_t index = 0; index < 4; ++index) {
				const auto selected = vector_element<std::uint32_t>(
					right.value, lane + index * 4U) & 3U;
				set_vector_element(result, lane + index * 4U,
					vector_element<std::uint32_t>(left.value,
						lane + selected * 4U));
			}
		}
		break;
	case 0x0d:
		for (std::size_t lane = 0; lane < size; lane += 16) {
			for (std::size_t index = 0; index < 2; ++index) {
				const auto selected = (vector_element<std::uint64_t>(
					right.value, lane + index * 8U) >> 1U) & 1U;
				set_vector_element(result, lane + index * 8U,
					vector_element<std::uint64_t>(left.value,
						lane + selected * 8U));
			}
		}
		break;
	case 0x13:
		for (std::size_t index = 0; index < size / sizeof(float); ++index)
			set_vector_element(result, index * sizeof(float), half_to_float(
				vector_element<std::uint16_t>(right.value,
					index * sizeof(std::uint16_t))));
		break;
	case 0x16:
	case 0x36:
		for (std::size_t index = 0; index < 8; ++index) {
			const auto selected = vector_element<std::uint32_t>(
				left.value, index * 4U) & 7U;
			set_vector_element(result, index * 4U,
				vector_element<std::uint32_t>(right.value, selected * 4U));
		}
		break;
	case 0x18:
	case 0x19:
	case 0x1a:
	case 0x58:
	case 0x59:
	case 0x5a:
	case 0x78:
	case 0x79: {
		std::size_t element_size{};
		switch (opcode) {
		case 0x18:
		case 0x58: element_size = 4; break;
		case 0x19:
		case 0x59: element_size = 8; break;
		case 0x1a:
		case 0x5a: element_size = 16; break;
		case 0x78: element_size = 1; break;
		default: element_size = 2; break;
		}
		for (std::size_t offset = 0; offset < size; offset += element_size)
			std::copy_n(right.value.begin(), element_size,
				result.begin() + static_cast<std::ptrdiff_t>(offset));
		break;
	}
	case 0x45:
	case 0x46:
	case 0x47: {
		const auto element_size = instruction.vector_group != 0 ? 8U : 4U;
		const auto bits = element_size * 8U;
		for (std::size_t offset = 0; offset < size; offset += element_size) {
			const auto value = element_size == 8 ?
				vector_element<std::uint64_t>(left.value, offset) :
				vector_element<std::uint32_t>(left.value, offset);
			const auto count = element_size == 8 ?
				vector_element<std::uint64_t>(right.value, offset) :
				vector_element<std::uint32_t>(right.value, offset);
			std::uint64_t output{};
			if (opcode == 0x46) {
				const auto signed_input = static_cast<std::int32_t>(value);
				output = count >= bits ?
					(signed_input < 0 ? 0xffffffffU : 0U) :
					static_cast<std::uint32_t>(signed_input >> count);
			} else if (count < bits) {
				output = opcode == 0x45 ? value >> count : value << count;
			}
			if (element_size == 8)
				set_vector_element(result, offset, output);
			else
				set_vector_element(result, offset,
					static_cast<std::uint32_t>(output));
		}
		break;
	}
	default:
		return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
	}
	const auto written = write_vector_operand(gadget, cpu, memory,
		instruction.destination, result, true);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult vector_gather(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto current = read_vector_operand(
		gadget, cpu, memory, instruction.destination);
	if (!current.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			current.fault};
	const auto mask = read_vector_operand(
		gadget, cpu, memory, instruction.source);
	if (!mask.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			mask.fault};
	const auto &address_operand = instruction.auxiliary;
	const auto &addressing = address_operand.memory;
	const auto data_size = (instruction.vector_group & 1U) != 0 ? 8U : 4U;
	const auto index_size = (instruction.vector_group & 2U) != 0 ? 8U : 4U;
	const auto register_bytes = (instruction.vector_group & 4U) != 0 ? 32U : 16U;
	const auto count = register_bytes / std::max(data_size, index_size);
	auto result = current.value;
	auto updated_mask = mask.value;
	MemoryOperand base_addressing = addressing;
	base_addressing.index = MemoryOperand::no_register;
	base_addressing.vector_index = false;
	const auto base = effective_address(gadget, cpu, base_addressing, true);
	const auto &indices = cpu.vectors[addressing.index];
	for (std::size_t index = 0; index < count; ++index) {
		const auto mask_offset = index * data_size;
		if ((std::to_integer<std::uint8_t>(
			mask.value[mask_offset + data_size - 1U]) & 0x80U) == 0)
			continue;
		std::int64_t vector_offset{};
		if (index_size == 8U)
			vector_offset = vector_element<std::int64_t>(
				indices, index * index_size);
		else
			vector_offset = vector_element<std::int32_t>(
				indices, index * index_size);
		auto address = base + static_cast<std::uint64_t>(vector_offset) *
			addressing.scale;
		if (addressing.address_32)
			address = static_cast<std::uint32_t>(address);
		if (!read_memory_bytes(memory, address,
			std::span{result}.subspan(mask_offset, data_size))) {
			const auto destination_written = write_vector_operand(
				gadget, cpu, memory, instruction.destination, result, true);
			if (destination_written.action == StepAction::exit)
				return destination_written;
			const auto mask_written = write_vector_operand(
				gadget, cpu, memory, instruction.source, updated_mask, true);
			if (mask_written.action == StepAction::exit)
				return mask_written;
			return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
				memory_fault(memory, address,
					::uml::memory::MemoryAccess::read)};
		}
		std::fill_n(updated_mask.begin() +
			static_cast<std::ptrdiff_t>(mask_offset), data_size, std::byte{});
	}
	const auto destination_written = write_vector_operand(gadget, cpu, memory,
		instruction.destination, result, true);
	if (destination_written.action == StepAction::exit)
		return destination_written;
	const auto mask_written = write_vector_operand(gadget, cpu, memory,
		instruction.source, updated_mask, true);
	if (mask_written.action == StepAction::exit)
		return mask_written;
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult vector_test(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto left = read_vector_operand(
		gadget, cpu, memory, instruction.source);
	if (!left.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			left.fault};
	const auto right = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!right.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			right.fault};
	const auto size = static_cast<std::size_t>(
		width_bytes(instruction.source.width));
	bool intersection_zero = true;
	bool outside_zero = true;
	const bool sign_bits_only = instruction.vector_map == 2 &&
		(instruction.vector_opcode == 0x0e ||
			instruction.vector_opcode == 0x0f);
	const auto stride = sign_bits_only ?
		(instruction.vector_opcode == 0x0f ? 8U : 4U) : 1U;
	for (std::size_t index = stride - 1U; index < size; index += stride) {
		const auto first = std::to_integer<std::uint8_t>(left.value[index]);
		const auto second = std::to_integer<std::uint8_t>(right.value[index]);
		const auto relevant = sign_bits_only ? 0x80U : 0xffU;
		intersection_zero = intersection_zero &&
			(first & second & relevant) == 0;
		outside_zero = outside_zero &&
			(static_cast<std::uint8_t>(~first) & second & relevant) == 0;
	}
	materialize_flags(cpu);
	constexpr auto affected = rflags_carry | rflags_parity |
		rflags_auxiliary | rflags_zero | rflags_sign | rflags_overflow;
	cpu.rflags &= ~affected;
	if (intersection_zero)
		cpu.rflags |= rflags_zero;
	if (outside_zero)
		cpu.rflags |= rflags_carry;
	cpu.rflags |= rflags_reserved;
	cpu.deferred_flags.operation = DeferredFlagOperation::none;
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult vector_integer(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto left = read_vector_operand(
		gadget, cpu, memory, instruction.source);
	if (!left.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			left.fault};
	const auto right = read_vector_operand(
		gadget, cpu, memory, instruction.auxiliary);
	if (!right.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			right.fault};
	auto result = left.value;
	const auto size = static_cast<std::size_t>(
		width_bytes(instruction.destination.width));
	const auto opcode = instruction.vector_opcode;
	switch (opcode) {
	case 0x60: vector_unpack<std::uint8_t>(result,
		left.value, right.value, size, false); break;
	case 0x61: vector_unpack<std::uint16_t>(result,
		left.value, right.value, size, false); break;
	case 0x62: vector_unpack<std::uint32_t>(result,
		left.value, right.value, size, false); break;
	case 0x63: vector_pack<std::int16_t, std::int8_t>(result,
		left.value, right.value, size, false); break;
	case 0x64: vector_binary_elements<std::int8_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::int8_t>(a > b ? -1 : 0); }); break;
	case 0x65: vector_binary_elements<std::int16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::int16_t>(a > b ? -1 : 0); }); break;
	case 0x66: vector_binary_elements<std::int32_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return a > b ? -1 : 0; }); break;
	case 0x67: vector_pack<std::int16_t, std::int8_t>(result,
		left.value, right.value, size, true); break;
	case 0x68: vector_unpack<std::uint8_t>(result,
		left.value, right.value, size, true); break;
	case 0x69: vector_unpack<std::uint16_t>(result,
		left.value, right.value, size, true); break;
	case 0x6a: vector_unpack<std::uint32_t>(result,
		left.value, right.value, size, true); break;
	case 0x6b: vector_pack<std::int32_t, std::int16_t>(result,
		left.value, right.value, size, false); break;
	case 0x6c: vector_unpack<std::uint64_t>(result,
		left.value, right.value, size, false); break;
	case 0x6d: vector_unpack<std::uint64_t>(result,
		left.value, right.value, size, true); break;
	case 0x70: {
		result = right.value;
		const auto lane_size = size == 8 ? 8U : 16U;
		const auto word_base = instruction.vector_prefix == 2 ? 4U : 0U;
		const auto word_limit = instruction.vector_prefix == 2 ? 8U : 4U;
		const auto word_shuffle = instruction.vector_prefix == 2 ||
			instruction.vector_prefix == 3 || size == 8;
		for (std::size_t lane = 0; lane < size; lane += lane_size) {
			if (word_shuffle) {
				for (std::size_t index = word_base;
					index < word_limit; ++index) {
					const auto selected = (instruction.vector_control >>
						((index - word_base) * 2U)) & 3U;
					set_vector_element(result, lane + index * 2U,
						vector_element<std::uint16_t>(right.value,
							lane + (word_base + selected) * 2U));
				}
			} else {
				for (std::size_t index = 0; index < 4; ++index) {
					const auto selected = (instruction.vector_control >>
						(index * 2U)) & 3U;
					set_vector_element(result, lane + index * 4U,
						vector_element<std::uint32_t>(right.value,
							lane + selected * 4U));
				}
			}
		}
		break;
	}
	case 0x71:
	case 0x72:
	case 0x73: {
		const auto count = static_cast<std::uint64_t>(
			instruction.vector_control);
		if (instruction.vector_group == 3 ||
			instruction.vector_group == 7) {
			result.fill(std::byte{});
			const auto shift = std::min<std::size_t>(count, 16);
			for (std::size_t lane = 0; lane < size; lane += 16) {
				for (std::size_t index = 0; index < 16 - shift; ++index) {
					const auto source_index = instruction.vector_group == 3 ?
						index + shift : index;
					const auto destination_index = instruction.vector_group == 3 ?
						index : index + shift;
					result[lane + destination_index] =
						left.value[lane + source_index];
				}
			}
		} else if (instruction.vector_group == 4) {
			if (opcode == 0x71)
				vector_arithmetic_shift_right<std::int16_t>(
					result, left.value, size, count);
			else
				vector_arithmetic_shift_right<std::int32_t>(
					result, left.value, size, count);
		} else {
			const bool left_shift = instruction.vector_group == 6;
			if (opcode == 0x71)
				vector_logical_shift<std::uint16_t>(
					result, left.value, size, count, left_shift);
			else if (opcode == 0x72)
				vector_logical_shift<std::uint32_t>(
					result, left.value, size, count, left_shift);
			else
				vector_logical_shift<std::uint64_t>(
					result, left.value, size, count, left_shift);
		}
		break;
	}
	case 0x74: vector_binary_elements<std::uint8_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint8_t>(a == b ? 0xffU : 0U); }); break;
	case 0x75: vector_binary_elements<std::uint16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint16_t>(a == b ? 0xffffU : 0U); }); break;
	case 0x76: vector_binary_elements<std::uint32_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return a == b ? 0xffff'ffffU : 0U; }); break;
	case 0xd1: vector_logical_shift<std::uint16_t>(result,
		left.value, size, vector_element<std::uint64_t>(right.value, 0), false); break;
	case 0xd2: vector_logical_shift<std::uint32_t>(result,
		left.value, size, vector_element<std::uint64_t>(right.value, 0), false); break;
	case 0xd3: vector_logical_shift<std::uint64_t>(result,
		left.value, size, vector_element<std::uint64_t>(right.value, 0), false); break;
	case 0xd4: vector_binary_elements<std::uint64_t>(result,
		left.value, right.value, size, [](auto a, auto b) { return a + b; }); break;
	case 0xd5: vector_binary_elements<std::uint16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint16_t>(a * b); }); break;
	case 0xd8: vector_binary_elements<std::uint8_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint8_t>(a < b ? 0 : a - b); }); break;
	case 0xd9: vector_binary_elements<std::uint16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint16_t>(a < b ? 0 : a - b); }); break;
	case 0xda: vector_binary_elements<std::uint8_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return std::min(a, b); }); break;
	case 0xdb: vector_binary_elements<std::uint64_t>(result,
		left.value, right.value, size, [](auto a, auto b) { return a & b; }); break;
	case 0xdc: vector_binary_elements<std::uint8_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint8_t>(std::min<unsigned>(a + b, 0xffU)); }); break;
	case 0xdd: vector_binary_elements<std::uint16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint16_t>(std::min<unsigned>(a + b, 0xffffU)); }); break;
	case 0xde: vector_binary_elements<std::uint8_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return std::max(a, b); }); break;
	case 0xdf: vector_binary_elements<std::uint64_t>(result,
		left.value, right.value, size, [](auto a, auto b) { return ~a & b; }); break;
	case 0xe0: vector_binary_elements<std::uint8_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint8_t>((unsigned{a} + b + 1U) >> 1U); }); break;
	case 0xe1: vector_arithmetic_shift_right<std::int16_t>(result,
		left.value, size, vector_element<std::uint64_t>(right.value, 0)); break;
	case 0xe2: vector_arithmetic_shift_right<std::int32_t>(result,
		left.value, size, vector_element<std::uint64_t>(right.value, 0)); break;
	case 0xe3: vector_binary_elements<std::uint16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint16_t>((unsigned{a} + b + 1U) >> 1U); }); break;
	case 0xe4: vector_binary_elements<std::uint16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint16_t>(
				(static_cast<std::uint32_t>(a) * b) >> 16U); }); break;
	case 0xe5: vector_binary_elements<std::int16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::int16_t>(
				(static_cast<std::int32_t>(a) * b) >> 16U); }); break;
	case 0xe8: vector_binary_elements<std::int8_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return saturate_signed<std::int8_t>(int{a} - int{b}); }); break;
	case 0xe9: vector_binary_elements<std::int16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return saturate_signed<std::int16_t>(int{a} - int{b}); }); break;
	case 0xea: vector_binary_elements<std::int16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return std::min(a, b); }); break;
	case 0xeb: vector_binary_elements<std::uint64_t>(result,
		left.value, right.value, size, [](auto a, auto b) { return a | b; }); break;
	case 0xec: vector_binary_elements<std::int8_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return saturate_signed<std::int8_t>(int{a} + int{b}); }); break;
	case 0xed: vector_binary_elements<std::int16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return saturate_signed<std::int16_t>(int{a} + int{b}); }); break;
	case 0xee: vector_binary_elements<std::int16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return std::max(a, b); }); break;
	case 0xef: vector_binary_elements<std::uint64_t>(result,
		left.value, right.value, size, [](auto a, auto b) { return a ^ b; }); break;
	case 0xf1: vector_logical_shift<std::uint16_t>(result,
		left.value, size, vector_element<std::uint64_t>(right.value, 0), true); break;
	case 0xf2: vector_logical_shift<std::uint32_t>(result,
		left.value, size, vector_element<std::uint64_t>(right.value, 0), true); break;
	case 0xf3: vector_logical_shift<std::uint64_t>(result,
		left.value, size, vector_element<std::uint64_t>(right.value, 0), true); break;
	case 0xf4:
		for (std::size_t offset = 0; offset < size; offset += 8) {
			const auto a = vector_element<std::uint32_t>(left.value, offset);
			const auto b = vector_element<std::uint32_t>(right.value, offset);
			set_vector_element(result, offset,
				static_cast<std::uint64_t>(a) * b);
		}
		break;
	case 0xf5:
		for (std::size_t offset = 0; offset < size; offset += 4) {
			const auto a0 = vector_element<std::int16_t>(left.value, offset);
			const auto a1 = vector_element<std::int16_t>(left.value, offset + 2);
			const auto b0 = vector_element<std::int16_t>(right.value, offset);
			const auto b1 = vector_element<std::int16_t>(right.value, offset + 2);
			const auto sum = static_cast<std::uint32_t>(
				static_cast<std::int64_t>(a0) * b0 +
				static_cast<std::int64_t>(a1) * b1);
			set_vector_element(result, offset, sum);
		}
		break;
	case 0xf6:
		result.fill(std::byte{});
		for (std::size_t group = 0; group < size; group += 8) {
			std::uint64_t sum{};
			for (std::size_t index = 0; index < 8; ++index) {
				const auto a = std::to_integer<unsigned>(left.value[group + index]);
				const auto b = std::to_integer<unsigned>(right.value[group + index]);
				sum += a > b ? a - b : b - a;
			}
			set_vector_element(result, group, sum);
		}
		break;
	case 0xf8: vector_binary_elements<std::uint8_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint8_t>(a - b); }); break;
	case 0xf9: vector_binary_elements<std::uint16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint16_t>(a - b); }); break;
	case 0xfa: vector_binary_elements<std::uint32_t>(result,
		left.value, right.value, size, [](auto a, auto b) { return a - b; }); break;
	case 0xfb: vector_binary_elements<std::uint64_t>(result,
		left.value, right.value, size, [](auto a, auto b) { return a - b; }); break;
	case 0xfc: vector_binary_elements<std::uint8_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint8_t>(a + b); }); break;
	case 0xfd: vector_binary_elements<std::uint16_t>(result,
		left.value, right.value, size, [](auto a, auto b) {
			return static_cast<std::uint16_t>(a + b); }); break;
	case 0xfe: vector_binary_elements<std::uint32_t>(result,
		left.value, right.value, size, [](auto a, auto b) { return a + b; }); break;
	default:
		return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
	}
	const auto written = write_vector_operand(gadget, cpu, memory,
		instruction.destination, result, instruction.vex);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult vector_to_gpr(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto source = read_vector_operand(
		gadget, cpu, memory, instruction.source);
	if (!source.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			source.fault};
	const auto size = static_cast<std::size_t>(
		width_bytes(instruction.source.width));
	std::uint64_t value{};
	if (instruction.vector_opcode == 0x50) {
		const auto element_size = instruction.vector_prefix == 1 ? 8U : 4U;
		for (std::size_t lane = 0; lane < size / element_size; ++lane) {
			const auto byte = source.value[(lane + 1U) * element_size - 1U];
			value |= static_cast<std::uint64_t>(
				(std::to_integer<unsigned>(byte) >> 7U) & 1U) << lane;
		}
	} else if (instruction.vector_opcode == 0xd7) {
		for (std::size_t index = 0; index < size; ++index)
			value |= static_cast<std::uint64_t>(
				(std::to_integer<unsigned>(source.value[index]) >> 7U) & 1U) <<
				index;
	} else {
		const auto index = instruction.vector_control % (size / 2U);
		value = vector_element<std::uint16_t>(source.value, index * 2U);
	}
	write_register(cpu, instruction.destination, value);
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult vector_insert(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto merge = read_vector_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!merge.succeeded)
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			merge.fault};
	const auto input = read_operand(
		gadget, cpu, memory, gadget.instruction.auxiliary);
	if (!input.succeeded)
		return read_failure(input);
	auto result = merge.value;
	const auto size = static_cast<std::size_t>(
		width_bytes(gadget.instruction.destination.width));
	const auto index = gadget.instruction.vector_control % (size / 2U);
	set_vector_element(result, index * 2U,
		static_cast<std::uint16_t>(input.value));
	const auto written = write_vector_operand(gadget, cpu, memory,
		gadget.instruction.destination, result, gadget.instruction.vex);
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

[[nodiscard]] constexpr std::uint64_t reverse_bytes(
	std::uint64_t value, OperandWidth width) noexcept
{
	if (width == OperandWidth::word)
		return static_cast<std::uint16_t>(
			__builtin_bswap16(static_cast<std::uint16_t>(value)));
	if (width == OperandWidth::dword)
		return static_cast<std::uint32_t>(
			__builtin_bswap32(static_cast<std::uint32_t>(value)));
	return __builtin_bswap64(value);
}

[[nodiscard]] StepResult move_byte_swap(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto source = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!source.succeeded)
		return read_failure(source);
	const auto written = write_operand(gadget, cpu, memory,
		gadget.instruction.destination,
		reverse_bytes(source.value, gadget.instruction.source.width));
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult crc32(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto source = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!source.succeeded)
		return read_failure(source);
	auto crc = static_cast<std::uint32_t>(
		read_register(cpu, gadget.instruction.destination));
	for (unsigned byte = 0;
		byte < width_bytes(gadget.instruction.source.width); ++byte) {
		crc ^= static_cast<std::uint8_t>(source.value >> (byte * 8U));
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1U) ^ ((crc & 1U) != 0 ? 0x82f63b78U : 0U);
	}
	write_register(cpu, gadget.instruction.destination, crc);
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult adx(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto source = read_operand(
		gadget, cpu, memory, gadget.instruction.source);
	if (!source.succeeded)
		return read_failure(source);
	materialize_flags(cpu);
	const auto flag = gadget.instruction.vector_control != 0 ?
		rflags_overflow : rflags_carry;
	const auto carry = (cpu.rflags & flag) != 0 ? 1U : 0U;
	const auto destination = read_register(cpu, gadget.instruction.destination);
	const auto mask = width_mask(gadget.instruction.destination.width);
	const auto full = static_cast<unsigned __int128>(destination) +
		source.value + carry;
	write_register(cpu, gadget.instruction.destination,
		static_cast<std::uint64_t>(full) & mask);
	if (full > mask)
		cpu.rflags |= flag;
	else
		cpu.rflags &= ~flag;
	cpu.rflags |= rflags_reserved;
	cpu.deferred_flags.operation = DeferredFlagOperation::none;
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

[[nodiscard]] StepResult segment_move(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	auto &selector = cpu.segment_selectors[instruction.vector_group];
	if (instruction.vector_opcode == 0x8e) {
		const auto value = read_operand(
			gadget, cpu, memory, instruction.source);
		if (!value.succeeded)
			return read_failure(value);
		selector = static_cast<std::uint16_t>(value.value);
		advance(gadget, cpu);
		return {};
	}
	const auto written = write_operand(
		gadget, cpu, memory, instruction.destination, selector);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
}

[[nodiscard]] StepResult segment_stack(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool pushing = instruction.vector_opcode == 0xa0 ||
		instruction.vector_opcode == 0xa8;
	const auto width = pushing ? instruction.source.width :
		instruction.destination.width;
	const auto size = width_bytes(width);
	if (pushing) {
		const auto address = cpu.gpr(Register::rsp) - size;
		if (!write_memory(memory, address, width,
			cpu.segment_selectors[instruction.vector_group]))
			return {StepAction::exit,
				::uml::cpu::EngineEvent::memory_fault,
				memory_fault(memory, address,
					::uml::memory::MemoryAccess::write)};
		cpu.gpr(Register::rsp) = address;
		advance(gadget, cpu);
		return {StepAction::continue_program,
			::uml::cpu::EngineEvent::continued, {}, address, size};
	}
	const auto address = cpu.gpr(Register::rsp);
	std::uint64_t value{};
	if (!read_memory(memory, address, width, value))
		return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
			memory_fault(memory, address, ::uml::memory::MemoryAccess::read)};
	cpu.segment_selectors[instruction.vector_group] =
		static_cast<std::uint16_t>(value);
	cpu.gpr(Register::rsp) += size;
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

[[nodiscard]] StepResult loop(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &) noexcept
{
	const auto opcode = gadget.instruction.vector_opcode;
	const auto width = gadget.instruction.vector_control != 0 ?
		OperandWidth::dword : OperandWidth::qword;
	auto count = cpu.gpr(Register::rcx) & width_mask(width);
	bool take{};
	if (opcode == 0xe3) {
		take = count == 0;
	} else {
		count = (count - 1U) & width_mask(width);
		write_gpr(cpu, static_cast<std::uint8_t>(Register::rcx), width, count);
		take = count != 0;
		if (opcode != 0xe2) {
			materialize_flags(cpu);
			const bool zero = (cpu.rflags & rflags_zero) != 0;
			take = take && (opcode == 0xe1 ? zero : !zero);
		}
	}
	cpu.rip = take ? relative_target(gadget) :
		gadget.address + gadget.instruction.length;
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

[[nodiscard]] StepResult mxcsr(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	if (gadget.instruction.vector_group == 2) {
		const auto value = read_operand(
			gadget, cpu, memory, gadget.instruction.destination);
		if (!value.succeeded)
			return read_failure(value);
		constexpr std::uint32_t modeled = (1U << 6U) |
			(3U << 13U) | (1U << 15U);
		cpu.mxcsr = 0x1f80U |
			(static_cast<std::uint32_t>(value.value) & modeled);
		advance(gadget, cpu);
		return {};
	}
	const auto written = write_operand(gadget, cpu, memory,
		gadget.instruction.destination, cpu.mxcsr);
	if (written.action == StepAction::exit)
		return written;
	advance(gadget, cpu);
	return written;
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
	case Operation::move_byte_swap: return move_byte_swap;
	case Operation::crc32: return crc32;
	case Operation::adx: return adx;
	case Operation::scalar_bmi: return scalar_bmi;
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
	case Operation::segment_move: return segment_move;
	case Operation::segment_stack: return segment_stack;
	case Operation::jump: return jump;
	case Operation::jump_indirect: return jump_indirect;
	case Operation::jump_conditional: return jump_conditional;
	case Operation::loop: return loop;
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
	case Operation::vector_move: return vector_move;
	case Operation::vector_move_scalar: return vector_move_scalar;
	case Operation::vector_zero: return vector_zero;
	case Operation::vector_alu: return vector_alu;
	case Operation::vector_lane_move: return vector_lane_move;
	case Operation::vector_convert: return vector_convert;
	case Operation::vector_gpr_move: return vector_gpr_move;
	case Operation::vector_integer: return vector_integer;
	case Operation::vector_integer_map2: return vector_integer_map2;
	case Operation::vector_integer_map3: return vector_integer_map3;
	case Operation::vector_fma: return vector_fma;
	case Operation::vector_map2_misc: return vector_map2_misc;
	case Operation::vector_gather: return vector_gather;
	case Operation::vector_test: return vector_test;
	case Operation::vector_aes: return vector_aes;
	case Operation::vector_sha: return vector_sha;
	case Operation::vector_to_gpr: return vector_to_gpr;
	case Operation::vector_insert: return vector_insert;
	case Operation::vector_scalar_convert: return vector_scalar_convert;
	case Operation::vector_compare_flags: return vector_compare_flags;
	case Operation::vector_mask_store: return vector_mask_store;
	case Operation::mxcsr: return mxcsr;
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
