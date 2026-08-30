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
	bool vex{};
	bool vex_long{};
	std::uint8_t vex_map{};
	std::uint8_t vex_vvvv{};
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
		instruction.repeat = prefixes_.repeat;
		if (!decode_opcode(opcode, instruction))
			return failed_ ? fetch_fault() : illegal();
		instruction.length = reader_.position();
		if (prefixes_.locked && !lock_allowed(instruction))
			return illegal();
		return {DecodeStatus::decoded, instruction, 0};
	}

private:
	[[nodiscard]] static bool lock_allowed(
		const Instruction &instruction) noexcept
	{
		if (instruction.destination.kind != OperandKind::memory)
			return false;
		switch (instruction.operation) {
		case Operation::add:
		case Operation::add_carry:
		case Operation::subtract:
		case Operation::subtract_borrow:
		case Operation::increment:
		case Operation::decrement:
		case Operation::negate:
		case Operation::bit_not:
		case Operation::bit_and:
		case Operation::bit_or:
		case Operation::bit_xor:
		case Operation::exchange:
		case Operation::bit_set:
		case Operation::bit_reset:
		case Operation::bit_complement:
		case Operation::compare_exchange:
		case Operation::compare_exchange_pair:
		case Operation::exchange_add:
			return true;
		default:
			return false;
		}
	}

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

	[[nodiscard]] static constexpr Operand vector_register_operand(
		std::uint8_t index, OperandWidth width) noexcept
	{
		Operand operand{};
		operand.kind = OperandKind::vector_reg;
		operand.width = width;
		operand.reg.index = index;
		return operand;
	}

	[[nodiscard]] Operand vector_reg_field(
		std::uint8_t modrm, OperandWidth width) const noexcept
	{
		return vector_register_operand(static_cast<std::uint8_t>(
			((modrm >> 3U) & 7U) |
			((prefixes_.rex & 4U) != 0 ? 8U : 0U)), width);
	}

	[[nodiscard]] bool vector_rm_operand(
		std::uint8_t modrm, OperandWidth width, Operand &operand)
	{
		if ((modrm >> 6U) == 3) {
			operand = vector_register_operand(static_cast<std::uint8_t>(
				(modrm & 7U) |
				((prefixes_.rex & 1U) != 0 ? 8U : 0U)), width);
			return true;
		}
		return rm_operand(modrm, width, operand);
	}

	[[nodiscard]] static constexpr Operand mmx_register_operand(
		std::uint8_t index) noexcept
	{
		Operand operand{};
		operand.kind = OperandKind::mmx_reg;
		operand.width = OperandWidth::qword;
		operand.reg.index = static_cast<std::uint8_t>(index & 7U);
		return operand;
	}

	[[nodiscard]] bool mmx_rm_operand(
		std::uint8_t modrm, Operand &operand)
	{
		if ((modrm >> 6U) == 3) {
			operand = mmx_register_operand(modrm & 7U);
			return true;
		}
		return rm_operand(modrm, OperandWidth::qword, operand);
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
		operand.kind = OperandKind::memory;
		operand.width = width;
		operand.memory.segment = prefixes_.segment;
		operand.memory.address_32 = prefixes_.address_size;
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
			operand.memory.rip_relative = !prefixes_.address_size;
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
		case 2: instruction.operation = Operation::add_carry; break;
		case 3: instruction.operation = Operation::subtract_borrow; break;
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
		const auto bytes = width == OperandWidth::qword ? 4U :
			static_cast<unsigned>(width);
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

	[[nodiscard]] bool decode_shift_group(Instruction &instruction,
		std::uint8_t opcode, OperandWidth width)
	{
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		const auto group = static_cast<std::uint8_t>((modrm >> 3U) & 7U);
		switch (group) {
		case 0: instruction.operation = Operation::rotate_left; break;
		case 1: instruction.operation = Operation::rotate_right; break;
		case 2: instruction.operation = Operation::rotate_carry_left; break;
		case 3: instruction.operation = Operation::rotate_carry_right; break;
		case 4:
		case 6: instruction.operation = Operation::shift_left; break;
		case 5: instruction.operation = Operation::shift_right; break;
		case 7: instruction.operation = Operation::shift_arithmetic_right; break;
		default: return false;
		}
		if (!rm_operand(modrm, width, instruction.destination))
			return false;
		if (opcode == 0xc0 || opcode == 0xc1) {
			std::uint64_t immediate{};
			if (!read_unsigned(1, immediate))
				return false;
			instruction.source = immediate_operand(
				immediate, OperandWidth::byte);
		} else if (opcode == 0xd0 || opcode == 0xd1) {
			instruction.source = immediate_operand(1, OperandWidth::byte);
		} else {
			instruction.source = register_operand(
				1, false, OperandWidth::byte, prefixes_.rex_present);
		}
		return true;
	}

	[[nodiscard]] bool decode_unary_group(Instruction &instruction,
		std::uint8_t opcode, OperandWidth ordinary)
	{
		const auto width = opcode == 0xf6 ? OperandWidth::byte : ordinary;
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		const auto group = static_cast<std::uint8_t>((modrm >> 3U) & 7U);
		if (!rm_operand(modrm, width, instruction.destination))
			return false;
		switch (group) {
		case 0: {
			const auto bytes = width == OperandWidth::byte ? 1U :
				(width == OperandWidth::word ? 2U : 4U);
			std::uint64_t immediate{};
			if (!read_unsigned(bytes, immediate))
				return false;
			if (width == OperandWidth::qword)
				immediate = sign_extend(immediate, 32);
			instruction.operation = Operation::test;
			instruction.source = immediate_operand(immediate, width);
			return true;
		}
		case 2: instruction.operation = Operation::bit_not; return true;
		case 3: instruction.operation = Operation::negate; return true;
		case 4: instruction.operation = Operation::multiply; return true;
		case 5: instruction.operation = Operation::multiply_signed; return true;
		case 6: instruction.operation = Operation::divide; return true;
		case 7: instruction.operation = Operation::divide_signed; return true;
		default: return false;
		}
	}

	[[nodiscard]] bool decode_ff_group(Instruction &instruction,
		OperandWidth width)
	{
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		const auto group = static_cast<std::uint8_t>((modrm >> 3U) & 7U);
		Operand operand{};
		switch (group) {
		case 0:
		case 1:
			if (!rm_operand(modrm, width, operand))
				return false;
			instruction.operation = group == 0 ?
				Operation::increment : Operation::decrement;
			instruction.destination = operand;
			return true;
		case 2:
		case 4:
			if (!rm_operand(modrm, OperandWidth::qword, operand))
				return false;
			instruction.operation = group == 2 ?
				Operation::call_indirect : Operation::jump_indirect;
			instruction.source = operand;
			return true;
		case 6:
			if (!rm_operand(modrm, stack_width(prefixes_), operand))
				return false;
			instruction.operation = Operation::push;
			instruction.source = operand;
			return true;
		default:
			return false;
		}
	}

	[[nodiscard]] bool decode_imul_immediate(Instruction &instruction,
		std::uint8_t opcode, OperandWidth width)
	{
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		Operand source{};
		if (!rm_operand(modrm, width, source))
			return false;
		const auto bytes = opcode == 0x6b ? 1U :
			(width == OperandWidth::word ? 2U : 4U);
		std::uint64_t immediate{};
		if (!read_unsigned(bytes, immediate))
			return false;
		instruction.operation = Operation::multiply_signed;
		instruction.destination = reg_field(modrm, width);
		instruction.source = source;
		instruction.auxiliary = immediate_operand(
			sign_extend(immediate, bytes * 8U), width);
		return true;
	}

	[[nodiscard]] bool decode_moffs(Instruction &instruction,
		std::uint8_t opcode, OperandWidth width)
	{
		std::uint64_t address{};
		if (!read_unsigned(prefixes_.address_size ? 4U : 8U, address))
			return false;
		Operand memory{};
		memory.kind = OperandKind::memory;
		memory.width = (opcode == 0xa0 || opcode == 0xa2) ?
			OperandWidth::byte : width;
		memory.memory.segment = prefixes_.segment;
		memory.memory.address_32 = prefixes_.address_size;
		memory.memory.absolute_address = true;
		memory.memory.absolute = prefixes_.address_size ?
			static_cast<std::uint32_t>(address) : address;
		const auto accumulator = register_operand(
			0, false, memory.width, prefixes_.rex_present);
		instruction.operation = Operation::move;
		const bool load = opcode == 0xa0 || opcode == 0xa1;
		instruction.destination = load ? accumulator : memory;
		instruction.source = load ? memory : accumulator;
		return true;
	}

	[[nodiscard]] bool decode_push_immediate(Instruction &instruction,
		std::uint8_t opcode)
	{
		const auto width = stack_width(prefixes_);
		const auto bytes = opcode == 0x6a ? 1U :
			(width == OperandWidth::word ? 2U : 4U);
		std::uint64_t immediate{};
		if (!read_unsigned(bytes, immediate))
			return false;
		instruction.operation = Operation::push;
		instruction.source = immediate_operand(
			sign_extend(immediate, bytes * 8U), width);
		return true;
	}

	[[nodiscard]] Operand string_memory(std::uint8_t base,
		OperandWidth width, Segment segment) const noexcept
	{
		Operand operand{};
		operand.kind = OperandKind::memory;
		operand.width = width;
		operand.memory.base = base;
		operand.memory.segment = segment;
		operand.memory.address_32 = prefixes_.address_size;
		return operand;
	}

	[[nodiscard]] bool decode_string(Instruction &instruction,
		std::uint8_t opcode, OperandWidth ordinary)
	{
		const auto width = (opcode & 1U) == 0 ?
			OperandWidth::byte : ordinary;
		const auto source = string_memory(6, width, prefixes_.segment);
		const auto destination = string_memory(7, width, Segment::none);
		const auto accumulator = register_operand(
			0, false, width, prefixes_.rex_present);
		switch (opcode) {
		case 0xa4:
		case 0xa5:
			instruction.operation = Operation::string_move;
			instruction.destination = destination;
			instruction.source = source;
			return true;
		case 0xa6:
		case 0xa7:
			instruction.operation = Operation::string_compare;
			instruction.destination = source;
			instruction.source = destination;
			return true;
		case 0xaa:
		case 0xab:
			instruction.operation = Operation::string_store;
			instruction.destination = destination;
			instruction.source = accumulator;
			return true;
		case 0xac:
		case 0xad:
			instruction.operation = Operation::string_load;
			instruction.destination = accumulator;
			instruction.source = source;
			return true;
		case 0xae:
		case 0xaf:
			instruction.operation = Operation::string_scan;
			instruction.destination = accumulator;
			instruction.source = destination;
			return true;
		default:
			return false;
		}
	}

	[[nodiscard]] bool decode_double_shift(Instruction &instruction,
		std::uint8_t opcode, OperandWidth width)
	{
		std::uint8_t modrm{};
		if (!read(modrm) || !rm_operand(
			modrm, width, instruction.destination))
			return false;
		instruction.operation = opcode == 0xa4 || opcode == 0xa5 ?
			Operation::double_shift_left : Operation::double_shift_right;
		instruction.source = reg_field(modrm, width);
		if (opcode == 0xa4 || opcode == 0xac) {
			std::uint64_t count{};
			if (!read_unsigned(1, count))
				return false;
			instruction.auxiliary = immediate_operand(
				count, OperandWidth::byte);
		} else {
			instruction.auxiliary = register_operand(
				1, false, OperandWidth::byte, prefixes_.rex_present);
		}
		return true;
	}

	[[nodiscard]] bool decode_bit_register(Instruction &instruction,
		Operation operation, OperandWidth width)
	{
		return decode_binary(instruction, operation, width, false);
	}

	[[nodiscard]] bool decode_bit_immediate(Instruction &instruction,
		OperandWidth width)
	{
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		const auto group = static_cast<std::uint8_t>((modrm >> 3U) & 7U);
		switch (group) {
		case 4: instruction.operation = Operation::bit_test; break;
		case 5: instruction.operation = Operation::bit_set; break;
		case 6: instruction.operation = Operation::bit_reset; break;
		case 7: instruction.operation = Operation::bit_complement; break;
		default: return false;
		}
		if (!rm_operand(modrm, width, instruction.destination))
			return false;
		std::uint64_t bit{};
		if (!read_unsigned(1, bit))
			return false;
		instruction.source = immediate_operand(bit, OperandWidth::byte);
		return true;
	}

	[[nodiscard]] bool decode_cmpxchg_pair(Instruction &instruction,
		std::uint8_t modrm)
	{
		if ((modrm >> 6U) == 3 || ((modrm >> 3U) & 7U) != 1 ||
			!rm_operand(modrm, OperandWidth::qword,
				instruction.destination))
			return false;
		instruction.operation = Operation::compare_exchange_pair;
		instruction.auxiliary = immediate_operand(
			(prefixes_.rex & 8U) != 0 ? 16U : 8U,
			OperandWidth::byte);
		return true;
	}

	[[nodiscard]] bool decode_c7_group(Instruction &instruction)
	{
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		const auto group = static_cast<std::uint8_t>((modrm >> 3U) & 7U);
		if (group == 1)
			return decode_cmpxchg_pair(instruction, modrm);
		if ((group != 6 && group != 7) || (modrm >> 6U) != 3)
			return false;
		const auto width = ordinary_width(prefixes_);
		if (!rm_operand(modrm, width, instruction.destination))
			return false;
		if (group == 7 && prefixes_.repeat == 0xf3) {
			instruction.operation = Operation::read_processor_id;
		} else {
			instruction.operation = Operation::random;
			instruction.auxiliary = immediate_operand(
				group, OperandWidth::byte);
		}
		return true;
	}

	[[nodiscard]] bool decode_0f01(Instruction &instruction)
	{
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		if (modrm == 0xd0) {
			instruction.operation = Operation::read_extended_control;
			return true;
		}
		if (modrm == 0xf9) {
			instruction.operation = Operation::read_timestamp;
			instruction.auxiliary = immediate_operand(1, OperandWidth::byte);
			return true;
		}
		if (modrm == 0xfc) {
			instruction.operation = Operation::cache_zero;
			return true;
		}
		const auto group = static_cast<std::uint8_t>((modrm >> 3U) & 7U);
		if (group == 0) {
			if ((modrm >> 6U) == 3 || !rm_operand(modrm,
				OperandWidth::qword, instruction.destination))
				return false;
			instruction.operation = Operation::store_gdtr;
			return true;
		}
		if (group == 4) {
			if (!rm_operand(modrm, ordinary_width(prefixes_),
				instruction.destination))
				return false;
			if (instruction.destination.kind == OperandKind::memory)
				instruction.destination.width = OperandWidth::word;
			instruction.operation = Operation::store_machine_status;
			return true;
		}
		return false;
	}

	[[nodiscard]] bool decode_0fae(Instruction &instruction)
	{
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		const auto group = static_cast<std::uint8_t>((modrm >> 3U) & 7U);
		if (prefixes_.repeat == 0xf3 && (modrm >> 6U) == 3 && group <= 3) {
			const auto width = (prefixes_.rex & 8U) != 0 ?
				OperandWidth::qword : OperandWidth::dword;
			Operand operand{};
			if (!rm_operand(modrm, width, operand))
				return false;
			switch (group) {
			case 0: instruction.operation = Operation::read_fs_base; break;
			case 1: instruction.operation = Operation::read_gs_base; break;
			case 2: instruction.operation = Operation::write_fs_base; break;
			case 3: instruction.operation = Operation::write_gs_base; break;
			default: return false;
			}
			if (group <= 1)
				instruction.destination = operand;
			else
				instruction.source = operand;
			return true;
		}
		if (modrm == 0xe8 || modrm == 0xf0 || modrm == 0xf8) {
			instruction.operation = Operation::memory_fence;
			instruction.auxiliary = immediate_operand(
				group, OperandWidth::byte);
			return true;
		}
		if ((group == 2 || group == 3) && (modrm >> 6U) != 3 &&
			prefixes_.repeat == 0 && !prefixes_.operand_size) {
			if (!rm_operand(modrm, OperandWidth::dword,
				instruction.destination))
				return false;
			instruction.operation = Operation::mxcsr;
			instruction.vector_group = group;
			return true;
		}
		if ((group == 6 || group == 7) && (modrm >> 6U) != 3) {
			if (!rm_operand(modrm, OperandWidth::byte,
				instruction.destination))
				return false;
			instruction.operation = Operation::cache_flush;
			return true;
		}
		return false;
	}

	[[nodiscard]] bool decode_vector_scalar_convert(
		Instruction &instruction, std::uint8_t opcode)
	{
		const auto prefix = mandatory_prefix();
		if (prefixes_.vex && prefix < 2)
			return false;
		const bool to_float = opcode == 0x2a;
		const bool scalar = prefix >= 2;
		const auto integer_width = scalar && (prefixes_.rex & 8U) != 0 ?
			OperandWidth::qword : OperandWidth::dword;
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		instruction.operation = Operation::vector_scalar_convert;
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = opcode;
		instruction.vector_map = 1;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		if (to_float) {
			instruction.destination = vector_reg_field(
				modrm, OperandWidth::xmm);
			instruction.source = scalar ?
				(prefixes_.vex ? vector_register_operand(
					prefixes_.vex_vvvv, OperandWidth::xmm) :
					instruction.destination) : instruction.destination;
			if (scalar)
				return rm_operand(modrm, integer_width,
					instruction.auxiliary);
			return mmx_rm_operand(modrm, instruction.auxiliary);
		}

		if (scalar) {
			instruction.destination = reg_field(modrm, integer_width);
			const auto source_width = prefix == 2 ?
				OperandWidth::dword : OperandWidth::qword;
			return vector_rm_operand(modrm, source_width,
				instruction.auxiliary);
		}
		instruction.destination = mmx_register_operand(
			(modrm >> 3U) & 7U);
		const auto source_width = prefix == 0 ?
			OperandWidth::qword : OperandWidth::xmm;
		return vector_rm_operand(modrm, source_width,
			instruction.auxiliary);
	}

	[[nodiscard]] bool decode_vector_compare_flags(
		Instruction &instruction, std::uint8_t opcode)
	{
		const auto prefix = mandatory_prefix();
		if (prefix > 1)
			return false;
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		const auto scalar_width = prefix == 1 ?
			OperandWidth::qword : OperandWidth::dword;
		instruction.operation = Operation::vector_compare_flags;
		instruction.source = vector_reg_field(modrm, OperandWidth::xmm);
		if (!vector_rm_operand(modrm, scalar_width, instruction.auxiliary))
			return false;
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = opcode;
		instruction.vector_map = 1;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		return true;
	}

	[[nodiscard]] bool decode_vector_non_temporal_store(
		Instruction &instruction, std::uint8_t opcode)
	{
		const auto prefix = mandatory_prefix();
		if (opcode == 0x2b && prefix > 3)
			return false;
		if (opcode == 0xe7 && prefix != 0 && prefix != 1)
			return false;
		if (prefixes_.vex && opcode == 0xe7 && prefix != 1)
			return false;
		const bool mmx = opcode == 0xe7 && !prefixes_.vex && prefix == 0;
		const bool scalar = opcode == 0x2b && prefix >= 2;
		const auto width = mmx ? OperandWidth::qword :
			(scalar ? (prefix == 2 ? OperandWidth::dword :
				OperandWidth::qword) :
				(prefixes_.vex && prefixes_.vex_long ?
					OperandWidth::ymm : OperandWidth::xmm));
		std::uint8_t modrm{};
		if (!read(modrm) || (modrm >> 6U) == 3 ||
			!rm_operand(modrm, width, instruction.destination))
			return false;
		instruction.operation = Operation::vector_move;
		instruction.source = mmx ?
			mmx_register_operand((modrm >> 3U) & 7U) :
			vector_reg_field(modrm, width);
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = opcode;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		return true;
	}

	[[nodiscard]] bool decode_vector_d6(Instruction &instruction)
	{
		const auto prefix = mandatory_prefix();
		if (prefix == 0 || (prefixes_.vex && prefix != 1) ||
			(prefix != 1 && prefix != 2 && prefix != 3))
			return false;
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		instruction.operation = Operation::vector_move;
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = 0xd6;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		if (prefix == 2) {
			instruction.destination = vector_reg_field(
				modrm, OperandWidth::xmm);
			return mmx_rm_operand(modrm, instruction.source);
		}
		if (prefix == 3) {
			instruction.destination = mmx_register_operand(
				(modrm >> 3U) & 7U);
			return vector_rm_operand(modrm, OperandWidth::qword,
				instruction.source);
		}
		instruction.source = vector_reg_field(modrm, OperandWidth::qword);
		if ((modrm >> 6U) == 3) {
			instruction.destination = vector_register_operand(
				static_cast<std::uint8_t>((modrm & 7U) |
					((prefixes_.rex & 1U) != 0 ? 8U : 0U)),
				OperandWidth::xmm);
			return true;
		}
		return rm_operand(modrm, OperandWidth::qword,
			instruction.destination);
	}

	[[nodiscard]] bool decode_vector_mask_store(Instruction &instruction)
	{
		const auto prefix = mandatory_prefix();
		if (prefix != 0 && prefix != 1)
			return false;
		if (prefixes_.vex && (prefix != 1 || prefixes_.vex_long))
			return false;
		std::uint8_t modrm{};
		if (!read(modrm) || (modrm >> 6U) != 3)
			return false;
		const bool mmx = !prefixes_.vex && prefix == 0;
		const auto width = mmx ? OperandWidth::qword : OperandWidth::xmm;
		instruction.operation = Operation::vector_mask_store;
		instruction.source = mmx ?
			mmx_register_operand((modrm >> 3U) & 7U) :
			vector_reg_field(modrm, width);
		instruction.auxiliary = mmx ? mmx_register_operand(modrm & 7U) :
			vector_register_operand(static_cast<std::uint8_t>(
				(modrm & 7U) |
				((prefixes_.rex & 1U) != 0 ? 8U : 0U)), width);
		instruction.vex = prefixes_.vex;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		return true;
	}

	[[nodiscard]] unsigned mandatory_prefix() const noexcept
	{
		if (prefixes_.repeat == 0xf3)
			return 2;
		if (prefixes_.repeat == 0xf2)
			return 3;
		return prefixes_.operand_size ? 1U : 0U;
	}

	[[nodiscard]] bool decode_vector_move(Instruction &instruction,
		std::uint8_t opcode)
	{
		const auto prefix = mandatory_prefix();
		if (opcode == 0xf0 && prefix != 3)
			return false;
		const bool mmx = !prefixes_.vex && prefix == 0 &&
			(opcode == 0x6f || opcode == 0x7f);
		if ((opcode == 0x28 || opcode == 0x29) && prefix > 1)
			return false;
		if ((opcode == 0x6f || opcode == 0x7f) &&
			prefix != 1 && prefix != 2 && !mmx)
			return false;
		if (prefixes_.vex && (opcode == 0x6f || opcode == 0x7f) &&
			prefix == 0)
			return false;
		const bool scalar = (opcode == 0x10 || opcode == 0x11) &&
			(prefix == 2 || prefix == 3);
		const auto width = scalar ?
			(prefix == 2 ? OperandWidth::dword : OperandWidth::qword) :
			(prefixes_.vex && prefixes_.vex_long ?
				OperandWidth::ymm : OperandWidth::xmm);
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		const bool load = opcode == 0x10 || opcode == 0x28 ||
			opcode == 0x6f || opcode == 0xf0;
		Operand rm{};
		Operand reg{};
		if (mmx) {
			if (!mmx_rm_operand(modrm, rm))
				return false;
			reg = mmx_register_operand((modrm >> 3U) & 7U);
		} else {
			if (!vector_rm_operand(modrm, width, rm))
				return false;
			reg = vector_reg_field(modrm, width);
		}
		instruction.operation = scalar ?
			Operation::vector_move_scalar : Operation::vector_move;
		instruction.destination = load ? reg : rm;
		instruction.source = load ? rm : reg;
		instruction.vex = prefixes_.vex;
		if (scalar && prefixes_.vex) {
			const bool register_form = rm.kind == OperandKind::vector_reg;
			if (register_form)
				instruction.auxiliary = vector_register_operand(
					prefixes_.vex_vvvv, OperandWidth::xmm);
		}
		return true;
	}

	[[nodiscard]] bool decode_vector_alu(Instruction &instruction,
		std::uint8_t opcode)
	{
		const auto prefix = mandatory_prefix();
		const bool packed_misc = opcode == 0x14 || opcode == 0x15 ||
			opcode == 0x7c || opcode == 0x7d || opcode == 0xc6 ||
			opcode == 0xd0;
		if ((opcode == 0x14 || opcode == 0x15 || opcode == 0xc6) &&
			prefix > 1)
			return false;
		if ((opcode == 0x7c || opcode == 0x7d || opcode == 0xd0) &&
			prefix != 1 && prefix != 3)
			return false;
		if (opcode >= 0x54 && opcode <= 0x57 && prefix > 1)
			return false;
		if ((opcode == 0x52 || opcode == 0x53) &&
			prefix != 0 && prefix != 2)
			return false;
		const bool scalar = !packed_misc && (prefix == 2 || prefix == 3);
		const auto vector_width = scalar ? OperandWidth::xmm :
			(prefixes_.vex && prefixes_.vex_long ?
				OperandWidth::ymm : OperandWidth::xmm);
		const auto source_width = scalar ?
			(prefix == 2 ? OperandWidth::dword : OperandWidth::qword) :
			vector_width;
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		Operand second{};
		if (!vector_rm_operand(modrm, source_width, second))
			return false;
		instruction.operation = Operation::vector_alu;
		instruction.destination = vector_reg_field(modrm, vector_width);
		instruction.source = prefixes_.vex ?
			vector_register_operand(prefixes_.vex_vvvv, vector_width) :
			instruction.destination;
		instruction.auxiliary = second;
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = opcode;
		instruction.vector_map = 1;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		if (opcode == 0xc2 || opcode == 0xc6) {
			std::uint64_t control{};
			if (!read_unsigned(1, control))
				return false;
			instruction.vector_control = static_cast<std::uint8_t>(control);
		}
		return true;
	}

	[[nodiscard]] bool decode_vector_lane_move(Instruction &instruction,
		std::uint8_t opcode)
	{
		const auto prefix = mandatory_prefix();
		const bool duplicate = (opcode == 0x12 &&
			(prefix == 2 || prefix == 3)) ||
			(opcode == 0x16 && prefix == 2);
		if ((opcode == 0x13 || opcode == 0x17) && prefix > 1)
			return false;
		if (opcode == 0x16 && prefix == 3)
			return false;
		const auto vector_width = duplicate && prefixes_.vex &&
			prefixes_.vex_long ? OperandWidth::ymm : OperandWidth::xmm;
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		if (opcode == 0x13 || opcode == 0x17) {
			if ((modrm >> 6U) == 3 || !rm_operand(
				modrm, OperandWidth::qword, instruction.destination))
				return false;
			instruction.source = vector_reg_field(
				modrm, OperandWidth::xmm);
		} else {
			Operand rm{};
			const auto rm_width = duplicate ?
				(prefix == 3 && (modrm >> 6U) != 3 &&
					vector_width == OperandWidth::xmm ?
					OperandWidth::qword : vector_width) :
				((modrm >> 6U) == 3 ? OperandWidth::xmm :
					OperandWidth::qword);
			if (!vector_rm_operand(modrm, rm_width, rm))
				return false;
			instruction.destination = vector_reg_field(modrm, vector_width);
			instruction.source = prefixes_.vex ?
				vector_register_operand(prefixes_.vex_vvvv,
					OperandWidth::xmm) : instruction.destination;
			instruction.auxiliary = rm;
		}
		instruction.operation = Operation::vector_lane_move;
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = opcode;
		instruction.vector_map = 1;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		return true;
	}

	[[nodiscard]] bool decode_vector_convert(Instruction &instruction,
		std::uint8_t opcode)
	{
		const auto prefix = mandatory_prefix();
		if ((opcode == 0x5b && prefix == 3) ||
			(opcode == 0xe6 && prefix == 0))
			return false;
		const auto long_vector = prefixes_.vex && prefixes_.vex_long;
		OperandWidth destination_width = long_vector ?
			OperandWidth::ymm : OperandWidth::xmm;
		OperandWidth source_width = destination_width;
		bool scalar{};
		if (opcode == 0x5a) {
			if (prefix == 0) {
				source_width = destination_width == OperandWidth::ymm ?
					OperandWidth::xmm : OperandWidth::qword;
			} else if (prefix == 1) {
				source_width = long_vector ?
					OperandWidth::ymm : OperandWidth::xmm;
				destination_width = OperandWidth::xmm;
			} else {
				scalar = true;
				destination_width = OperandWidth::xmm;
				source_width = prefix == 2 ?
					OperandWidth::dword : OperandWidth::qword;
			}
		} else if (opcode == 0xe6) {
			if (prefix == 2) {
				source_width = destination_width == OperandWidth::ymm ?
					OperandWidth::xmm : OperandWidth::qword;
			} else {
				source_width = long_vector ?
					OperandWidth::ymm : OperandWidth::xmm;
				destination_width = OperandWidth::xmm;
			}
		}
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		Operand rm{};
		if (!vector_rm_operand(modrm, source_width, rm))
			return false;
		instruction.operation = Operation::vector_convert;
		instruction.destination = vector_reg_field(
			modrm, destination_width);
		if (scalar)
			instruction.source = prefixes_.vex ?
				vector_register_operand(prefixes_.vex_vvvv,
					OperandWidth::xmm) : instruction.destination;
		instruction.auxiliary = rm;
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = opcode;
		instruction.vector_map = 1;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		return true;
	}

	[[nodiscard]] bool decode_vector_gpr_move(Instruction &instruction,
		std::uint8_t opcode)
	{
		const auto prefix = mandatory_prefix();
		if (opcode == 0x6e && prefix != 0 && prefix != 1)
			return false;
		if (opcode == 0x7e && prefix == 3)
			return false;
		if (prefixes_.vex && prefix != 1 &&
			!(opcode == 0x7e && prefix == 2))
			return false;
		const auto scalar_width = (prefixes_.rex & 8U) != 0 ?
			OperandWidth::qword : OperandWidth::dword;
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		instruction.operation = Operation::vector_gpr_move;
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = opcode;
		instruction.vector_map = 1;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		if (opcode == 0x7e && prefix == 2) {
			Operand source{};
			if (!vector_rm_operand(modrm, OperandWidth::qword, source))
				return false;
			instruction.destination = vector_reg_field(
				modrm, OperandWidth::xmm);
			instruction.source = source;
			return true;
		}
		const bool mmx = !prefixes_.vex && prefix == 0;
		const auto simd = mmx ?
			mmx_register_operand((modrm >> 3U) & 7U) :
			vector_reg_field(modrm, OperandWidth::xmm);
		Operand general{};
		if (!rm_operand(modrm, scalar_width, general))
			return false;
		if (opcode == 0x6e) {
			instruction.destination = simd;
			instruction.source = general;
		} else {
			instruction.destination = general;
			instruction.source = simd;
		}
		return true;
	}

	[[nodiscard]] static bool packed_integer_opcode(
		std::uint8_t opcode) noexcept
	{
		return (opcode >= 0x60 && opcode <= 0x6d) || opcode == 0x70 ||
			(opcode >= 0x74 && opcode <= 0x76) ||
			(opcode >= 0xd1 && opcode <= 0xd5) ||
			(opcode >= 0xd8 && opcode <= 0xe5) ||
			(opcode >= 0xe8 && opcode <= 0xef) ||
			(opcode >= 0xf1 && opcode <= 0xf6) ||
			(opcode >= 0xf8 && opcode <= 0xfe);
	}

	[[nodiscard]] bool decode_vector_integer(Instruction &instruction,
		std::uint8_t opcode)
	{
		const auto prefix = mandatory_prefix();
		const bool mmx = !prefixes_.vex && prefix == 0;
		if (prefixes_.vex && prefix != 1 && opcode != 0x70)
			return false;
		if (!prefixes_.vex && prefix > 1 && opcode != 0x70)
			return false;
		if ((opcode == 0x6c || opcode == 0x6d) && mmx)
			return false;
		const auto width = mmx ? OperandWidth::qword :
			(prefixes_.vex && prefixes_.vex_long ?
				OperandWidth::ymm : OperandWidth::xmm);
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		Operand rm{};
		Operand reg{};
		if (mmx) {
			if (!mmx_rm_operand(modrm, rm))
				return false;
			reg = mmx_register_operand((modrm >> 3U) & 7U);
		} else {
			if (!vector_rm_operand(modrm, width, rm))
				return false;
			reg = vector_reg_field(modrm, width);
		}
		instruction.operation = Operation::vector_integer;
		instruction.destination = reg;
		instruction.source = prefixes_.vex ?
			vector_register_operand(prefixes_.vex_vvvv, width) : reg;
		instruction.auxiliary = rm;
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = opcode;
		instruction.vector_map = 1;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		if (opcode == 0x70) {
			std::uint64_t control{};
			if (!read_unsigned(1, control))
				return false;
			instruction.vector_control = static_cast<std::uint8_t>(control);
		}
		return true;
	}

	[[nodiscard]] bool decode_segment_move(Instruction &instruction,
		std::uint8_t opcode)
	{
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		const auto segment = static_cast<std::uint8_t>(
			(modrm >> 3U) & 7U);
		if (segment > 5 || (opcode == 0x8e && segment == 1))
			return false;
		instruction.operation = Operation::segment_move;
		instruction.vector_opcode = opcode;
		instruction.vector_group = segment;
		if (opcode == 0x8e)
			return rm_operand(modrm, OperandWidth::word,
				instruction.source);
		const auto width = (modrm >> 6U) == 3 ?
			ordinary_width(prefixes_) : OperandWidth::word;
		return rm_operand(modrm, width, instruction.destination);
	}

	[[nodiscard]] bool decode_segment_stack(Instruction &instruction,
		std::uint8_t opcode)
	{
		instruction.operation = Operation::segment_stack;
		instruction.vector_opcode = opcode;
		instruction.vector_group = opcode >= 0xa8 ? 5U : 4U;
		if (opcode == 0xa0 || opcode == 0xa8)
			instruction.source.width = stack_width(prefixes_);
		else
			instruction.destination.width = stack_width(prefixes_);
		return true;
	}

	[[nodiscard]] static bool packed_map2_binary_opcode(
		std::uint8_t opcode) noexcept
	{
		return opcode <= 0x0b || opcode == 0x10 || opcode == 0x14 ||
			opcode == 0x15 || opcode == 0x28 || opcode == 0x29 ||
			opcode == 0x2b || (opcode >= 0x37 && opcode <= 0x40);
	}

	[[nodiscard]] static bool packed_map2_extend_opcode(
		std::uint8_t opcode) noexcept
	{
		return (opcode >= 0x20 && opcode <= 0x25) ||
			(opcode >= 0x30 && opcode <= 0x35);
	}

	[[nodiscard]] bool decode_vector_map2(Instruction &instruction,
		std::uint8_t opcode)
	{
		const auto prefix = mandatory_prefix();
		if (!prefixes_.vex && (opcode == 0xf0 || opcode == 0xf1)) {
			std::uint8_t modrm{};
			if (!read(modrm))
				return false;
			if (prefixes_.repeat == 0xf2) {
				const auto destination_width = (prefixes_.rex & 8U) != 0 ?
					OperandWidth::qword : OperandWidth::dword;
				const auto source_width = opcode == 0xf0 ?
					OperandWidth::byte : ((prefixes_.rex & 8U) != 0 ?
						OperandWidth::qword : (prefixes_.operand_size ?
							OperandWidth::word : OperandWidth::dword));
				instruction.operation = Operation::crc32;
				instruction.destination = reg_field(modrm, destination_width);
				return rm_operand(modrm, source_width, instruction.source);
			}
			if (prefixes_.repeat != 0)
				return false;
			const auto width = ordinary_width(prefixes_);
			Operand memory{};
			if ((modrm >> 6U) == 3 || !rm_operand(modrm, width, memory))
				return false;
			instruction.operation = Operation::move_byte_swap;
			const auto reg = reg_field(modrm, width);
			instruction.destination = opcode == 0xf0 ? reg : memory;
			instruction.source = opcode == 0xf0 ? memory : reg;
			return true;
		}
		if (!prefixes_.vex && opcode == 0xf6) {
			const bool adox = prefixes_.repeat == 0xf3;
			if (!adox && (prefixes_.repeat != 0 || !prefixes_.operand_size))
				return false;
			const auto width = (prefixes_.rex & 8U) != 0 ?
				OperandWidth::qword : OperandWidth::dword;
			std::uint8_t modrm{};
			if (!read(modrm) || !rm_operand(
				modrm, width, instruction.source))
				return false;
			instruction.operation = Operation::adx;
			instruction.destination = reg_field(modrm, width);
			instruction.vector_control = adox ? 1U : 0U;
			return true;
		}
		if (opcode >= 0xdb && opcode <= 0xdf) {
			if (prefix != 1 || (opcode == 0xdb &&
				prefixes_.vex && prefixes_.vex_long))
				return false;
			const auto width = prefixes_.vex && prefixes_.vex_long ?
				OperandWidth::ymm : OperandWidth::xmm;
			std::uint8_t modrm{};
			if (!read(modrm))
				return false;
			instruction.operation = Operation::vector_aes;
			instruction.destination = vector_reg_field(modrm, width);
			instruction.source = opcode == 0xdb ? Operand{} :
				(prefixes_.vex ? vector_register_operand(
					prefixes_.vex_vvvv, width) : instruction.destination);
			if (!vector_rm_operand(modrm, width,
				instruction.auxiliary))
				return false;
			instruction.vex = prefixes_.vex;
			instruction.vector_opcode = opcode;
			instruction.vector_map = 2;
			instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
			return true;
		}
		if (opcode >= 0xc8 && opcode <= 0xcd) {
			if (prefixes_.vex || prefix != 0)
				return false;
			std::uint8_t modrm{};
			if (!read(modrm))
				return false;
			instruction.operation = Operation::vector_sha;
			instruction.destination = vector_reg_field(
				modrm, OperandWidth::xmm);
			instruction.source = instruction.destination;
			if (!vector_rm_operand(modrm, OperandWidth::xmm,
				instruction.auxiliary))
				return false;
			instruction.vector_opcode = opcode;
			instruction.vector_map = 2;
			return true;
		}
		const bool mmx_opcode = opcode <= 0x0b ||
			(opcode >= 0x1c && opcode <= 0x1e);
		const bool mmx = !prefixes_.vex && prefix == 0 && mmx_opcode;
		if (prefixes_.vex && prefix != 1)
			return false;
		if (!prefixes_.vex && prefix != 1 && !mmx)
			return false;
		const auto width = mmx ? OperandWidth::qword :
			(prefixes_.vex && prefixes_.vex_long ?
				OperandWidth::ymm : OperandWidth::xmm);
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = opcode;
		instruction.vector_map = 2;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);

		if (opcode == 0x2a) {
			if (mmx || (modrm >> 6U) == 3 ||
				!rm_operand(modrm, width, instruction.source))
				return false;
			instruction.operation = Operation::vector_move;
			instruction.destination = vector_reg_field(modrm, width);
			return true;
		}
		if (opcode == 0x17) {
			if (mmx)
				return false;
			instruction.operation = Operation::vector_test;
			instruction.source = vector_reg_field(modrm, width);
			return vector_rm_operand(modrm, width,
				instruction.auxiliary);
		}

		const bool unary = (opcode >= 0x1c && opcode <= 0x1e) ||
			packed_map2_extend_opcode(opcode) || opcode == 0x41;
		if (unary) {
			if (opcode == 0x41 && (mmx || width != OperandWidth::xmm))
				return false;
			instruction.operation = Operation::vector_integer_map2;
			instruction.destination = mmx ?
				mmx_register_operand((modrm >> 3U) & 7U) :
				vector_reg_field(modrm, width);
			OperandWidth source_width = width;
			if (packed_map2_extend_opcode(opcode)) {
				const auto output_size = static_cast<unsigned>(width);
				constexpr std::array<unsigned, 6> divisors{
					2, 4, 8, 2, 4, 2};
				const auto divisor = divisors[opcode & 7U];
				source_width = static_cast<OperandWidth>(output_size / divisor);
			}
			if (mmx)
				return mmx_rm_operand(modrm, instruction.auxiliary);
			return vector_rm_operand(modrm, source_width,
				instruction.auxiliary);
		}

		if (!packed_map2_binary_opcode(opcode))
			return false;
		if ((opcode == 0x10 || opcode == 0x14 || opcode == 0x15) &&
			prefixes_.vex)
			return false;
		Operand rm{};
		Operand reg{};
		if (mmx) {
			if (!mmx_rm_operand(modrm, rm))
				return false;
			reg = mmx_register_operand((modrm >> 3U) & 7U);
		} else {
			if (!vector_rm_operand(modrm, width, rm))
				return false;
			reg = vector_reg_field(modrm, width);
		}
		instruction.operation = Operation::vector_integer_map2;
		instruction.destination = reg;
		instruction.source = prefixes_.vex ?
			vector_register_operand(prefixes_.vex_vvvv, width) : reg;
		instruction.auxiliary = rm;
		return true;
	}

	[[nodiscard]] bool decode_vector_map3(Instruction &instruction,
		std::uint8_t opcode)
	{
		const auto prefix = mandatory_prefix();
		const bool legacy_mmx = !prefixes_.vex && prefix == 0 && opcode == 0x0f;
		const bool legacy_sha = !prefixes_.vex && prefix == 0 && opcode == 0xcc;
		if (prefixes_.vex) {
			if (prefix != 1 || opcode == 0xcc ||
				((opcode >= 0x60 && opcode <= 0x63) &&
					(prefixes_.vex_long || prefixes_.vex_vvvv != 0)))
				return false;
		} else if (prefix != 1 && !legacy_mmx && !legacy_sha) {
			return false;
		}

		const bool supported = opcode <= 0x02 ||
			(opcode >= 0x04 && opcode <= 0x0f) ||
			(opcode >= 0x14 && opcode <= 0x19) || opcode == 0x1d ||
			(opcode >= 0x20 && opcode <= 0x22) ||
			(opcode == 0x38 || opcode == 0x39) ||
			(opcode >= 0x40 && opcode <= 0x44) || opcode == 0x46 ||
			(opcode >= 0x4a && opcode <= 0x4c) ||
			(opcode >= 0x60 && opcode <= 0x63) || opcode == 0xcc ||
			opcode == 0xdf;
		if (!supported)
			return false;
		if (!prefixes_.vex && (opcode <= 0x06 || opcode == 0x18 ||
			opcode == 0x19 || opcode == 0x1d || opcode == 0x38 ||
			opcode == 0x39 || opcode == 0x46 ||
			(opcode >= 0x4a && opcode <= 0x4c)))
			return false;

		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		const auto vector_width = prefixes_.vex && prefixes_.vex_long ?
			OperandWidth::ymm : OperandWidth::xmm;
		instruction.operation = Operation::vector_integer_map3;
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = opcode;
		instruction.vector_map = 3;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		instruction.vector_group = (prefixes_.rex & 8U) != 0 ? 1U : 0U;

		if (opcode >= 0x14 && opcode <= 0x17) {
			if (prefixes_.vex && prefixes_.vex_long)
				return false;
			OperandWidth output_width = OperandWidth::dword;
			if (opcode == 0x14)
				output_width = (modrm >> 6U) == 3 ?
					OperandWidth::dword : OperandWidth::byte;
			else if (opcode == 0x15)
				output_width = (modrm >> 6U) == 3 ?
					OperandWidth::dword : OperandWidth::word;
			else if (opcode == 0x16 && (prefixes_.rex & 8U) != 0)
				output_width = OperandWidth::qword;
			if (!rm_operand(modrm, output_width, instruction.destination))
				return false;
			instruction.source = vector_reg_field(modrm, OperandWidth::xmm);
		} else if (opcode == 0x19 || opcode == 0x39) {
			if (!prefixes_.vex || !prefixes_.vex_long ||
				!vector_rm_operand(modrm, OperandWidth::xmm,
					instruction.destination))
				return false;
			instruction.source = vector_reg_field(modrm, OperandWidth::ymm);
		} else if (opcode == 0x1d) {
			if (!prefixes_.vex)
				return false;
			const auto output_width = prefixes_.vex_long ?
				OperandWidth::xmm : OperandWidth::qword;
			if (!vector_rm_operand(modrm, output_width,
				instruction.destination))
				return false;
			instruction.source = vector_reg_field(modrm, vector_width);
		} else if (opcode == 0x20 || opcode == 0x22) {
			if (prefixes_.vex && prefixes_.vex_long)
				return false;
			const auto scalar_width = opcode == 0x20 ?
				((modrm >> 6U) == 3 ? OperandWidth::dword : OperandWidth::byte) :
				((prefixes_.rex & 8U) != 0 ? OperandWidth::qword :
					OperandWidth::dword);
			if (!rm_operand(modrm, scalar_width, instruction.auxiliary))
				return false;
			instruction.destination = vector_reg_field(modrm, OperandWidth::xmm);
			instruction.source = prefixes_.vex ?
				vector_register_operand(prefixes_.vex_vvvv, OperandWidth::xmm) :
				instruction.destination;
		} else if (opcode == 0x21) {
			if (prefixes_.vex && prefixes_.vex_long)
				return false;
			if ((modrm >> 6U) == 3) {
				instruction.auxiliary = vector_register_operand(
					static_cast<std::uint8_t>((modrm & 7U) |
						((prefixes_.rex & 1U) != 0 ? 8U : 0U)),
					OperandWidth::xmm);
			} else if (!rm_operand(modrm, OperandWidth::dword,
				instruction.auxiliary)) {
				return false;
			}
			instruction.destination = vector_reg_field(modrm, OperandWidth::xmm);
			instruction.source = prefixes_.vex ?
				vector_register_operand(prefixes_.vex_vvvv, OperandWidth::xmm) :
				instruction.destination;
		} else if (opcode >= 0x60 && opcode <= 0x63) {
			instruction.source = vector_reg_field(modrm, OperandWidth::xmm);
			if (!vector_rm_operand(modrm, OperandWidth::xmm,
				instruction.auxiliary))
				return false;
		} else if (opcode == 0xcc) {
			instruction.destination = vector_reg_field(modrm, OperandWidth::xmm);
			instruction.source = instruction.destination;
			if (!vector_rm_operand(modrm, OperandWidth::xmm,
				instruction.auxiliary))
				return false;
		} else if (opcode == 0xdf) {
			if (prefixes_.vex && prefixes_.vex_long)
				return false;
			instruction.destination = vector_reg_field(modrm, OperandWidth::xmm);
			if (!vector_rm_operand(modrm, OperandWidth::xmm,
				instruction.auxiliary))
				return false;
		} else if (legacy_mmx) {
			instruction.destination = mmx_register_operand((modrm >> 3U) & 7U);
			instruction.source = instruction.destination;
			if (!mmx_rm_operand(modrm, instruction.auxiliary))
				return false;
		} else if (opcode == 0x18 || opcode == 0x38) {
			if (!prefixes_.vex || !prefixes_.vex_long)
				return false;
			instruction.destination = vector_reg_field(modrm, OperandWidth::ymm);
			instruction.source = vector_register_operand(
				prefixes_.vex_vvvv, OperandWidth::ymm);
			if (!vector_rm_operand(modrm, OperandWidth::xmm,
				instruction.auxiliary))
				return false;
		} else {
			instruction.destination = vector_reg_field(modrm, vector_width);
			const bool unary = opcode == 0x00 || opcode == 0x01 ||
				opcode == 0x04 || opcode == 0x05 ||
				opcode == 0x08 || opcode == 0x09;
			const bool scalar_round = opcode == 0x0a || opcode == 0x0b;
			instruction.source = unary ? Operand{} :
				(prefixes_.vex ? vector_register_operand(
					prefixes_.vex_vvvv,
					scalar_round ? OperandWidth::xmm : vector_width) :
					instruction.destination);
			const auto rm_width = scalar_round ?
				(opcode == 0x0a ? OperandWidth::dword : OperandWidth::qword) :
				vector_width;
			if (!vector_rm_operand(modrm, rm_width, instruction.auxiliary))
				return false;
		}

		std::uint64_t control{};
		if (!read_unsigned(1, control))
			return false;
		instruction.vector_control = static_cast<std::uint8_t>(control);
		return true;
	}

	[[nodiscard]] bool decode_vector_shift_immediate(
		Instruction &instruction, std::uint8_t opcode)
	{
		const auto prefix = mandatory_prefix();
		const bool mmx = !prefixes_.vex && prefix == 0;
		if ((prefixes_.vex && prefix != 1) ||
			(!prefixes_.vex && prefix > 1))
			return false;
		const auto width = mmx ? OperandWidth::qword :
			(prefixes_.vex && prefixes_.vex_long ?
				OperandWidth::ymm : OperandWidth::xmm);
		std::uint8_t modrm{};
		if (!read(modrm) || (modrm >> 6U) != 3)
			return false;
		const auto group = static_cast<std::uint8_t>((modrm >> 3U) & 7U);
		if (group != 2 && group != 6 &&
			!(group == 4 && opcode != 0x73) &&
			!(opcode == 0x73 && !mmx && (group == 3 || group == 7)))
			return false;
		Operand source = mmx ? mmx_register_operand(modrm & 7U) :
			vector_register_operand(static_cast<std::uint8_t>(
				(modrm & 7U) |
				((prefixes_.rex & 1U) != 0 ? 8U : 0U)), width);
		instruction.operation = Operation::vector_integer;
		instruction.destination = prefixes_.vex ?
			vector_register_operand(prefixes_.vex_vvvv, width) : source;
		instruction.source = source;
		instruction.auxiliary = source;
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = opcode;
		instruction.vector_map = 1;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		instruction.vector_group = group;
		std::uint64_t count{};
		if (!read_unsigned(1, count))
			return false;
		instruction.vector_control = static_cast<std::uint8_t>(count);
		return true;
	}

	[[nodiscard]] bool decode_vector_to_gpr(Instruction &instruction,
		std::uint8_t opcode)
	{
		const auto prefix = mandatory_prefix();
		if (opcode == 0x50 && prefix > 1)
			return false;
		if ((opcode == 0xc5 || opcode == 0xd7) &&
			(prefix > 1 || (prefixes_.vex && prefix != 1)))
			return false;
		std::uint8_t modrm{};
		if (!read(modrm) || (modrm >> 6U) != 3)
			return false;
		const bool mmx = opcode != 0x50 && !prefixes_.vex && prefix == 0;
		const auto width = mmx ? OperandWidth::qword :
			(prefixes_.vex && prefixes_.vex_long ?
				OperandWidth::ymm : OperandWidth::xmm);
		instruction.operation = Operation::vector_to_gpr;
		instruction.destination = reg_field(modrm, OperandWidth::dword);
		instruction.source = mmx ? mmx_register_operand(modrm & 7U) :
			vector_register_operand(static_cast<std::uint8_t>(
				(modrm & 7U) |
				((prefixes_.rex & 1U) != 0 ? 8U : 0U)), width);
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = opcode;
		instruction.vector_map = 1;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		if (opcode == 0xc5) {
			std::uint64_t control{};
			if (!read_unsigned(1, control))
				return false;
			instruction.vector_control = static_cast<std::uint8_t>(control);
		}
		return true;
	}

	[[nodiscard]] bool decode_vector_insert(Instruction &instruction)
	{
		const auto prefix = mandatory_prefix();
		if (prefix > 1 || (prefixes_.vex && prefix != 1))
			return false;
		const bool mmx = !prefixes_.vex && prefix == 0;
		const auto width = mmx ? OperandWidth::qword : OperandWidth::xmm;
		std::uint8_t modrm{};
		if (!read(modrm))
			return false;
		Operand input{};
		if (!rm_operand(modrm, OperandWidth::word, input))
			return false;
		const auto destination = mmx ?
			mmx_register_operand((modrm >> 3U) & 7U) :
			vector_reg_field(modrm, width);
		instruction.operation = Operation::vector_insert;
		instruction.destination = destination;
		instruction.source = prefixes_.vex ?
			vector_register_operand(prefixes_.vex_vvvv, width) : destination;
		instruction.auxiliary = input;
		instruction.vex = prefixes_.vex;
		instruction.vector_opcode = 0xc4;
		instruction.vector_map = 1;
		instruction.vector_prefix = static_cast<std::uint8_t>(prefix);
		std::uint64_t control{};
		if (!read_unsigned(1, control))
			return false;
		instruction.vector_control = static_cast<std::uint8_t>(control);
		return true;
	}

	[[nodiscard]] bool decode_vex(
		std::uint8_t first, Instruction &instruction)
	{
		std::uint8_t second{};
		if (!read(second))
			return false;
		std::uint8_t third{};
		std::uint8_t rex{};
		std::uint8_t map = 1;
		std::uint8_t controls = second;
		if (first == 0xc4) {
			if (!read(third))
				return false;
			map = static_cast<std::uint8_t>(second & 0x1fU);
			if ((second & 0x80U) == 0)
				rex |= 4U;
			if ((second & 0x40U) == 0)
				rex |= 2U;
			if ((second & 0x20U) == 0)
				rex |= 1U;
			if ((third & 0x80U) != 0)
				rex |= 8U;
			controls = third;
		} else if ((second & 0x80U) == 0) {
			rex |= 4U;
		}
		prefixes_.rex = rex;
		prefixes_.rex_present = true;
		prefixes_.vex = true;
		prefixes_.vex_map = map;
		prefixes_.vex_vvvv = static_cast<std::uint8_t>(
			(~controls >> 3U) & 0xfU);
		prefixes_.vex_long = (controls & 4U) != 0;
		prefixes_.operand_size = false;
		prefixes_.repeat = 0;
		switch (controls & 3U) {
		case 1: prefixes_.operand_size = true; break;
		case 2: prefixes_.repeat = 0xf3; break;
		case 3: prefixes_.repeat = 0xf2; break;
		default: break;
		}
		instruction.vex = true;
		instruction.repeat = prefixes_.repeat;
		std::uint8_t opcode{};
		if (!read(opcode))
			return false;
		if (map == 2)
			return decode_vector_map2(instruction, opcode);
		if (map == 3)
			return decode_vector_map3(instruction, opcode);
		if (map != 1)
			return false;
		switch (opcode) {
		case 0x10:
		case 0x11:
		case 0x28:
		case 0x29:
		case 0x6f:
		case 0x7f:
		case 0xf0:
			return decode_vector_move(instruction, opcode);
		case 0x6e:
		case 0x7e:
			return decode_vector_gpr_move(instruction, opcode);
		case 0x2a:
		case 0x2c:
		case 0x2d:
			return decode_vector_scalar_convert(instruction, opcode);
		case 0x2e:
		case 0x2f:
			return decode_vector_compare_flags(instruction, opcode);
		case 0x2b:
		case 0xe7:
			return decode_vector_non_temporal_store(instruction, opcode);
		case 0xd6:
			return decode_vector_d6(instruction);
		case 0xf7:
			return decode_vector_mask_store(instruction);
		case 0xae:
			return decode_0fae(instruction);
		case 0x12:
		case 0x13:
		case 0x16:
		case 0x17:
			return decode_vector_lane_move(instruction, opcode);
		case 0x77:
			instruction.operation = Operation::vector_zero;
			instruction.auxiliary = immediate_operand(
				prefixes_.vex_long ? 1U : 0U, OperandWidth::byte);
			return true;
		case 0x51:
		case 0x52:
		case 0x53:
		case 0x54:
		case 0x55:
		case 0x56:
		case 0x57:
		case 0x58:
		case 0x59:
		case 0x5c:
		case 0x5d:
		case 0x5e:
		case 0x5f:
		case 0xc2:
		case 0x14:
		case 0x15:
		case 0x7c:
		case 0x7d:
		case 0xc6:
		case 0xd0:
			return decode_vector_alu(instruction, opcode);
		case 0x50:
		case 0xc5:
		case 0xd7:
			return decode_vector_to_gpr(instruction, opcode);
		case 0xc4:
			return decode_vector_insert(instruction);
		case 0x5a:
		case 0x5b:
		case 0xe6:
			return decode_vector_convert(instruction, opcode);
		case 0x71:
		case 0x72:
		case 0x73:
			return decode_vector_shift_immediate(instruction, opcode);
		default:
			if (packed_integer_opcode(opcode))
				return decode_vector_integer(instruction, opcode);
			return false;
		}
	}

	[[nodiscard]] bool decode_two_byte(Instruction &instruction)
	{
		std::uint8_t opcode{};
		if (!read(opcode))
			return false;
		if (opcode == 0x10 || opcode == 0x11 || opcode == 0x28 ||
			opcode == 0x29 || opcode == 0x6f || opcode == 0x7f ||
			opcode == 0xf0)
			return decode_vector_move(instruction, opcode);
		if (opcode == 0x6e || opcode == 0x7e)
			return decode_vector_gpr_move(instruction, opcode);
		if (opcode == 0x2a || opcode == 0x2c || opcode == 0x2d)
			return decode_vector_scalar_convert(instruction, opcode);
		if (opcode == 0x2e || opcode == 0x2f)
			return decode_vector_compare_flags(instruction, opcode);
		if (opcode == 0x2b || opcode == 0xe7)
			return decode_vector_non_temporal_store(instruction, opcode);
		if (opcode == 0xd6)
			return decode_vector_d6(instruction);
		if (opcode == 0xf7)
			return decode_vector_mask_store(instruction);
		if (opcode == 0x12 || opcode == 0x13 ||
			opcode == 0x16 || opcode == 0x17)
			return decode_vector_lane_move(instruction, opcode);
		if (opcode == 0x51 || opcode == 0x52 || opcode == 0x53 ||
			(opcode >= 0x54 && opcode <= 0x59) ||
			(opcode >= 0x5c && opcode <= 0x5f) || opcode == 0xc2 ||
			opcode == 0x14 || opcode == 0x15 || opcode == 0x7c ||
			opcode == 0x7d || opcode == 0xc6 || opcode == 0xd0)
			return decode_vector_alu(instruction, opcode);
		if (opcode == 0x5a || opcode == 0x5b || opcode == 0xe6)
			return decode_vector_convert(instruction, opcode);
		if (packed_integer_opcode(opcode))
			return decode_vector_integer(instruction, opcode);
		if (opcode >= 0x71 && opcode <= 0x73)
			return decode_vector_shift_immediate(instruction, opcode);
		if (opcode == 0x50 || opcode == 0xc5 || opcode == 0xd7)
			return decode_vector_to_gpr(instruction, opcode);
		if (opcode == 0xc4)
			return decode_vector_insert(instruction);
		if (opcode >= 0x80 && opcode <= 0x8f) {
			instruction.condition = static_cast<Condition>(opcode & 0xfU);
			return decode_relative(instruction,
				Operation::jump_conditional, 4);
		}
		if (opcode >= 0x40 && opcode <= 0x4f) {
			instruction.condition = static_cast<Condition>(opcode & 0xfU);
			return decode_binary(instruction, Operation::conditional_move,
				ordinary_width(prefixes_), true);
		}
		if (opcode >= 0x90 && opcode <= 0x9f) {
			std::uint8_t modrm{};
			if (!read(modrm) || ((modrm >> 3U) & 7U) != 0)
				return false;
			instruction.condition = static_cast<Condition>(opcode & 0xfU);
			instruction.operation = Operation::set_condition;
			return rm_operand(modrm, OperandWidth::byte,
				instruction.destination);
		}
		if (opcode >= 0xc8 && opcode <= 0xcf) {
			const auto width = (prefixes_.rex & 8U) != 0 ?
				OperandWidth::qword : OperandWidth::dword;
			instruction.operation = Operation::byte_swap;
			instruction.destination = register_operand(
				static_cast<std::uint8_t>(opcode & 7U),
				(prefixes_.rex & 1U) != 0, width, true);
			return true;
		}
		switch (opcode) {
		case 0x0e:
		case 0x77:
			instruction.operation = Operation::nop;
			return true;
		case 0x01:
			return decode_0f01(instruction);
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
		case 0x18: {
			std::uint8_t modrm{};
			if (!read(modrm) || (modrm >> 6U) == 3)
				return false;
			Operand ignored{};
			if (!rm_operand(modrm, OperandWidth::byte, ignored))
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
		case 0x31:
			instruction.operation = Operation::read_timestamp;
			return true;
		case 0x38: {
			std::uint8_t map_opcode{};
			return read(map_opcode) &&
				decode_vector_map2(instruction, map_opcode);
		}
		case 0x3a: {
			std::uint8_t map_opcode{};
			return read(map_opcode) &&
				decode_vector_map3(instruction, map_opcode);
		}
		case 0xa2:
			instruction.operation = Operation::cpuid;
			return true;
		case 0xa0:
		case 0xa1:
		case 0xa8:
		case 0xa9:
			return decode_segment_stack(instruction, opcode);
		case 0xa3:
			return decode_bit_register(instruction, Operation::bit_test,
				ordinary_width(prefixes_));
		case 0xa4:
		case 0xa5:
		case 0xac:
		case 0xad:
			return decode_double_shift(
				instruction, opcode, ordinary_width(prefixes_));
		case 0xab:
			return decode_bit_register(instruction, Operation::bit_set,
				ordinary_width(prefixes_));
		case 0xae:
			return decode_0fae(instruction);
		case 0xaf:
			return decode_binary(instruction, Operation::multiply_signed,
				ordinary_width(prefixes_), true);
		case 0xb0:
			return decode_binary(instruction, Operation::compare_exchange,
				OperandWidth::byte, false);
		case 0xb1:
			return decode_binary(instruction, Operation::compare_exchange,
				ordinary_width(prefixes_), false);
		case 0xb3:
			return decode_bit_register(instruction, Operation::bit_reset,
				ordinary_width(prefixes_));
		case 0xb8:
			if (prefixes_.repeat != 0xf3)
				return false;
			return decode_binary(instruction, Operation::population_count,
				ordinary_width(prefixes_), true);
		case 0xb6:
		case 0xb7:
		case 0xbe:
		case 0xbf: {
			std::uint8_t modrm{};
			if (!read(modrm))
				return false;
			const auto source_width = (opcode == 0xb6 || opcode == 0xbe) ?
				OperandWidth::byte : OperandWidth::word;
			Operand source{};
			if (!rm_operand(modrm, source_width, source))
				return false;
			instruction.operation = (opcode == 0xb6 || opcode == 0xb7) ?
				Operation::move_zero_extend : Operation::move_sign_extend;
			instruction.destination = reg_field(
				modrm, ordinary_width(prefixes_));
			instruction.source = source;
			return true;
		}
		case 0xba:
			return decode_bit_immediate(
				instruction, ordinary_width(prefixes_));
		case 0xbb:
			return decode_bit_register(instruction, Operation::bit_complement,
				ordinary_width(prefixes_));
		case 0xbc:
			return decode_binary(instruction,
				prefixes_.repeat == 0xf3 ? Operation::count_trailing_zeros :
					Operation::bit_scan_forward,
				ordinary_width(prefixes_), true);
		case 0xbd:
			return decode_binary(instruction,
				prefixes_.repeat == 0xf3 ? Operation::count_leading_zeros :
					Operation::bit_scan_reverse,
				ordinary_width(prefixes_), true);
		case 0xc0:
			return decode_binary(instruction, Operation::exchange_add,
				OperandWidth::byte, false);
		case 0xc1:
			return decode_binary(instruction, Operation::exchange_add,
				ordinary_width(prefixes_), false);
		case 0xc3: {
			const auto move_width = (prefixes_.rex & 8U) != 0 ?
				OperandWidth::qword : OperandWidth::dword;
			std::uint8_t modrm{};
			if (!read(modrm) || (modrm >> 6U) == 3 ||
				!rm_operand(modrm, move_width, instruction.destination))
				return false;
			instruction.operation = Operation::move;
			instruction.source = reg_field(modrm, move_width);
			return true;
		}
		case 0xc7:
			return decode_c7_group(instruction);
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
		if (opcode >= 0x90 && opcode <= 0x97) {
			const auto register_index = static_cast<std::uint8_t>(opcode & 7U);
			if (register_index == 0 && (prefixes_.rex & 1U) == 0) {
				instruction.operation = Operation::nop;
				return true;
			}
			instruction.operation = Operation::exchange;
			instruction.destination = register_operand(
				0, false, width, prefixes_.rex_present);
			instruction.source = register_operand(register_index,
				(prefixes_.rex & 1U) != 0, width, prefixes_.rex_present);
			return true;
		}
		if (opcode >= 0xb0 && opcode <= 0xb7) {
			std::uint64_t immediate{};
			if (!read_unsigned(1, immediate))
				return false;
			instruction.operation = Operation::move;
			instruction.destination = register_operand(
				static_cast<std::uint8_t>(opcode & 7U),
				(prefixes_.rex & 1U) != 0, OperandWidth::byte,
				prefixes_.rex_present);
			instruction.source = immediate_operand(
				immediate, OperandWidth::byte);
			return true;
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
		case 0x00: return decode_binary(instruction, Operation::add,
			OperandWidth::byte, false);
		case 0x01: return decode_binary(instruction, Operation::add, width, false);
		case 0x02: return decode_binary(instruction, Operation::add,
			OperandWidth::byte, true);
		case 0x03: return decode_binary(instruction, Operation::add, width, true);
		case 0x04: return decode_accumulator_immediate(instruction,
			Operation::add, OperandWidth::byte);
		case 0x05: return decode_accumulator_immediate(instruction, Operation::add, width);
		case 0x08: return decode_binary(instruction, Operation::bit_or,
			OperandWidth::byte, false);
		case 0x09: return decode_binary(instruction, Operation::bit_or, width, false);
		case 0x0a: return decode_binary(instruction, Operation::bit_or,
			OperandWidth::byte, true);
		case 0x0b: return decode_binary(instruction, Operation::bit_or, width, true);
		case 0x0c: return decode_accumulator_immediate(instruction,
			Operation::bit_or, OperandWidth::byte);
		case 0x0d: return decode_accumulator_immediate(instruction, Operation::bit_or, width);
		case 0x10: return decode_binary(instruction, Operation::add_carry,
			OperandWidth::byte, false);
		case 0x11: return decode_binary(instruction, Operation::add_carry, width, false);
		case 0x12: return decode_binary(instruction, Operation::add_carry,
			OperandWidth::byte, true);
		case 0x13: return decode_binary(instruction, Operation::add_carry, width, true);
		case 0x14: return decode_accumulator_immediate(instruction,
			Operation::add_carry, OperandWidth::byte);
		case 0x15: return decode_accumulator_immediate(instruction,
			Operation::add_carry, width);
		case 0x18: return decode_binary(instruction, Operation::subtract_borrow,
			OperandWidth::byte, false);
		case 0x19: return decode_binary(instruction, Operation::subtract_borrow,
			width, false);
		case 0x1a: return decode_binary(instruction, Operation::subtract_borrow,
			OperandWidth::byte, true);
		case 0x1b: return decode_binary(instruction, Operation::subtract_borrow,
			width, true);
		case 0x1c: return decode_accumulator_immediate(instruction,
			Operation::subtract_borrow, OperandWidth::byte);
		case 0x1d: return decode_accumulator_immediate(instruction,
			Operation::subtract_borrow, width);
		case 0x20: return decode_binary(instruction, Operation::bit_and,
			OperandWidth::byte, false);
		case 0x21: return decode_binary(instruction, Operation::bit_and, width, false);
		case 0x22: return decode_binary(instruction, Operation::bit_and,
			OperandWidth::byte, true);
		case 0x23: return decode_binary(instruction, Operation::bit_and, width, true);
		case 0x24: return decode_accumulator_immediate(instruction,
			Operation::bit_and, OperandWidth::byte);
		case 0x25: return decode_accumulator_immediate(instruction, Operation::bit_and, width);
		case 0x28: return decode_binary(instruction, Operation::subtract,
			OperandWidth::byte, false);
		case 0x29: return decode_binary(instruction, Operation::subtract, width, false);
		case 0x2a: return decode_binary(instruction, Operation::subtract,
			OperandWidth::byte, true);
		case 0x2b: return decode_binary(instruction, Operation::subtract, width, true);
		case 0x2c: return decode_accumulator_immediate(instruction,
			Operation::subtract, OperandWidth::byte);
		case 0x2d: return decode_accumulator_immediate(instruction, Operation::subtract, width);
		case 0x30: return decode_binary(instruction, Operation::bit_xor,
			OperandWidth::byte, false);
		case 0x31: return decode_binary(instruction, Operation::bit_xor, width, false);
		case 0x32: return decode_binary(instruction, Operation::bit_xor,
			OperandWidth::byte, true);
		case 0x33: return decode_binary(instruction, Operation::bit_xor, width, true);
		case 0x34: return decode_accumulator_immediate(instruction,
			Operation::bit_xor, OperandWidth::byte);
		case 0x35: return decode_accumulator_immediate(instruction, Operation::bit_xor, width);
		case 0x38: return decode_binary(instruction, Operation::compare,
			OperandWidth::byte, false);
		case 0x39: return decode_binary(instruction, Operation::compare, width, false);
		case 0x3a: return decode_binary(instruction, Operation::compare,
			OperandWidth::byte, true);
		case 0x3b: return decode_binary(instruction, Operation::compare, width, true);
		case 0x3c: return decode_accumulator_immediate(instruction,
			Operation::compare, OperandWidth::byte);
		case 0x3d: return decode_accumulator_immediate(instruction, Operation::compare, width);
		case 0x63: {
			std::uint8_t modrm{};
			if (!read(modrm))
				return false;
			Operand source{};
			if (!rm_operand(modrm, OperandWidth::dword, source))
				return false;
			instruction.operation = Operation::move_sign_extend;
			instruction.destination = reg_field(modrm, width);
			instruction.source = source;
			return true;
		}
		case 0x68:
		case 0x6a:
			return decode_push_immediate(instruction, opcode);
		case 0x69:
		case 0x6b:
			return decode_imul_immediate(instruction, opcode, width);
		case 0x80: return decode_immediate_group(instruction,
			OperandWidth::byte, 1, false);
		case 0x81: return decode_immediate_group(instruction, width,
			width == OperandWidth::word ? 2U : 4U,
			width == OperandWidth::qword);
		case 0x83: return decode_immediate_group(instruction, width, 1, true);
		case 0x84: return decode_binary(instruction, Operation::test,
			OperandWidth::byte, false);
		case 0x85: return decode_binary(instruction, Operation::test, width, false);
		case 0x86: return decode_binary(instruction, Operation::exchange,
			OperandWidth::byte, false);
		case 0x87: return decode_binary(instruction, Operation::exchange,
			width, false);
		case 0x88: return decode_binary(instruction, Operation::move,
			OperandWidth::byte, false);
		case 0x89: return decode_binary(instruction, Operation::move, width, false);
		case 0x8a: return decode_binary(instruction, Operation::move,
			OperandWidth::byte, true);
		case 0x8b: return decode_binary(instruction, Operation::move, width, true);
		case 0x8c:
		case 0x8e:
			return decode_segment_move(instruction, opcode);
		case 0x8d: {
			std::uint8_t modrm{};
			if (!read(modrm) || (modrm >> 6U) == 3)
				return false;
			instruction.operation = Operation::lea;
			instruction.destination = reg_field(modrm, width);
			return rm_operand(modrm, width, instruction.source);
		}
		case 0x8f: {
			std::uint8_t modrm{};
			if (!read(modrm) || ((modrm >> 3U) & 7U) != 0)
				return false;
			instruction.operation = Operation::pop;
			return rm_operand(modrm, stack_width(prefixes_),
				instruction.destination);
		}
		case 0x98:
			instruction.operation = Operation::sign_extend_accumulator;
			instruction.destination = register_operand(
				0, false, width, prefixes_.rex_present);
			return true;
		case 0x99:
			instruction.operation = Operation::sign_extend_data;
			instruction.destination = register_operand(
				2, false, width, prefixes_.rex_present);
			return true;
		case 0x9c:
			instruction.operation = Operation::push_flags;
			instruction.source.width = stack_width(prefixes_);
			return true;
		case 0x9d:
			instruction.operation = Operation::pop_flags;
			instruction.destination.width = stack_width(prefixes_);
			return true;
		case 0x9e:
			instruction.operation = Operation::sahf;
			return true;
		case 0x9f:
			instruction.operation = Operation::lahf;
			return true;
		case 0xa0:
		case 0xa1:
		case 0xa2:
		case 0xa3:
			return decode_moffs(instruction, opcode, width);
		case 0xa4:
		case 0xa5:
		case 0xa6:
		case 0xa7:
		case 0xaa:
		case 0xab:
		case 0xac:
		case 0xad:
		case 0xae:
		case 0xaf:
			return decode_string(instruction, opcode, width);
		case 0xa8:
			return decode_accumulator_immediate(instruction,
				Operation::test, OperandWidth::byte);
		case 0xa9:
			return decode_accumulator_immediate(instruction, Operation::test, width);
		case 0xc0:
		case 0xc1:
		case 0xd0:
		case 0xd1:
		case 0xd2:
		case 0xd3:
			return decode_shift_group(instruction, opcode,
				(opcode == 0xc0 || opcode == 0xd0 || opcode == 0xd2) ?
				OperandWidth::byte : width);
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
		case 0xc4:
		case 0xc5:
			return decode_vex(opcode, instruction);
		case 0xc8: {
			std::uint64_t allocation{};
			std::uint64_t nesting{};
			if (!read_unsigned(2, allocation) ||
				!read_unsigned(1, nesting))
				return false;
			instruction.operation = Operation::enter;
			instruction.destination.width = stack_width(prefixes_);
			instruction.source = immediate_operand(
				allocation, OperandWidth::word);
			instruction.auxiliary = immediate_operand(
				nesting & 0x1fU, OperandWidth::byte);
			return true;
		}
		case 0xc9:
			instruction.operation = Operation::leave;
			instruction.destination.width = stack_width(prefixes_);
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
		case 0xe0:
		case 0xe1:
		case 0xe2:
		case 0xe3:
			if (!decode_relative(instruction, Operation::loop, 1))
				return false;
			instruction.vector_opcode = opcode;
			instruction.vector_control = prefixes_.address_size ? 1U : 0U;
			return true;
		case 0xf5:
			instruction.operation = Operation::carry_complement;
			return true;
		case 0xf6:
		case 0xf7:
			return decode_unary_group(instruction, opcode, width);
		case 0xf8:
			instruction.operation = Operation::carry_clear;
			return true;
		case 0xf9:
			instruction.operation = Operation::carry_set;
			return true;
		case 0xfc:
			instruction.operation = Operation::direction_clear;
			return true;
		case 0xfd:
			instruction.operation = Operation::direction_set;
			return true;
		case 0xfe: {
			std::uint8_t modrm{};
			if (!read(modrm))
				return false;
			const auto group = static_cast<std::uint8_t>(
				(modrm >> 3U) & 7U);
			if (group > 1 || !rm_operand(modrm, OperandWidth::byte,
				instruction.destination))
				return false;
			instruction.operation = group == 0 ?
				Operation::increment : Operation::decrement;
			return true;
		}
		case 0xff:
			return decode_ff_group(instruction, width);
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
	case Operation::jump_indirect:
	case Operation::jump_conditional:
	case Operation::loop:
	case Operation::call:
	case Operation::call_indirect:
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
