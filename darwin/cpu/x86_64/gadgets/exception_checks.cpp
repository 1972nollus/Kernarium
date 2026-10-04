// SPDX-License-Identifier: GPL-2.0-or-later
#include "exception_checks.hpp"

#include <SoftFloat-3e/platform.h>

extern "C" {
#include <SoftFloat-3e/softfloat.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>

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

namespace {

/* An IEEE binary32 or binary64 lane's classes. */
struct FloatFormat final {
	bool dbl{};

	[[nodiscard]] std::uint64_t sign() const noexcept
	{
		return dbl ? 1ULL << 63U : 1ULL << 31U;
	}
	[[nodiscard]] std::uint64_t magnitude() const noexcept
	{
		return sign() - 1U;
	}
	[[nodiscard]] std::uint64_t exponent() const noexcept
	{
		return dbl ? 0x7ffULL << 52U : 0xffULL << 23U;
	}
	[[nodiscard]] bool nan(std::uint64_t x) const noexcept
	{
		return (x & magnitude()) > exponent();
	}
	[[nodiscard]] bool infinite(std::uint64_t x) const noexcept
	{
		return (x & magnitude()) == exponent();
	}
	[[nodiscard]] bool zero(std::uint64_t x) const noexcept
	{
		return (x & magnitude()) == 0;
	}
	[[nodiscard]] bool denormal(std::uint64_t x) const noexcept
	{
		return (x & exponent()) == 0 && !zero(x);
	}
	[[nodiscard]] bool negative(std::uint64_t x) const noexcept
	{
		return (x & sign()) != 0;
	}
	[[nodiscard]] bool signalling(std::uint64_t x) const noexcept
	{
		return nan(x) && (x & (dbl ? 1ULL << 51U : 1ULL << 22U)) == 0;
	}
	/* `x` as MXCSR.DAZ reads it: a denormal is a zero of its sign. */
	[[nodiscard]] std::uint64_t input(std::uint64_t x,
		bool denormals_are_zero) const noexcept
	{
		return denormals_are_zero && denormal(x) ? x & sign() : x;
	}
	/* A lane of a two-source operation that raises a denormal operand,
	 * which a NaN in it overrides. */
	[[nodiscard]] bool denormal_pair(std::uint64_t a,
		std::uint64_t b) const noexcept
	{
		return (denormal(a) || denormal(b)) && !nan(a) && !nan(b);
	}
};

using VectorValue = std::array<std::byte, 32>;

[[nodiscard]] std::uint64_t value_lane(const VectorValue &value, bool dbl,
	std::size_t index) noexcept
{
	if (dbl) {
		std::uint64_t lane{};
		std::memcpy(&lane, value.data() + index * 8U, 8U);
		return lane;
	}
	std::uint32_t lane{};
	std::memcpy(&lane, value.data() + index * 4U, 4U);
	return lane;
}

/* An operand's register, or the bytes its memory operand reads with zeros
 * after them; false when that memory is unreadable. */
[[nodiscard]] bool operand_value(const CpuState &cpu, const Gadget &gadget,
	const Operand &operand, ::uml::memory::GuestAddressSpace &memory,
	VectorValue &value)
{
	value = {};
	if (operand.kind == OperandKind::vector_reg) {
		value = cpu.vectors[operand.reg.index & 15U];
		return true;
	}
	if (operand.kind != OperandKind::memory)
		return false;
	const auto bytes = std::min(static_cast<std::size_t>(operand.width),
		value.size());
	return memory.read(effective_address(cpu, gadget, operand),
		std::span<std::byte>{value}.first(bytes));
}

[[nodiscard]] std::size_t vector_lanes(const Operand &operand,
	bool dbl) noexcept
{
	return static_cast<std::size_t>(operand.width) / (dbl ? 8U : 4U);
}

/* An FMA's x86 a, b and c of a * b + c, and the signs its lanes give the
 * product and the addend. */
struct FmaOperands final {
	VectorValue a{};
	VectorValue b{};
	VectorValue c{};
	bool dbl{};
	std::size_t lanes{};
	unsigned operation{};

	[[nodiscard]] bool negate_product() const noexcept
	{
		return operation >= 0xcU;
	}
	/* FMADDSUB subtracts in its even lanes and FMSUBADD in its odd ones. */
	[[nodiscard]] bool negate_addend(std::size_t index) const noexcept
	{
		if (operation == 6U)
			return (index & 1U) == 0;
		if (operation == 7U)
			return (index & 1U) != 0;
		return operation == 0xaU || operation == 0xbU ||
			operation == 0xeU || operation == 0xfU;
	}
};

/* False when a memory source is unreadable. */
[[nodiscard]] bool read_fma_operands(const CpuState &cpu,
	const Gadget &gadget, ::uml::memory::GuestAddressSpace &memory,
	FmaOperands &operands)
{
	const auto &instruction = gadget.instruction;
	VectorValue destination{};
	VectorValue vvvv{};
	VectorValue rm{};
	if (!operand_value(cpu, gadget, instruction.destination, memory,
			destination) ||
		!operand_value(cpu, gadget, instruction.source, memory, vvvv) ||
		!operand_value(cpu, gadget, instruction.auxiliary, memory, rm))
		return false;
	operands.dbl = instruction.vector_group != 0;
	operands.operation = instruction.vector_opcode & 0x0fU;
	const bool scalar = operands.operation >= 9U &&
		(operands.operation & 1U) != 0;
	operands.lanes = scalar ? 1U :
		vector_lanes(instruction.destination, operands.dbl);
	/* 132 is destination * r/m + vvvv, 213 vvvv * destination + r/m, and
	 * 231 vvvv * r/m + destination. */
	const unsigned form = instruction.vector_opcode >> 4U;
	operands.a = form == 0x9U ? destination : vvvv;
	operands.b = form == 0xaU ? destination : rm;
	operands.c = form == 0x9U ? vvvv : form == 0xaU ? rm : destination;
	return true;
}

/* Whether an FMA lane raises invalid operation on x86: for a signalling
 * NaN, for infinity times zero plus anything but a quiet NaN, and for
 * infinities of opposite signs added. The host raises it for infinity times
 * zero plus a quiet NaN too. */
[[nodiscard]] bool fma_lane_invalid(const FloatFormat &f, std::uint64_t a,
	std::uint64_t b, std::uint64_t c, bool product_negative,
	bool addend_negative) noexcept
{
	if (f.signalling(a) || f.signalling(b) || f.signalling(c))
		return true;
	if ((f.zero(a) && f.infinite(b)) || (f.infinite(a) && f.zero(b)))
		return !f.nan(c);
	if (f.nan(a) || f.nan(b) || f.nan(c))
		return false;
	return (f.infinite(a) || f.infinite(b)) && f.infinite(c) &&
		product_negative != addend_negative;
}

/* x86 detects underflow after rounding: a result is tiny when, rounded to
 * full precision as if the exponent were unbounded, it lies below the
 * smallest normal, where the host detects it before rounding. The two
 * differ for a result the host rounds up to exactly the smallest normal.
 * SoftFloat rounds as x86 does, from a binary128 value that holds the exact
 * result rounded to odd: truncated, with its lowest bit set when that lost
 * anything. A single or double keeps at most 53 of the 113 bits, so
 * rounding it again rounds as the exact result would. A product of two
 * lanes is exact in binary128. */

[[nodiscard]] std::uint8_t soft_rounding(unsigned control) noexcept
{
	switch (control & 3U) {
	case 1: return softfloat_round_min;
	case 2: return softfloat_round_max;
	case 3: return softfloat_round_minMag;
	default: return softfloat_round_near_even;
	}
}

/* A lane's value, exactly. */
[[nodiscard]] float128_t soft_wide(bool dbl, std::uint64_t x) noexcept
{
	softfloat_state state{};
	if (!dbl)
		return f32_to_f128(&state,
			float32_t{static_cast<std::uint32_t>(x)});
	return extF80_to_f128(&state, f64_to_extF80(&state, float64_t{x}));
}

/* `operation` rounded to odd, with SoftFloat's flags for it in `flags`. */
template <typename Operation>
[[nodiscard]] float128_t soft_odd(Operation operation,
	std::uint8_t *flags = nullptr) noexcept
{
	softfloat_state state{};
	state.roundingMode = softfloat_round_minMag;
	auto result = operation(state);
	if ((state.exceptionFlags & softfloat_flag_inexact) != 0)
		result.v[0] |= 1U;
	if (flags != nullptr)
		*flags = state.exceptionFlags;
	return result;
}

/* A result rounded as x86 does: whether it is tiny after rounding, whether
 * it is inexact and whether it overflowed, and the result; and whether it
 * is inexact rounded to full precision as if the exponent were unbounded,
 * which is what an unmasked underflow or overflow reports as precision. */
struct SoftRounding final {
	bool tiny{};
	bool inexact{};
	bool overflow{};
	bool significand_inexact{};
	std::uint64_t bits{};
};

/* `value` rounded to a single or a double in rounding mode `control`. Its
 * significand rounds exactly when the binary128 fraction bits below the
 * format's are clear; every value here is normal in binary128. */
[[nodiscard]] SoftRounding soft_round(float128_t value, bool dbl,
	unsigned control) noexcept
{
	softfloat_state state{};
	state.detectTininess = softfloat_tininess_afterRounding;
	state.roundingMode = soft_rounding(control);
	SoftRounding result{};
	result.bits = dbl ? f128_to_f64(&state, value).v :
		f128_to_f32(&state, value).v;
	/* SoftFloat raises underflow for a tiny inexact result; an exact one
	 * is a denormal. */
	result.tiny = (state.exceptionFlags & softfloat_flag_underflow) != 0 ||
		FloatFormat{dbl}.denormal(result.bits);
	result.inexact = (state.exceptionFlags & softfloat_flag_inexact) != 0;
	result.overflow = (state.exceptionFlags & softfloat_flag_overflow) != 0;
	/* Of 112 fraction bits, a double keeps 52 and drops 60, and a single
	 * keeps 23 and drops 89. */
	result.significand_inexact = dbl ?
		(value.v[0] & ((1ULL << 60U) - 1U)) != 0 :
		value.v[0] != 0 || (value.v[1] & ((1ULL << 25U) - 1U)) != 0;
	return result;
}

/* The single `x` rounded to a half, which SoftFloat here lacks. Just below
 * the smallest normal half, 2^-14, eleven bits step by 2^-25: rounding to
 * nearest reaches it from within 2^-26 and rounding away from zero from
 * within 2^-25. The halves delivered there step by 2^-24. Just below 2^16,
 * they step by 2^5, and the largest is 65504: rounding to nearest
 * overflows from 65520, rounding away from zero above 65504, and rounding
 * toward zero from 2^16. A double holds the single exactly, and these
 * bounds and scalings too. */
[[nodiscard]] SoftRounding half_round(std::uint32_t x,
	unsigned control) noexcept
{
	float single{};
	std::memcpy(&single, &x, sizeof single);
	const double magnitude = std::fabs(static_cast<double>(single));
	if (!(magnitude < 0x1p128) || magnitude == 0)
		return {};
	const bool negative = (x >> 31U) != 0;
	/* Rounding to nearest, away from zero, or toward zero. */
	const unsigned direction = (control & 3U) == 0 ? 0U :
		(control & 3U) == 3U ? 2U :
		((control & 3U) == 1U) == negative ? 1U : 2U;
	int exponent{};
	const double fraction = std::frexp(magnitude, &exponent);
	SoftRounding result{};
	/* x86 reports precision for an unmasked tiny half of a denormal single
	 * too, as an Intel Xeon 8581C does. */
	result.significand_inexact =
		fraction * 0x1p11 != std::floor(fraction * 0x1p11) ||
		FloatFormat{false}.denormal(x);
	if (magnitude < 0x1p-14) {
		const double bound = direction == 0 ? 0x1p-14 - 0x1p-26 :
			0x1p-14 - 0x1p-25;
		result.tiny = direction == 2 || (direction == 0 ?
			magnitude < bound : !(magnitude > bound));
		const double steps = magnitude * 0x1p24;
		result.inexact = steps != std::floor(steps);
		return result;
	}
	result.overflow = direction == 0 ? magnitude >= 65520.0 :
		direction == 1 ? magnitude > 65504.0 : magnitude >= 0x1p16;
	result.inexact = result.overflow || result.significand_inexact;
	return result;
}

/* Whether the finite lane `x` is an integer. */
[[nodiscard]] bool integral(const FloatFormat &f, std::uint64_t x) noexcept
{
	if (f.zero(x))
		return true;
	const unsigned fraction = f.dbl ? 52U : 23U;
	const unsigned bias = f.dbl ? 1023U : 127U;
	const auto exponent =
		static_cast<unsigned>((x & f.exponent()) >> fraction);
	if (exponent >= bias + fraction)
		return true;
	if (exponent < bias)
		return false;
	return (x & ((1ULL << (bias + fraction - exponent)) - 1U)) == 0;
}

/* x86's underflow and precision flags for the results an instruction
 * rounds. A tiny result raises underflow when it is inexact, or regardless
 * while MXCSR unmasks underflow. While it masks underflow, FTZ flushes a
 * tiny result to zero, which raises underflow and precision even when it
 * was exact. Beside an unmasked underflow or overflow, which faults, a
 * result raises precision as its significand rounds, and the others as
 * they do: the model then decides precision too. */
struct UnderflowModel final {
	static constexpr std::uint32_t underflow = 0x10U;
	static constexpr std::uint32_t precision = 0x20U;

	bool unmasked{};
	bool overflow_unmasked{};
	bool flush{};
	bool faults{};
	std::uint32_t raised{};
	std::uint32_t precise{};

	UnderflowModel(std::uint32_t mxcsr, bool flushes) noexcept :
		unmasked{(mxcsr & (underflow << 7U)) == 0},
		overflow_unmasked{(mxcsr & (0x08U << 7U)) == 0},
		flush{flushes && !unmasked &&
			(mxcsr & mxcsr_flush_to_zero) != 0}
	{
	}

	/* Whether FTZ flushes the result. */
	bool record(const SoftRounding &result) noexcept
	{
		bool inexact = result.inexact;
		bool flushed{};
		if (result.tiny && flush) {
			raised |= underflow | precision;
			inexact = flushed = true;
		} else if (result.tiny && unmasked) {
			raised |= underflow;
			inexact = result.significand_inexact;
			faults = true;
		} else if (result.tiny && result.inexact) {
			raised |= underflow;
		} else if (result.overflow && overflow_unmasked) {
			inexact = result.significand_inexact;
			faults = true;
		}
		if (inexact)
			precise |= precision;
		return flushed;
	}

	/* Precision as x86 raises it, which differs from the host's only beside
	 * an unmasked underflow or overflow. */
	[[nodiscard]] std::uint32_t precision_raised() const noexcept
	{
		return precise;
	}

	[[nodiscard]] HostFlagCorrection correction(
		std::uint32_t decided = 0, std::uint32_t also = 0) const noexcept
	{
		return {decided | underflow | (faults ? precision : 0U),
			raised | also | (faults ? precise : 0U)};
	}
};

/* DPPS or DPPD as x86 computes it, in steps: the selected products, their
 * sums in pairs, and for DPPS the sum of those, each step across both
 * halves of a 256-bit DPPS. x86 raises each step's flags in turn and faults
 * at the first that raises an unmasked exception -- before computing it,
 * for invalid operation or a denormal operand -- so that later steps raise
 * nothing. FTZ flushes each tiny result, and DAZ makes a denormal one zero
 * as an input of the next step. An unselected product is zero and raises
 * nothing. The flags are settled: precedence included, and no host flag. */
[[nodiscard]] HostFlagCorrection dot_product_flags(std::uint32_t mxcsr,
	bool dbl, unsigned control, std::size_t halves, const VectorValue &first,
	const VectorValue &second)
{
	constexpr std::uint32_t invalid = 0x01U;
	constexpr std::uint32_t denormal = 0x02U;
	constexpr std::uint32_t overflow = 0x08U;
	using Lanes = std::array<std::uint64_t, 8>;
	const FloatFormat f{dbl};
	const bool denormals_are_zero = (mxcsr & mxcsr_denormals_are_zero) != 0;
	const std::uint32_t unmasked = ~(mxcsr >> 7U) & 0x3fU;
	std::uint32_t total{};
	bool faulted{};
	/* One step's `count` operations of `left` and `right`, lane by lane. */
	const auto step = [&](bool multiply, std::size_t count, const Lanes &left,
		const Lanes &right) {
		Lanes results{};
		if (faulted)
			return results;
		std::array<float128_t, 8> values{};
		std::uint32_t before{};
		for (std::size_t index = 0; index < count; ++index) {
			const auto a = left[index];
			const auto b = right[index];
			std::uint8_t flags{};
			values[index] = soft_odd([&](auto &state) {
				const auto x = soft_wide(dbl, a);
				const auto y = soft_wide(dbl, b);
				return multiply ? f128_mul(&state, x, y) :
					f128_add(&state, x, y);
			}, &flags);
			if (f.signalling(a) || f.signalling(b) ||
				(flags & softfloat_flag_invalid) != 0)
				before |= invalid;
			else if (f.denormal_pair(a, b))
				before |= denormal;
		}
		total |= before;
		if ((before & unmasked) != 0) {
			faulted = true;
			return results;
		}
		UnderflowModel model{mxcsr, true};
		std::uint32_t after{};
		for (std::size_t index = 0; index < count; ++index) {
			const auto result = soft_round(values[index], dbl, mxcsr >> 13U);
			if (result.overflow)
				after |= overflow;
			results[index] = f.input(model.record(result) ?
				result.bits & f.sign() : result.bits, denormals_are_zero);
		}
		after |= model.raised | model.precision_raised();
		total |= after;
		faulted = (after & unmasked) != 0;
		return results;
	};
	const std::size_t per_half = dbl ? 2U : 4U;
	Lanes left{};
	Lanes right{};
	for (std::size_t lane = 0; lane < halves * per_half; ++lane) {
		if (((control >> (4U + lane % per_half)) & 1U) == 0)
			continue;
		left[lane] = f.input(value_lane(first, dbl, lane), denormals_are_zero);
		right[lane] = f.input(value_lane(second, dbl, lane),
			denormals_are_zero);
	}
	auto sums = step(true, halves * per_half, left, right);
	for (std::size_t steps = per_half; steps > 1; steps /= 2) {
		const std::size_t count = halves * steps / 2U;
		for (std::size_t index = 0; index < count; ++index) {
			left[index] = sums[2U * index];
			right[index] = sums[2U * index + 1U];
		}
		sums = step(false, count, left, right);
	}
	return {0x3fU, total, true};
}

/* Precision for a conversion of `gadget`'s auxiliary lanes, singles or
 * doubles, to 32- or 64-bit integers, truncated or rounded as MXCSR does:
 * x86 raises it for a lane that is not an integer and converts in range,
 * and only invalid operation for one out of range, which the host raises.
 * Its conversions do not raise it. */
[[nodiscard]] HostFlagCorrection integer_conversion_precision(
	const CpuState &cpu, const Gadget &gadget,
	::uml::memory::GuestAddressSpace &memory, bool dbl, bool truncate,
	bool wide)
{
	const auto &instruction = gadget.instruction;
	const FloatFormat f{dbl};
	VectorValue source{};
	if (!operand_value(cpu, gadget, instruction.auxiliary, memory, source))
		return {};
	const auto lanes = vector_lanes(instruction.auxiliary, dbl);
	const bool denormals_are_zero =
		(cpu.mxcsr & mxcsr_denormals_are_zero) != 0;
	const auto mode = truncate ? std::uint8_t{softfloat_round_minMag} :
		soft_rounding(cpu.mxcsr >> 13U);
	std::uint32_t raised{};
	for (std::size_t index = 0; index < lanes; ++index) {
		const auto x = f.input(value_lane(source, dbl, index),
			denormals_are_zero);
		if (f.nan(x) || f.infinite(x))
			continue;
		softfloat_state state{};
		const auto value = soft_wide(dbl, x);
		if (wide)
			(void)f128_to_i64(&state, value, mode, true);
		else
			(void)f128_to_i32(&state, value, mode, true);
		if ((state.exceptionFlags & softfloat_flag_inexact) != 0)
			raised = UnderflowModel::precision;
	}
	return {UnderflowModel::precision, raised};
}

} // namespace

std::uint32_t denormal_operand(const CpuState &cpu, const Gadget &gadget,
	::uml::memory::GuestAddressSpace &memory)
{
	constexpr std::uint32_t raised = 0x02U;
	const auto &instruction = gadget.instruction;
	if ((cpu.mxcsr & mxcsr_denormals_are_zero) != 0 ||
		!simd_floating(instruction))
		return 0;
	const auto opcode = instruction.vector_opcode;
	const auto prefix = instruction.vector_prefix;
	const auto read = [&](const Operand &operand, VectorValue &value) {
		return operand_value(cpu, gadget, operand, memory, value);
	};
	VectorValue first{};
	VectorValue second{};
	switch (instruction.operation) {
	case Operation::vector_alu: {
		/* HADD, HSUB and ADDSUB are PD with 66 and PS with F2; the rest
		 * PS, PD, SS and SD with none, 66, F3 and F2. */
		const bool horizontal = opcode == 0x7c || opcode == 0x7d ||
			opcode == 0xd0;
		const bool dbl = horizontal ? prefix == 1 :
			prefix == 1 || prefix == 3;
		const bool scalar = !horizontal && prefix >= 2;
		const FloatFormat f{dbl};
		const std::size_t lanes = scalar ? 1U :
			vector_lanes(instruction.destination, dbl);
		if (!read(instruction.auxiliary, second))
			return 0;
		if (opcode == 0x51) {
			/* The square root of a negative denormal is invalid. */
			for (std::size_t index = 0; index < lanes; ++index) {
				const auto x = value_lane(second, dbl, index);
				if (f.denormal(x) && !f.negative(x))
					return raised;
			}
			return 0;
		}
		if (!read(instruction.source, first))
			return 0;
		if (opcode == 0x7c || opcode == 0x7d) {
			/* Adjacent pairs of either source, in each 128-bit half. */
			for (std::size_t pair = 0; pair < lanes; pair += 2)
				if (f.denormal_pair(value_lane(first, dbl, pair),
						value_lane(first, dbl, pair + 1U)) ||
					f.denormal_pair(value_lane(second, dbl, pair),
						value_lane(second, dbl, pair + 1U)))
					return raised;
			return 0;
		}
		for (std::size_t index = 0; index < lanes; ++index) {
			const auto b = value_lane(second, dbl, index);
			/* A division by zero takes precedence. */
			if (f.denormal_pair(value_lane(first, dbl, index), b) &&
				!(opcode == 0x5e && f.zero(b)))
				return raised;
		}
		return 0;
	}
	case Operation::vector_compare_flags: {
		const bool dbl = prefix == 1;
		if (!read(instruction.source, first) ||
			!read(instruction.auxiliary, second))
			return 0;
		return FloatFormat{dbl}.denormal_pair(value_lane(first, dbl, 0),
			value_lane(second, dbl, 0)) ? raised : 0U;
	}
	case Operation::vector_convert: {
		/* CVTPS2PD and CVTSS2SD widen singles, CVTPD2PS and CVTSD2SS
		 * narrow doubles; the other conversions raise none. */
		if (opcode != 0x5a || !read(instruction.auxiliary, second))
			return 0;
		const bool dbl = prefix == 1 || prefix == 3;
		const std::size_t lanes = prefix >= 2 ? 1U :
			vector_lanes(instruction.auxiliary, dbl);
		for (std::size_t index = 0; index < lanes; ++index)
			if (FloatFormat{dbl}.denormal(value_lane(second, dbl, index)))
				return raised;
		return 0;
	}
	case Operation::vector_fma: {
		FmaOperands fma{};
		if (!read_fma_operands(cpu, gadget, memory, fma))
			return 0;
		const FloatFormat f{fma.dbl};
		for (std::size_t index = 0; index < fma.lanes; ++index) {
			const auto x = value_lane(fma.a, fma.dbl, index);
			const auto y = value_lane(fma.b, fma.dbl, index);
			const auto z = value_lane(fma.c, fma.dbl, index);
			if (f.nan(x) || f.nan(y) || f.nan(z) ||
				!(f.denormal(x) || f.denormal(y) || f.denormal(z)))
				continue;
			/* An invalid operation takes precedence: zero times
			 * infinity, or infinities of opposite signs added. */
			const bool product_negative =
				(f.negative(x) != f.negative(y)) != fma.negate_product();
			const bool addend_negative =
				f.negative(z) != fma.negate_addend(index);
			if (fma_lane_invalid(f, x, y, z, product_negative,
					addend_negative))
				continue;
			return raised;
		}
		return 0;
	}
	case Operation::vector_integer_map3:
		if (opcode == 0x1d) {
			/* VCVTPS2PH narrows singles. */
			if (!read(instruction.source, first))
				return 0;
			const auto lanes = vector_lanes(instruction.source, false);
			for (std::size_t index = 0; index < lanes; ++index)
				if (FloatFormat{false}.denormal(
						value_lane(first, false, index)))
					return raised;
			return 0;
		}
		/* DPPS and DPPD settle every flag (`host_flag_correction`). */
		return 0;
	default:
		return 0;
	}
}

HostFlagCorrection host_flag_correction(const CpuState &cpu,
	const Gadget &gadget, ::uml::memory::GuestAddressSpace &memory)
{
	constexpr std::uint32_t invalid = 0x01U;
	const auto &instruction = gadget.instruction;
	const bool denormals_are_zero =
		(cpu.mxcsr & mxcsr_denormals_are_zero) != 0;
	const unsigned rounding = cpu.mxcsr >> 13U;
	const auto read = [&](const Operand &operand, VectorValue &value) {
		return operand_value(cpu, gadget, operand, memory, value);
	};
	switch (instruction.operation) {
	case Operation::vector_alu: {
		const auto opcode = instruction.vector_opcode;
		const bool horizontal = opcode == 0x7c || opcode == 0x7d ||
			opcode == 0xd0;
		if (horizontal || opcode == 0x58 || opcode == 0x59 ||
			opcode == 0x5c || opcode == 0x5e) {
			/* ADD, MUL, SUB and DIV, PS, PD, SS and SD with none, 66, F3
			 * and F2; HADD and HSUB, which add or subtract adjacent pairs
			 * of either source, and ADDSUB, which subtracts in its even
			 * lanes, PD with 66 and PS with F2. A quotient never lies close
			 * enough below the smallest normal to round up to it, nor a
			 * sum, which is exact that small, but either may be tiny and
			 * exact. */
			const auto prefix = instruction.vector_prefix;
			const bool dbl = horizontal ? prefix == 1 :
				prefix == 1 || prefix == 3;
			const FloatFormat f{dbl};
			const std::size_t lanes = !horizontal && prefix >= 2 ? 1U :
				vector_lanes(instruction.destination, dbl);
			VectorValue first{};
			VectorValue second{};
			if (!read(instruction.source, first) ||
				!read(instruction.auxiliary, second))
				return {};
			UnderflowModel model{cpu.mxcsr, true};
			const auto compute = [&](std::uint64_t x, std::uint64_t y,
				bool subtract) {
				const auto a = soft_wide(dbl, f.input(x, denormals_are_zero));
				const auto b = soft_wide(dbl, f.input(
					subtract ? y ^ f.sign() : y, denormals_are_zero));
				model.record(soft_round(soft_odd([&](auto &state) {
					return opcode == 0x59 ? f128_mul(&state, a, b) :
						opcode == 0x5e ? f128_div(&state, a, b) :
						f128_add(&state, a, b);
				}), dbl, rounding));
			};
			if (opcode == 0x7c || opcode == 0x7d) {
				for (const auto *source : {&first, &second})
					for (std::size_t pair = 0; pair < lanes; pair += 2)
						compute(value_lane(*source, dbl, pair),
							value_lane(*source, dbl, pair + 1U),
							opcode == 0x7d);
			} else {
				for (std::size_t index = 0; index < lanes; ++index)
					compute(value_lane(first, dbl, index),
						value_lane(second, dbl, index), opcode == 0x5c ||
							(opcode == 0xd0 && (index & 1U) == 0));
			}
			return model.correction();
		}
		if (opcode != 0xc2)
			return {};
		/* CMPPS, CMPPD, CMPSS and CMPSD raise invalid operation for a
		 * signalling NaN, and under a signalling predicate for a quiet one
		 * too. The legacy encoding has predicates 0 to 7; VEX's bit 4 makes
		 * a quiet predicate of the low four bits signalling and the
		 * reverse, which the host's comparison does not follow, and its
		 * false and true predicates compare nothing. */
		const unsigned predicate = instruction.vex ?
			instruction.vector_control & 0x1fU :
			instruction.vector_control & 0x7U;
		constexpr std::uint32_t signalling_predicates = 0x6666U;
		const bool signalling =
			(((signalling_predicates >> (predicate & 0xfU)) & 1U) != 0) !=
			((predicate & 0x10U) != 0);
		const auto prefix = instruction.vector_prefix;
		const bool dbl = prefix == 1 || prefix == 3;
		const FloatFormat f{dbl};
		const std::size_t lanes = prefix >= 2 ? 1U :
			vector_lanes(instruction.destination, dbl);
		VectorValue first{};
		VectorValue second{};
		if (!operand_value(cpu, gadget, instruction.source, memory, first) ||
			!operand_value(cpu, gadget, instruction.auxiliary, memory,
				second))
			return {};
		HostFlagCorrection correction{.decided = invalid};
		for (std::size_t index = 0; index < lanes; ++index) {
			const auto a = value_lane(first, dbl, index);
			const auto b = value_lane(second, dbl, index);
			if (f.signalling(a) || f.signalling(b) ||
				(signalling && (f.nan(a) || f.nan(b))))
				correction.raised |= invalid;
		}
		return correction;
	}
	case Operation::vector_fma: {
		FmaOperands fma{};
		if (!read_fma_operands(cpu, gadget, memory, fma))
			return {};
		const FloatFormat f{fma.dbl};
		std::uint32_t raised{};
		UnderflowModel model{cpu.mxcsr, true};
		for (std::size_t index = 0; index < fma.lanes; ++index) {
			const auto a = f.input(value_lane(fma.a, fma.dbl, index),
				denormals_are_zero);
			const auto b = f.input(value_lane(fma.b, fma.dbl, index),
				denormals_are_zero);
			const auto c = f.input(value_lane(fma.c, fma.dbl, index),
				denormals_are_zero);
			const bool product_negative =
				(f.negative(a) != f.negative(b)) != fma.negate_product();
			const bool addend_negative =
				f.negative(c) != fma.negate_addend(index);
			if (fma_lane_invalid(f, a, b, c, product_negative,
					addend_negative))
				raised |= invalid;
			const auto x = soft_wide(fma.dbl,
				fma.negate_product() ? a ^ f.sign() : a);
			const auto y = soft_wide(fma.dbl, b);
			const auto z = soft_wide(fma.dbl,
				fma.negate_addend(index) ? c ^ f.sign() : c);
			model.record(soft_round(soft_odd([&](auto &state) {
				return f128_add(&state, f128_mul(&state, x, y), z);
			}), fma.dbl, rounding));
		}
		return model.correction(invalid, raised);
	}
	case Operation::vector_convert: {
		const auto prefix = instruction.vector_prefix;
		const auto opcode = instruction.vector_opcode;
		/* CVTPS2DQ and CVTTPS2DQ with 66 and F3, and CVTTPD2DQ and
		 * CVTPD2DQ with 66 and F2, convert to integers. */
		if ((opcode == 0x5b && (prefix == 1 || prefix == 2)) ||
			(opcode == 0xe6 && (prefix == 1 || prefix == 3)))
			return integer_conversion_precision(cpu, gadget, memory,
				opcode == 0xe6, prefix == 2 || (opcode == 0xe6 && prefix == 1),
				false);
		/* CVTPD2PS and CVTSD2SS narrow doubles, with 66 and F2. */
		if (opcode != 0x5a || (prefix != 1 && prefix != 3))
			return {};
		VectorValue source{};
		if (!read(instruction.auxiliary, source))
			return {};
		const std::size_t lanes = prefix == 3 ? 1U :
			vector_lanes(instruction.auxiliary, true);
		UnderflowModel model{cpu.mxcsr, true};
		for (std::size_t index = 0; index < lanes; ++index)
			model.record(soft_round(soft_wide(true, FloatFormat{true}.input(
					value_lane(source, true, index), denormals_are_zero)),
				false, rounding));
		return model.correction();
	}
	case Operation::vector_scalar_convert: {
		/* CVTTPS2PI, CVTTPD2PI, CVTTSS2SI and CVTTSD2SI truncate, and
		 * the CVT forms round as MXCSR does, with none, 66, F3 and F2. */
		const auto opcode = instruction.vector_opcode;
		const auto prefix = instruction.vector_prefix;
		if (opcode != 0x2c && opcode != 0x2d)
			return {};
		return integer_conversion_precision(cpu, gadget, memory,
			prefix == 1 || prefix == 3, opcode == 0x2c, prefix >= 2 &&
				instruction.destination.width == OperandWidth::qword);
	}
	case Operation::vector_integer_map3: {
		const auto opcode = instruction.vector_opcode;
		const unsigned control = instruction.vector_control;
		if (opcode >= 0x08 && opcode <= 0x0b) {
			/* ROUNDPS, ROUNDPD, ROUNDSS and ROUNDSD raise precision for a
			 * lane that is not an integer, unless bit 3 of the immediate
			 * suppresses it, whatever the rounding mode. FRINT does not
			 * raise it. */
			const bool dbl = (opcode & 1U) != 0;
			const FloatFormat f{dbl};
			VectorValue source{};
			if (!read(instruction.auxiliary, source))
				return {};
			const std::size_t lanes = opcode >= 0x0a ? 1U :
				vector_lanes(instruction.destination, dbl);
			std::uint32_t raised{};
			for (std::size_t index = 0; index < lanes; ++index) {
				const auto x = f.input(value_lane(source, dbl, index),
					denormals_are_zero);
				if ((control & 8U) == 0 && !f.nan(x) && !f.infinite(x) &&
					!integral(f, x))
					raised = UnderflowModel::precision;
			}
			return {UnderflowModel::precision, raised};
		}
		if (opcode == 0x40 || opcode == 0x41) {
			VectorValue first{};
			VectorValue second{};
			if (!read(instruction.source, first) ||
				!read(instruction.auxiliary, second))
				return {};
			return dot_product_flags(cpu.mxcsr, opcode == 0x41, control,
				instruction.destination.width == OperandWidth::ymm ? 2U : 1U,
				first, second);
		}
		/* VCVTPS2PH narrows singles, in the rounding mode its immediate
		 * names unless its bit 2 defers to MXCSR's. FTZ does not apply. */
		if (opcode != 0x1d)
			return {};
		VectorValue source{};
		if (!read(instruction.source, source))
			return {};
		const auto lanes = vector_lanes(instruction.source, false);
		UnderflowModel model{cpu.mxcsr, false};
		for (std::size_t index = 0; index < lanes; ++index)
			model.record(half_round(static_cast<std::uint32_t>(
					FloatFormat{false}.input(value_lane(source, false, index),
						denormals_are_zero)),
				(control & 4U) != 0 ? rounding : control));
		return model.correction();
	}
	default:
		return {};
	}
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
