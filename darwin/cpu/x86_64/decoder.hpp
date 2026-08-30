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
	xmm = 16,
	ymm = 32,
};

enum class Segment : std::uint8_t {
	none,
	fs,
	gs,
};

enum class OperandKind : std::uint8_t {
	none,
	reg,
	vector_reg,
	mmx_reg,
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
	std::uint64_t absolute{};
	Segment segment{Segment::none};
	bool rip_relative{};
	bool address_32{};
	bool absolute_address{};
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
	move_zero_extend,
	move_sign_extend,
	lea,
	exchange,
	add,
	add_carry,
	subtract,
	subtract_borrow,
	increment,
	decrement,
	negate,
	bit_not,
	bit_and,
	bit_or,
	bit_xor,
	compare,
	test,
	shift_left,
	shift_right,
	shift_arithmetic_right,
	rotate_left,
	rotate_right,
	rotate_carry_left,
	rotate_carry_right,
	double_shift_left,
	double_shift_right,
	multiply,
	multiply_signed,
	divide,
	divide_signed,
	bit_test,
	bit_set,
	bit_reset,
	bit_complement,
	bit_scan_forward,
	bit_scan_reverse,
	count_trailing_zeros,
	count_leading_zeros,
	population_count,
	compare_exchange,
	compare_exchange_pair,
	exchange_add,
	set_condition,
	conditional_move,
	byte_swap,
	sign_extend_accumulator,
	sign_extend_data,
	push,
	pop,
	push_flags,
	pop_flags,
	leave,
	enter,
	string_move,
	string_compare,
	string_store,
	string_load,
	string_scan,
	carry_clear,
	carry_set,
	carry_complement,
	direction_clear,
	direction_set,
	lahf,
	sahf,
	jump,
	jump_indirect,
	jump_conditional,
	call,
	call_indirect,
	ret,
	cpuid,
	read_extended_control,
	read_timestamp,
	read_processor_id,
	random,
	read_fs_base,
	read_gs_base,
	write_fs_base,
	write_gs_base,
	store_gdtr,
	store_machine_status,
	cache_zero,
	memory_fence,
	cache_flush,
	vector_move,
	vector_move_scalar,
	vector_zero,
	vector_alu,
	vector_lane_move,
	vector_convert,
	vector_gpr_move,
	vector_integer,
	vector_to_gpr,
	vector_insert,
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
	Operand auxiliary{};
	Condition condition{Condition::equal};
	std::uint8_t length{};
	std::uint8_t repeat{};
	std::uint8_t vector_opcode{};
	std::uint8_t vector_map{};
	std::uint8_t vector_prefix{};
	std::uint8_t vector_control{};
	std::uint8_t vector_group{};
	bool locked{};
	bool vex{};

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
