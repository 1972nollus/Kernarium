// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "memory/guest_address_space.hpp"

#include <cstdint>

namespace uml::cpu::x86_64 {

enum class OperandWidth : std::uint8_t {
	byte = 1,
	word = 2,
	dword = 4,
	qword = 8,
};

enum class Segment : std::uint8_t {
	none,
	fs,
	gs,
};

enum class OperandKind : std::uint8_t {
	none,
	reg,
	memory,
	immediate,
};

struct RegisterOperand final {
	std::uint8_t index{};
	bool high_byte{};
};

struct MemoryOperand final {
	static constexpr std::uint8_t no_register = 0xff;

	std::uint8_t base{no_register};
	std::uint8_t index{no_register};
	std::uint8_t scale{1};
	std::int32_t displacement{};
	Segment segment{Segment::none};
	bool rip_relative{};
};

struct Operand final {
	OperandKind kind{OperandKind::none};
	OperandWidth width{OperandWidth::qword};
	RegisterOperand reg{};
	MemoryOperand memory{};
	std::uint64_t immediate{};
};

enum class Operation : std::uint8_t {
	nop,
	move,
	lea,
	add,
	subtract,
	bit_and,
	bit_or,
	bit_xor,
	compare,
	test,
	push,
	pop,
	jump,
	jump_conditional,
	call,
	ret,
	system_call,
	breakpoint,
};

enum class Condition : std::uint8_t {
	overflow,
	not_overflow,
	below,
	above_or_equal,
	equal,
	not_equal,
	below_or_equal,
	above,
	sign,
	not_sign,
	parity,
	not_parity,
	less,
	greater_or_equal,
	less_or_equal,
	greater,
};

struct Instruction final {
	Operation operation{Operation::nop};
	Operand destination{};
	Operand source{};
	Condition condition{Condition::equal};
	std::uint8_t length{};
	bool locked{};

	[[nodiscard]] bool terminal() const noexcept;
};

enum class DecodeStatus : std::uint8_t {
	decoded,
	fetch_fault,
	illegal_instruction,
};

struct DecodeResult final {
	DecodeStatus status{DecodeStatus::illegal_instruction};
	Instruction instruction{};
	std::uint64_t fault_address{};
};

[[nodiscard]] DecodeResult decode(
	::uml::memory::GuestAddressSpace &memory, std::uint64_t rip) noexcept;

} // namespace uml::cpu::x86_64
