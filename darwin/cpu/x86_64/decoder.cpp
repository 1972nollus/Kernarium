// SPDX-License-Identifier: GPL-2.0-or-later
#include "decoder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::cpu::x86_64 {
namespace {

class ByteReader final {
public:
	ByteReader(::uml::memory::GuestAddressSpace &memory,
		std::uint64_t address) noexcept
		: memory_{memory}, address_{address}
	{}

	[[nodiscard]] bool read(std::uint8_t &value)
	{
		if (position_ == maximum_instruction_bytes) {
			fault_address_ = address_ + position_;
			return false;
		}
		std::byte byte{};
		if (!memory_.fetch(address_ + position_, std::span{&byte, 1})) {
			fault_address_ = address_ + position_;
			return false;
		}
		value = std::to_integer<std::uint8_t>(byte);
		++position_;
		return true;
	}

	[[nodiscard]] bool read_unsigned(
		unsigned bytes, std::uint64_t &value)
	{
		value = 0;
		for (unsigned index = 0; index < bytes; ++index) {
			std::uint8_t byte{};
			if (!read(byte))
				return false;
			value |= std::uint64_t{byte} << (index * 8U);
		}
		return true;
	}

	[[nodiscard]] std::uint8_t position() const noexcept { return position_; }
	[[nodiscard]] std::uint64_t fault_address() const noexcept
	{
		return fault_address_;
	}

private:
	static constexpr std::uint8_t maximum_instruction_bytes = 15;
	::uml::memory::GuestAddressSpace &memory_;
	std::uint64_t address_;
	std::uint64_t fault_address_{};
	std::uint8_t position_{};
};

struct Prefixes final {
	Segment segment{Segment::none};
	std::uint8_t rex{};
	std::uint8_t repeat{};
	bool operand_size{};
	bool address_size{};
	bool locked{};
	bool rex_present{};
};

[[nodiscard]] constexpr std::uint64_t sign_extend(
	std::uint64_t value, unsigned bits) noexcept
{
	if (bits == 64)
		return value;
	const auto sign = std::uint64_t{1} << (bits - 1U);
	const auto mask = (std::uint64_t{1} << bits) - 1U;
	value &= mask;
	return (value ^ sign) - sign;
}

[[nodiscard]] constexpr OperandWidth ordinary_width(
	const Prefixes &prefixes) noexcept
{
	if ((prefixes.rex & 8U) != 0)
		return OperandWidth::qword;
	return prefixes.operand_size ? OperandWidth::word : OperandWidth::dword;
}

[[nodiscard]] constexpr OperandWidth stack_width(
	const Prefixes &prefixes) noexcept
{
	return prefixes.operand_size ? OperandWidth::word : OperandWidth::qword;
}

[[nodiscard]] constexpr Operand register_operand(std::uint8_t low,
	bool extension, OperandWidth width, bool rex_present) noexcept
{
	Operand operand{};
	operand.kind = OperandKind::reg;
	operand.width = width;
	if (width == OperandWidth::byte && !rex_present && !extension && low >= 4) {
		operand.reg.index = static_cast<std::uint8_t>(low - 4U);
		operand.reg.high_byte = true;
	} else {
		operand.reg.index = static_cast<std::uint8_t>(
			low | (extension ? 8U : 0U));
	}
	return operand;
}

[[nodiscard]] constexpr Operand immediate_operand(
	std::uint64_t value, OperandWidth width) noexcept
{
	Operand operand{};
	operand.kind = OperandKind::immediate;
	operand.width = width;
	operand.immediate = value;
	return operand;
}

class InstructionDecoder final {
public:
	InstructionDecoder(::uml::memory::GuestAddressSpace &memory,
		std::uint64_t rip) noexcept
		: reader_{memory, rip}
	{}

	[[nodiscard]] DecodeResult run()
	{
		std::uint8_t opcode{};
		if (!read_opcode(opcode))
			return fetch_fault();

		Instruction instruction{};
		instruction.locked = prefixes_.locked;
		if (!decode_opcode(opcode, instruction))
			return failed_ ? fetch_fault() : illegal();
		instruction.length = reader_.position();
		if (prefixes_.locked)
			return illegal();
		return {DecodeStatus::decoded, instruction, 0};
	}

private:
	[[nodiscard]] DecodeResult fetch_fault() const noexcept
	{
		return {DecodeStatus::fetch_fault, {}, reader_.fault_address()};
	}

	[[nodiscard]] static DecodeResult illegal() noexcept
	{
		return {DecodeStatus::illegal_instruction, {}, 0};
	}

	[[nodiscard]] bool read(std::uint8_t &value)
	{
		if (reader_.read(value))
			return true;
		failed_ = true;
		return false;
	}

	[[nodiscard]] bool read_unsigned(unsigned bytes, std::uint64_t &value)
	{
		if (reader_.read_unsigned(bytes, value))
			return true;
		failed_ = true;
		return false;
	}

	[[nodiscard]] bool read_opcode(std::uint8_t &opcode)
	{
		for (;;) {
			if (!read(opcode))
				return false;
			switch (opcode) {
			case 0x26:
			case 0x2e:
			case 0x36:
			case 0x3e:
				prefixes_.rex = 0;
				prefixes_.rex_present = false;
				continue;
			case 0x64:
				prefixes_.segment = Segment::fs;
				prefixes_.rex = 0;
				prefixes_.rex_present = false;
				continue;
			case 0x65:
				prefixes_.segment = Segment::gs;
				prefixes_.rex = 0;
				prefixes_.rex_present = false;
				continue;
			case 0x66:
				prefixes_.operand_size = true;
				prefixes_.rex = 0;
				prefixes_.rex_present = false;
				continue;
			case 0x67:
				prefixes_.address_size = true;
				prefixes_.rex = 0;
				prefixes_.rex_present = false;
				continue;
			case 0xf0:
				prefixes_.locked = true;
				prefixes_.rex = 0;
				prefixes_.rex_present = false;
				continue;
			case 0xf2:
			case 0xf3:
				prefixes_.repeat = opcode;
				prefixes_.rex = 0;
				prefixes_.rex_present = false;
				continue;
			default:
				if (opcode >= 0x40 && opcode <= 0x4f) {
					prefixes_.rex = opcode;
					prefixes_.rex_present = true;
					continue;
				}
				return true;
			}
		}
	}

	[[nodiscard]] Operand reg_field(
		std::uint8_t modrm, OperandWidth width) const noexcept
	{
		return register_operand(static_cast<std::uint8_t>((modrm >> 3U) & 7U),
			(prefixes_.rex & 4U) != 0, width, prefixes_.rex_present);
	}

	[[nodiscard]] bool rm_operand(
		std::uint8_t modrm, OperandWidth width, Operand &operand)
	{
		const auto mod = static_cast<std::uint8_t>(modrm >> 6U);
		const auto rm = static_cast<std::uint8_t>(modrm & 7U);
		if (mod == 3) {
			operand = register_operand(rm, (prefixes_.rex & 1U) != 0,
				width, prefixes_.rex_present);
			return true;
		}
		if (prefixes_.address_size)
			return false;

		operand.kind = OperandKind::memory;
		operand.width = width;
		operand.memory.segment = prefixes_.segment;
		if (rm == 4) {
			std::uint8_t sib{};
			if (!read(sib))
				return false;
			operand.memory.scale = static_cast<std::uint8_t>(
				1U << (sib >> 6U));
			const auto index = static_cast<std::uint8_t>((sib >> 3U) & 7U);
			if (index != 4 || (prefixes_.rex & 2U) != 0)
				operand.memory.index = static_cast<std::uint8_t>(index |
					((prefixes_.rex & 2U) != 0 ? 8U : 0U));
			const auto base = static_cast<std::uint8_t>(sib & 7U);
			if (mod == 0 && base == 5) {
				std::uint64_t displacement{};
				if (!read_unsigned(4, displacement))
					return false;
				operand.memory.displacement = static_cast<std::int32_t>(
					static_cast<std::uint32_t>(displacement));
			} else {
				operand.memory.base = static_cast<std::uint8_t>(base |
					((prefixes_.rex & 1U) != 0 ? 8U : 0U));
			}
		} else if (mod == 0 && rm == 5) {
			std::uint64_t displacement{};
			if (!read_unsigned(4, displacement))
				return false;
			operand.memory.rip_relative = true;
			operand.memory.displacement = static_cast<std::int32_t>(
				static_cast<std::uint32_t>(displacement));
		} else {
			operand.memory.base = static_cast<std::uint8_t>(rm |
				((prefixes_.rex & 1U) != 0 ? 8U : 0U));
		}

		if (mod == 1) {
			std::uint64_t displacement{};
			if (!read_unsigned(1, displacement))
				return false;
			operand.memory.displacement = static_cast<std::int8_t>(displacement);
		} else if (mod == 2) {
			std::uint64_t displacement{};
			if (!read_unsigned(4, displacement))
				return false;
			operand.memory.displacement = static_cast<std::int32_t>(
				static_cast<std::uint32_t>(displacement));
		}
		return true;
	}

	[[nodiscard]] bool decode_binary(Instruction &instruction,
		Operation operation, OperandWidth width, bool register_destination)
	{
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		Operand rm{};
		if (!rm_operand(modrm, width, rm))
			return false;
		const auto reg = reg_field(modrm, width);
		instruction.operation = operation;
		instruction.destination = register_destination ? reg : rm;
		instruction.source = register_destination ? rm : reg;
		return true;
	}

	[[nodiscard]] bool decode_immediate_group(Instruction &instruction,
		OperandWidth width, unsigned immediate_bytes, bool sign_extended)
	{
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		const auto group = static_cast<std::uint8_t>((modrm >> 3U) & 7U);
		switch (group) {
		case 0: instruction.operation = Operation::add; break;
		case 1: instruction.operation = Operation::bit_or; break;
		case 4: instruction.operation = Operation::bit_and; break;
		case 5: instruction.operation = Operation::subtract; break;
		case 6: instruction.operation = Operation::bit_xor; break;
		case 7: instruction.operation = Operation::compare; break;
		default: return false;
		}
		if (!rm_operand(modrm, width, instruction.destination))
			return false;
		std::uint64_t immediate{};
		if (!read_unsigned(immediate_bytes, immediate))
			return false;
		if (sign_extended)
			immediate = sign_extend(immediate, immediate_bytes * 8U);
		instruction.source = immediate_operand(immediate, width);
		return true;
	}

	[[nodiscard]] bool decode_accumulator_immediate(Instruction &instruction,
		Operation operation, OperandWidth width)
	{
		const auto bytes = width == OperandWidth::word ? 2U : 4U;
		std::uint64_t immediate{};
		if (!read_unsigned(bytes, immediate))
			return false;
		if (width == OperandWidth::qword)
			immediate = sign_extend(immediate, 32);
		instruction.operation = operation;
		instruction.destination = register_operand(
			0, false, width, prefixes_.rex_present);
		instruction.source = immediate_operand(immediate, width);
		return true;
	}

	[[nodiscard]] bool decode_relative(Instruction &instruction,
		Operation operation, unsigned bytes)
	{
		std::uint64_t displacement{};
		if (!read_unsigned(bytes, displacement))
			return false;
		instruction.operation = operation;
		instruction.source = immediate_operand(
			sign_extend(displacement, bytes * 8U), OperandWidth::qword);
		return true;
	}

	[[nodiscard]] bool decode_two_byte(Instruction &instruction)
	{
		std::uint8_t opcode{};
		if (!read(opcode))
			return false;
		if (opcode >= 0x80 && opcode <= 0x8f) {
			instruction.condition = static_cast<Condition>(opcode & 0xfU);
			return decode_relative(instruction,
				Operation::jump_conditional, 4);
		}
		switch (opcode) {
		case 0x05:
			instruction.operation = Operation::system_call;
			return true;
		case 0x1e: {
			std::uint8_t suffix{};
			if (!read(suffix))
				return false;
			if (prefixes_.repeat != 0xf3 || suffix != 0xfa)
				return false;
			instruction.operation = Operation::nop;
			return true;
		}
		case 0x1f: {
			std::uint8_t modrm{};
			if (!read(modrm))
				return false;
			Operand ignored{};
			if (((modrm >> 3U) & 7U) != 0 ||
				!rm_operand(modrm, ordinary_width(prefixes_), ignored))
				return false;
			instruction.operation = Operation::nop;
			return true;
		}
		default:
			return false;
		}
	}

	[[nodiscard]] bool decode_opcode(
		std::uint8_t opcode, Instruction &instruction)
	{
		const auto width = ordinary_width(prefixes_);
		if (opcode >= 0x50 && opcode <= 0x57) {
			instruction.operation = Operation::push;
			instruction.source = register_operand(
				static_cast<std::uint8_t>(opcode & 7U),
				(prefixes_.rex & 1U) != 0, stack_width(prefixes_), true);
			return true;
		}
		if (opcode >= 0x58 && opcode <= 0x5f) {
			instruction.operation = Operation::pop;
			instruction.destination = register_operand(
				static_cast<std::uint8_t>(opcode & 7U),
				(prefixes_.rex & 1U) != 0, stack_width(prefixes_), true);
			return true;
		}
		if (opcode >= 0x70 && opcode <= 0x7f) {
			instruction.condition = static_cast<Condition>(opcode & 0xfU);
			return decode_relative(instruction,
				Operation::jump_conditional, 1);
		}
		if (opcode >= 0xb8 && opcode <= 0xbf) {
			const auto immediate_bytes = width == OperandWidth::qword ? 8U :
				static_cast<unsigned>(width);
			std::uint64_t immediate{};
			if (!read_unsigned(immediate_bytes, immediate))
				return false;
			instruction.operation = Operation::move;
			instruction.destination = register_operand(
				static_cast<std::uint8_t>(opcode & 7U),
				(prefixes_.rex & 1U) != 0, width, prefixes_.rex_present);
			instruction.source = immediate_operand(immediate, width);
			return true;
		}

		switch (opcode) {
		case 0x01: return decode_binary(instruction, Operation::add, width, false);
		case 0x03: return decode_binary(instruction, Operation::add, width, true);
		case 0x05: return decode_accumulator_immediate(instruction, Operation::add, width);
		case 0x09: return decode_binary(instruction, Operation::bit_or, width, false);
		case 0x0b: return decode_binary(instruction, Operation::bit_or, width, true);
		case 0x0d: return decode_accumulator_immediate(instruction, Operation::bit_or, width);
		case 0x21: return decode_binary(instruction, Operation::bit_and, width, false);
		case 0x23: return decode_binary(instruction, Operation::bit_and, width, true);
		case 0x25: return decode_accumulator_immediate(instruction, Operation::bit_and, width);
		case 0x29: return decode_binary(instruction, Operation::subtract, width, false);
		case 0x2b: return decode_binary(instruction, Operation::subtract, width, true);
		case 0x2d: return decode_accumulator_immediate(instruction, Operation::subtract, width);
		case 0x31: return decode_binary(instruction, Operation::bit_xor, width, false);
		case 0x33: return decode_binary(instruction, Operation::bit_xor, width, true);
		case 0x35: return decode_accumulator_immediate(instruction, Operation::bit_xor, width);
		case 0x39: return decode_binary(instruction, Operation::compare, width, false);
		case 0x3b: return decode_binary(instruction, Operation::compare, width, true);
		case 0x3d: return decode_accumulator_immediate(instruction, Operation::compare, width);
		case 0x80: return decode_immediate_group(instruction,
			OperandWidth::byte, 1, false);
		case 0x81: return decode_immediate_group(instruction, width,
			width == OperandWidth::word ? 2U : 4U,
			width == OperandWidth::qword);
		case 0x83: return decode_immediate_group(instruction, width, 1, true);
		case 0x85: return decode_binary(instruction, Operation::test, width, false);
		case 0x88: return decode_binary(instruction, Operation::move,
			OperandWidth::byte, false);
		case 0x89: return decode_binary(instruction, Operation::move, width, false);
		case 0x8a: return decode_binary(instruction, Operation::move,
			OperandWidth::byte, true);
		case 0x8b: return decode_binary(instruction, Operation::move, width, true);
		case 0x8d: {
			std::uint8_t modrm{};
			if (!read(modrm) || (modrm >> 6U) == 3)
				return false;
			instruction.operation = Operation::lea;
			instruction.destination = reg_field(modrm, width);
			return rm_operand(modrm, width, instruction.source);
		}
		case 0x90:
			instruction.operation = Operation::nop;
			return (prefixes_.rex & 1U) == 0;
		case 0xa9:
			return decode_accumulator_immediate(instruction, Operation::test, width);
		case 0xc2: {
			std::uint64_t adjustment{};
			if (!read_unsigned(2, adjustment))
				return false;
			instruction.operation = Operation::ret;
			instruction.source = immediate_operand(
				adjustment, OperandWidth::word);
			return true;
		}
		case 0xc3:
			instruction.operation = Operation::ret;
			return true;
		case 0xc6:
		case 0xc7: {
			std::uint8_t modrm{};
			if (!read(modrm) || ((modrm >> 3U) & 7U) != 0)
				return false;
			const auto move_width = opcode == 0xc6 ? OperandWidth::byte : width;
			if (!rm_operand(modrm, move_width, instruction.destination))
				return false;
			const auto bytes = opcode == 0xc6 ? 1U :
				(move_width == OperandWidth::word ? 2U : 4U);
			std::uint64_t immediate{};
			if (!read_unsigned(bytes, immediate))
				return false;
			if (move_width == OperandWidth::qword)
				immediate = sign_extend(immediate, 32);
			instruction.operation = Operation::move;
			instruction.source = immediate_operand(immediate, move_width);
			return true;
		}
		case 0xcc:
			instruction.operation = Operation::breakpoint;
			return true;
		case 0xe8: return decode_relative(instruction, Operation::call, 4);
		case 0xe9: return decode_relative(instruction, Operation::jump, 4);
		case 0xeb: return decode_relative(instruction, Operation::jump, 1);
		case 0x0f: return decode_two_byte(instruction);
		default:
			return false;
		}
	}

	ByteReader reader_;
	Prefixes prefixes_{};
	bool failed_{};
};

} // namespace

bool Instruction::terminal() const noexcept
{
	switch (operation) {
	case Operation::jump:
	case Operation::jump_conditional:
	case Operation::call:
	case Operation::ret:
	case Operation::system_call:
	case Operation::breakpoint:
		return true;
	default:
		return false;
	}
}

DecodeResult decode(
	::uml::memory::GuestAddressSpace &memory, std::uint64_t rip) noexcept
{
	try {
		return InstructionDecoder{memory, rip}.run();
	} catch (...) {
		return {DecodeStatus::fetch_fault, {}, rip};
	}
}

} // namespace uml::cpu::x86_64
