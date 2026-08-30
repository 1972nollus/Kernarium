// SPDX-License-Identifier: GPL-2.0-or-later
#include "program.hpp"

#include <SoftFloat-3e/platform.h>
#include <cephes_128bit.h>

extern "C" {
#include <SoftFloat-3e/softfloat.h>
}

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace uml::cpu::x86_64::gadgets {
namespace {

constexpr std::uint16_t status_invalid = 1U << 0U;
constexpr std::uint16_t status_zero_divide = 1U << 2U;
constexpr std::uint16_t status_overflow = 1U << 3U;
constexpr std::uint16_t status_underflow = 1U << 4U;
constexpr std::uint16_t status_precision = 1U << 5U;
constexpr std::uint16_t status_stack_fault = 1U << 6U;
constexpr std::uint16_t status_error_summary = 1U << 7U;
constexpr std::uint16_t status_c0 = 1U << 8U;
constexpr std::uint16_t status_c1 = 1U << 9U;
constexpr std::uint16_t status_c2 = 1U << 10U;
constexpr std::uint16_t status_top = 7U << 11U;
constexpr std::uint16_t status_c3 = 1U << 14U;
constexpr std::uint16_t status_exception_mask = 0x7fU;

constexpr std::uint8_t tag_valid = 0;
constexpr std::uint8_t tag_zero = 1;
constexpr std::uint8_t tag_special = 2;
constexpr std::uint8_t tag_empty = 3;

[[nodiscard]] std::uint64_t effective_address(const Gadget &gadget,
	const CpuState &cpu, const MemoryOperand &operand) noexcept
{
	std::uint64_t address = operand.absolute_address ? operand.absolute :
		static_cast<std::uint64_t>(
			static_cast<std::int64_t>(operand.displacement));
	if (!operand.absolute_address) {
		if (operand.rip_relative)
			address += gadget.address + gadget.instruction.length;
		if (operand.base != MemoryOperand::no_register)
			address += operand.address_32 ?
				static_cast<std::uint32_t>(cpu.registers[operand.base]) :
				cpu.registers[operand.base];
		if (operand.index != MemoryOperand::no_register)
			address += (operand.address_32 ?
				static_cast<std::uint32_t>(cpu.registers[operand.index]) :
				cpu.registers[operand.index]) * operand.scale;
		if (operand.address_32)
			address = static_cast<std::uint32_t>(address);
	}
	if (operand.segment == Segment::fs)
		address += cpu.fs_base;
	else if (operand.segment == Segment::gs)
		address += cpu.gs_base;
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

[[nodiscard]] bool read_bytes(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, std::span<std::byte> bytes) noexcept
{
	try {
		return memory.read(address, bytes);
	} catch (...) {
		return false;
	}
}

[[nodiscard]] bool write_bytes(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, std::span<const std::byte> bytes) noexcept
{
	try {
		return memory.write(address, bytes);
	} catch (...) {
		return false;
	}
}

void advance(const Gadget &gadget, CpuState &cpu) noexcept
{
	cpu.rip = gadget.address + gadget.instruction.length;
}

[[nodiscard]] StepResult read_failure(
	::uml::memory::GuestAddressSpace &memory, std::uint64_t address) noexcept
{
	return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
		memory_fault(memory, address, ::uml::memory::MemoryAccess::read)};
}

[[nodiscard]] StepResult write_failure(
	::uml::memory::GuestAddressSpace &memory, std::uint64_t address) noexcept
{
	return {StepAction::exit, ::uml::cpu::EngineEvent::memory_fault,
		memory_fault(memory, address, ::uml::memory::MemoryAccess::write)};
}

[[nodiscard]] constexpr std::uint8_t physical_register(
	const CpuState &cpu, std::uint8_t logical) noexcept
{
	return static_cast<std::uint8_t>((cpu.x87_top + logical) & 7U);
}

[[nodiscard]] constexpr std::uint8_t register_tag(
	const CpuState &cpu, std::uint8_t physical) noexcept
{
	return static_cast<std::uint8_t>(
		(cpu.x87_tag >> (physical * 2U)) & 3U);
}

void set_register_tag(CpuState &cpu, std::uint8_t physical,
	std::uint8_t tag) noexcept
{
	const auto shift = static_cast<unsigned>(physical * 2U);
	cpu.x87_tag = static_cast<std::uint16_t>(
		(cpu.x87_tag & ~(3U << shift)) | ((tag & 3U) << shift));
}

[[nodiscard]] constexpr std::uint8_t classify_tag(
	const X87Register &value) noexcept
{
	const auto exponent = static_cast<std::uint16_t>(
		value.sign_exponent & 0x7fffU);
	if (exponent == 0 && value.significand == 0)
		return tag_zero;
	if (exponent == 0 || exponent == 0x7fffU ||
		(value.significand & (std::uint64_t{1} << 63U)) == 0)
		return tag_special;
	return tag_valid;
}

void store_physical(CpuState &cpu, std::uint8_t physical,
	const X87Register &value) noexcept
{
	cpu.x87[physical] = value;
	cpu.mmx[physical] = value.significand;
	set_register_tag(cpu, physical, classify_tag(value));
}

void store_stack(CpuState &cpu, std::uint8_t logical,
	const X87Register &value) noexcept
{
	store_physical(cpu, physical_register(cpu, logical), value);
}

[[nodiscard]] X87Register load_stack(
	CpuState &cpu, std::uint8_t logical) noexcept
{
	const auto physical = physical_register(cpu, logical);
	if (register_tag(cpu, physical) == tag_empty) {
		cpu.x87_status |= status_invalid | status_stack_fault;
		return {0xc000000000000000ULL, 0xffffU};
	}
	return cpu.x87[physical];
}

void push(CpuState &cpu, const X87Register &value) noexcept
{
	cpu.x87_status &= ~status_c1;
	const auto next = static_cast<std::uint8_t>((cpu.x87_top - 1U) & 7U);
	if (register_tag(cpu, next) != tag_empty)
		cpu.x87_status |= status_invalid | status_stack_fault | status_c1;
	cpu.x87_top = next;
	store_physical(cpu, next, value);
}

void pop(CpuState &cpu) noexcept
{
	set_register_tag(cpu, cpu.x87_top, tag_empty);
	cpu.x87_top = static_cast<std::uint8_t>((cpu.x87_top + 1U) & 7U);
}

[[nodiscard]] constexpr extFloat80_t to_soft(
	const X87Register &value) noexcept
{
	return {value.significand, value.sign_exponent};
}

[[nodiscard]] constexpr X87Register from_soft(
	extFloat80_t value) noexcept
{
	return {value.signif, value.signExp};
}

[[nodiscard]] softfloat_state soft_state(const CpuState &cpu,
	bool force_80_bit_precision = false) noexcept
{
	softfloat_state state{};
	state.detectTininess = softfloat_tininess_afterRounding;
	switch ((cpu.x87_control >> 10U) & 3U) {
	case 1: state.roundingMode = softfloat_round_min; break;
	case 2: state.roundingMode = softfloat_round_max; break;
	case 3: state.roundingMode = softfloat_round_minMag; break;
	default: state.roundingMode = softfloat_round_near_even; break;
	}
	if (force_80_bit_precision) {
		state.roundingPrecision = 80;
	} else {
		switch ((cpu.x87_control >> 8U) & 3U) {
		case 0: state.roundingPrecision = 32; break;
		case 2: state.roundingPrecision = 64; break;
		default: state.roundingPrecision = 80; break;
		}
	}
	return state;
}

void commit_exceptions(CpuState &cpu,
	const softfloat_state &state) noexcept
{
	if ((state.exceptionFlags & softfloat_flag_invalid) != 0)
		cpu.x87_status |= status_invalid;
	if ((state.exceptionFlags & softfloat_flag_infinite) != 0)
		cpu.x87_status |= status_zero_divide;
	if ((state.exceptionFlags & softfloat_flag_overflow) != 0)
		cpu.x87_status |= status_overflow;
	if ((state.exceptionFlags & softfloat_flag_underflow) != 0)
		cpu.x87_status |= status_underflow;
	if ((state.exceptionFlags & softfloat_flag_inexact) != 0)
		cpu.x87_status |= status_precision;
	const auto pending = static_cast<std::uint16_t>(
		cpu.x87_status & status_exception_mask & ~cpu.x87_control);
	if (pending != 0)
		cpu.x87_status |= status_error_summary;
}

[[nodiscard]] X87Register from_float32(CpuState &cpu,
	std::uint32_t bits) noexcept
{
	auto state = soft_state(cpu);
	const auto result = f32_to_extF80(&state, float32_t{bits});
	commit_exceptions(cpu, state);
	return from_soft(result);
}

[[nodiscard]] X87Register from_float64(CpuState &cpu,
	std::uint64_t bits) noexcept
{
	auto state = soft_state(cpu);
	const auto result = f64_to_extF80(&state, float64_t{bits});
	commit_exceptions(cpu, state);
	return from_soft(result);
}

[[nodiscard]] X87Register from_i32(std::int32_t value) noexcept
{
	return from_soft(i32_to_extF80(value));
}

[[nodiscard]] X87Register from_i64(std::int64_t value) noexcept
{
	const bool negative = value < 0;
	const auto magnitude = negative ?
		std::uint64_t{0} - static_cast<std::uint64_t>(value) :
		static_cast<std::uint64_t>(value);
	auto result = from_soft(ui64_to_extF80(magnitude));
	if (negative)
		result.sign_exponent |= 0x8000U;
	return result;
}

[[nodiscard]] std::uint32_t to_float32(
	CpuState &cpu, X87Register value) noexcept
{
	auto state = soft_state(cpu);
	const auto result = extF80_to_f32(&state, to_soft(value));
	commit_exceptions(cpu, state);
	return result.v;
}

[[nodiscard]] std::uint64_t to_float64(
	CpuState &cpu, X87Register value) noexcept
{
	auto state = soft_state(cpu);
	const auto result = extF80_to_f64(&state, to_soft(value));
	commit_exceptions(cpu, state);
	return result.v;
}

[[nodiscard]] double to_double(CpuState &cpu, X87Register value) noexcept
{
	return std::bit_cast<double>(to_float64(cpu, value));
}

[[nodiscard]] X87Register from_double(CpuState &cpu, double value) noexcept
{
	return from_float64(cpu, std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] constexpr bool is_zero(const X87Register &value) noexcept
{
	return (value.sign_exponent & 0x7fffU) == 0 && value.significand == 0;
}

[[nodiscard]] constexpr bool is_infinity(const X87Register &value) noexcept
{
	return (value.sign_exponent & 0x7fffU) == 0x7fffU &&
		value.significand == 0x8000000000000000ULL;
}

[[nodiscard]] constexpr bool is_nan(const X87Register &value) noexcept
{
	return (value.sign_exponent & 0x7fffU) == 0x7fffU &&
		(value.significand & 0x7fffffffffffffffULL) != 0;
}

[[nodiscard]] constexpr X87Register quiet_nan() noexcept
{
	return {0xc000000000000000ULL, 0x7fffU};
}

[[nodiscard]] X87Register invalid_result(CpuState &cpu) noexcept
{
	auto state = soft_state(cpu, true);
	state.exceptionFlags |= softfloat_flag_invalid;
	commit_exceptions(cpu, state);
	return quiet_nan();
}

[[nodiscard]] float128_t to_float128(softfloat_state &state,
	X87Register value) noexcept
{
	return extF80_to_f128(&state, to_soft(value));
}

[[nodiscard]] X87Register from_float128(softfloat_state &state,
	float128_t value) noexcept
{
	return from_soft(f128_to_extF80(&state, value));
}

enum class TranscendentalOperation : std::uint8_t {
	sine,
	cosine,
	tangent,
};

[[nodiscard]] X87Register transcendental(CpuState &cpu,
	TranscendentalOperation operation, X87Register value) noexcept
{
	if (is_infinity(value))
		return invalid_result(cpu);
	if (cpu.x87_reduced_precision) {
		const auto input = to_double(cpu, value);
		double result{};
		switch (operation) {
		case TranscendentalOperation::sine: result = std::sin(input); break;
		case TranscendentalOperation::cosine: result = std::cos(input); break;
		case TranscendentalOperation::tangent: result = std::tan(input); break;
		}
		return from_double(cpu, result);
	}
	auto state = soft_state(cpu, true);
	const auto input = to_float128(state, value);
	float128_t result{};
	switch (operation) {
	case TranscendentalOperation::sine:
		result = FEXCore::cephes_128bit::sinl(input);
		break;
	case TranscendentalOperation::cosine:
		result = FEXCore::cephes_128bit::cosl(input);
		break;
	case TranscendentalOperation::tangent:
		result = FEXCore::cephes_128bit::tanl(input);
		break;
	}
	const auto converted = from_float128(state, result);
	commit_exceptions(cpu, state);
	return converted;
}

[[nodiscard]] X87Register f2xm1(CpuState &cpu,
	X87Register value) noexcept
{
	if (cpu.x87_reduced_precision) {
		const auto input = to_double(cpu, value);
		if (std::abs(input) > 1.0)
			return from_double(cpu, std::exp2(input) - 1.0);
		const auto integral = std::round(input);
		const auto remainder = input - integral;
		const auto scale_bits = static_cast<std::uint64_t>(
			static_cast<std::int64_t>(integral) + 1023) << 52U;
		const auto scale = std::bit_cast<double>(scale_bits);
		constexpr std::array<std::uint64_t, 13> coefficient_bits{
			0x3fe62e42fefa39efULL, 0x3fcebfbdff82c58eULL,
			0x3fac6b08d704a0beULL, 0x3f83b2ab6fba4e76ULL,
			0x3f55d87fe78a672fULL, 0x3f2430912f86c785ULL,
			0x3eeffcbfc588b0c2ULL, 0x3eb62c0223a5c821ULL,
			0x3e7b5253d395e7c0ULL, 0x3e3e4cf5158b8ec5ULL,
			0x3dfe8cac7351bb20ULL, 0x3dbc3bd650fc2981ULL,
			0x3d7816193166d0f5ULL,
		};
		auto polynomial = std::bit_cast<double>(coefficient_bits.back());
		for (std::size_t index = coefficient_bits.size() - 1; index != 0;
			--index) {
			polynomial = std::fma(remainder, polynomial,
				std::bit_cast<double>(coefficient_bits[index - 1]));
		}
		return from_double(cpu, std::fma(scale,
			remainder * polynomial, scale - 1.0));
	}
	auto state = soft_state(cpu, true);
	const float128_t one{0, 0x3fff000000000000ULL};
	const auto result = f128_sub(&state,
		FEXCore::cephes_128bit::exp2l(to_float128(state, value)), one);
	const auto converted = from_float128(state, result);
	commit_exceptions(cpu, state);
	return converted;
}

[[nodiscard]] X87Register fyl2x(CpuState &cpu, X87Register value,
	X87Register multiplier, bool plus_one) noexcept
{
	if (cpu.x87_reduced_precision) {
		const auto input = to_double(cpu, value);
		const auto right = to_double(cpu, multiplier);
		return from_double(cpu, right * std::log2(
			plus_one ? input + 1.0 : input));
	}
	auto state = soft_state(cpu, true);
	auto input = to_float128(state, value);
	if (plus_one) {
		const float128_t one{0, 0x3fff000000000000ULL};
		input = f128_add(&state, input, one);
	}
	const auto result = f128_mul(&state, to_float128(state, multiplier),
		FEXCore::cephes_128bit::log2l(input));
	const auto converted = from_float128(state, result);
	commit_exceptions(cpu, state);
	return converted;
}

[[nodiscard]] X87Register fpatan(CpuState &cpu, X87Register y,
	X87Register x) noexcept
{
	if (cpu.x87_reduced_precision)
		return from_double(cpu,
			std::atan2(to_double(cpu, y), to_double(cpu, x)));
	auto state = soft_state(cpu, true);
	const auto result = FEXCore::cephes_128bit::atan2l(
		to_float128(state, y), to_float128(state, x));
	const auto converted = from_float128(state, result);
	commit_exceptions(cpu, state);
	return converted;
}

[[nodiscard]] X87Register scale(CpuState &cpu, X87Register value,
	X87Register exponent) noexcept
{
	if (cpu.x87_reduced_precision) {
		if (is_zero(value))
			return value;
		return from_double(cpu, to_double(cpu, value) *
			std::exp2(std::trunc(to_double(cpu, exponent))));
	}
	auto state = soft_state(cpu, true);
	const extFloat80_t zero{};
	if (extF80_eq(&state, to_soft(value), zero)) {
		if (is_infinity(exponent) &&
			(exponent.sign_exponent & 0x8000U) == 0) {
			state.exceptionFlags |= softfloat_flag_invalid;
			commit_exceptions(cpu, state);
			return quiet_nan();
		}
		commit_exceptions(cpu, state);
		return value;
	}
	const auto integer = extF80_roundToInt(&state, to_soft(exponent),
		softfloat_round_minMag, false);
	const auto power = FEXCore::cephes_128bit::exp2l(
		extF80_to_f128(&state, integer));
	const auto factor = f128_to_extF80(&state, power);
	const auto result = extF80_mul(&state, to_soft(value), factor);
	commit_exceptions(cpu, state);
	return from_soft(result);
}

[[nodiscard]] X87Register remainder(CpuState &cpu, X87Register dividend,
	X87Register divisor, bool nearest) noexcept
{
	if (cpu.x87_reduced_precision) {
		const auto left = to_double(cpu, dividend);
		const auto right = to_double(cpu, divisor);
		return from_double(cpu, nearest ? std::remainder(left, right) :
			std::fmod(left, right));
	}
	auto state = soft_state(cpu, true);
	extFloat80_t result{};
	if (nearest) {
		result = extF80_rem(&state, to_soft(dividend), to_soft(divisor));
	} else if (is_infinity(dividend)) {
		state.exceptionFlags |= softfloat_flag_invalid;
		result = to_soft(quiet_nan());
	} else if (is_nan(dividend)) {
		state.exceptionFlags |= softfloat_flag_invalid;
		result = to_soft(dividend);
	} else if (is_zero(divisor)) {
		state.exceptionFlags |= softfloat_flag_invalid;
		result = to_soft(quiet_nan());
	} else {
		const auto quotient = extF80_div(
			&state, to_soft(dividend), to_soft(divisor));
		const auto integer = extF80_roundToInt(&state, quotient,
			softfloat_round_minMag, true);
		if (integer.signif == 0 && (integer.signExp & 0x7fffU) == 0) {
			result = to_soft(dividend);
		} else {
			result = extF80_sub(&state, to_soft(dividend),
				extF80_mul(&state, integer, to_soft(divisor)));
		}
	}
	commit_exceptions(cpu, state);
	return from_soft(result);
}

[[nodiscard]] X87Register extract_significand(
	X87Register value) noexcept
{
	const auto exponent = value.sign_exponent & 0x7fffU;
	if (exponent == 0 || exponent == 0x7fffU)
		return value;
	value.sign_exponent = static_cast<std::uint16_t>(
		(value.sign_exponent & 0x8000U) | 0x3fffU);
	return value;
}

[[nodiscard]] X87Register extract_exponent(X87Register value) noexcept
{
	const auto exponent = value.sign_exponent & 0x7fffU;
	if (exponent == 0 && value.significand == 0)
		return {0x8000000000000000ULL, 0xffffU};
	if (exponent == 0x7fffU) {
		if (is_infinity(value))
			return {0x8000000000000000ULL, 0x7fffU};
		return value;
	}
	return from_i32(static_cast<std::int32_t>(exponent) - 16383);
}

enum class BinaryOperation : std::uint8_t {
	add,
	multiply,
	subtract,
	divide,
};

[[nodiscard]] X87Register binary(CpuState &cpu, BinaryOperation operation,
	X87Register left, X87Register right) noexcept
{
	auto state = soft_state(cpu);
	extFloat80_t result{};
	switch (operation) {
	case BinaryOperation::add:
		result = extF80_add(&state, to_soft(left), to_soft(right));
		break;
	case BinaryOperation::multiply:
		result = extF80_mul(&state, to_soft(left), to_soft(right));
		break;
	case BinaryOperation::subtract:
		result = extF80_sub(&state, to_soft(left), to_soft(right));
		break;
	case BinaryOperation::divide:
		result = extF80_div(&state, to_soft(left), to_soft(right));
		break;
	}
	commit_exceptions(cpu, state);
	return from_soft(result);
}

enum class CompareResult : std::uint8_t {
	greater,
	less,
	equal,
	unordered,
};

[[nodiscard]] CompareResult compare(CpuState &cpu, X87Register left,
	X87Register right) noexcept
{
	if (is_nan(left) || is_nan(right))
		return CompareResult::unordered;
	auto state = soft_state(cpu);
	const bool equal = extF80_eq(&state, to_soft(left), to_soft(right));
	const bool less = !equal && extF80_lt(&state, to_soft(left), to_soft(right));
	commit_exceptions(cpu, state);
	return equal ? CompareResult::equal :
		(less ? CompareResult::less : CompareResult::greater);
}

void set_x87_compare(CpuState &cpu, CompareResult result) noexcept
{
	cpu.x87_status &= ~(status_c0 | status_c1 | status_c2 | status_c3);
	if (result == CompareResult::less)
		cpu.x87_status |= status_c0;
	else if (result == CompareResult::equal)
		cpu.x87_status |= status_c3;
	else if (result == CompareResult::unordered)
		cpu.x87_status |= status_c0 | status_c2 | status_c3;
}

void set_integer_compare(CpuState &cpu, CompareResult result) noexcept
{
	materialize_flags(cpu);
	constexpr auto affected = rflags_carry | rflags_parity | rflags_zero |
		rflags_overflow | rflags_sign | rflags_auxiliary;
	cpu.rflags &= ~affected;
	if (result == CompareResult::less)
		cpu.rflags |= rflags_carry;
	else if (result == CompareResult::equal)
		cpu.rflags |= rflags_zero;
	else if (result == CompareResult::unordered)
		cpu.rflags |= rflags_carry | rflags_parity | rflags_zero;
	cpu.rflags |= rflags_reserved;
}

[[nodiscard]] std::uint16_t status_word(const CpuState &cpu) noexcept
{
	return static_cast<std::uint16_t>((cpu.x87_status & ~status_top) |
		((cpu.x87_top & 7U) << 11U));
}

void load_status_word(CpuState &cpu, std::uint16_t value) noexcept
{
	cpu.x87_top = static_cast<std::uint8_t>((value >> 11U) & 7U);
	cpu.x87_status = static_cast<std::uint16_t>(value & ~status_top);
}

void initialize(CpuState &cpu) noexcept
{
	cpu.x87_control = 0x037f;
	cpu.x87_status = 0;
	cpu.x87_tag = 0xffff;
	cpu.x87_top = 0;
}

[[nodiscard]] X87Register memory_number(CpuState &cpu,
	std::uint8_t opcode, std::span<const std::byte> bytes) noexcept
{
	if (opcode == 0xd8 || opcode == 0xd9 || opcode == 0xda ||
		opcode == 0xdb || opcode == 0xde || opcode == 0xdf) {
		std::uint32_t value{};
		if (opcode == 0xde || opcode == 0xdf) {
			std::int16_t integer{};
			std::memcpy(&integer, bytes.data(), sizeof(integer));
			return from_i32(integer);
		}
		std::memcpy(&value, bytes.data(), sizeof(value));
		if (opcode == 0xd8 || opcode == 0xd9)
			return from_float32(cpu, value);
		return from_i32(static_cast<std::int32_t>(value));
	}
	std::uint64_t value{};
	std::memcpy(&value, bytes.data(), sizeof(value));
	return opcode == 0xdc || opcode == 0xdd ?
		from_float64(cpu, value) : from_i64(static_cast<std::int64_t>(value));
}

[[nodiscard]] std::size_t number_size(
	std::uint8_t opcode, std::uint8_t group) noexcept
{
	if ((opcode == 0xdb && group == 5) ||
		(opcode == 0xdb && group == 7) ||
		(opcode == 0xdf && (group == 4 || group == 6)))
		return 10;
	if (opcode == 0xdc || opcode == 0xdd ||
		(opcode == 0xdf && (group == 5 || group == 7)))
		return 8;
	if (opcode == 0xde || opcode == 0xdf)
		return 2;
	return 4;
}

[[nodiscard]] StepResult write_memory_result(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, std::span<const std::byte> bytes) noexcept
{
	if (!write_bytes(memory, address, bytes))
		return write_failure(memory, address);
	advance(gadget, cpu);
	return {StepAction::continue_program,
		::uml::cpu::EngineEvent::continued, {}, address,
		static_cast<std::uint8_t>(bytes.size())};
}

[[nodiscard]] bool condition_holds(CpuState &cpu,
	std::uint8_t group) noexcept
{
	materialize_flags(cpu);
	const bool carry = (cpu.rflags & rflags_carry) != 0;
	const bool zero = (cpu.rflags & rflags_zero) != 0;
	const bool parity = (cpu.rflags & rflags_parity) != 0;
	switch (group) {
	case 0: return carry;
	case 1: return zero;
	case 2: return carry || zero;
	case 3: return parity;
	case 4: return !carry;
	case 5: return !zero;
	case 6: return !carry && !zero;
	case 7: return !parity;
	default: return false;
	}
}

[[nodiscard]] X87Register constant(std::uint8_t selector) noexcept
{
	switch (selector) {
	case 0: return {0x8000000000000000ULL, 0x3fffU};
	case 1: return {0xd49a784bcd1b8afeULL, 0x4000U};
	case 2: return {0xb8aa3b295c17f0bcULL, 0x3fffU};
	case 3: return {0xc90fdaa22168c235ULL, 0x4000U};
	case 4: return {0x9a209a84fbcff799ULL, 0x3ffdU};
	case 5: return {0xb17217f7d1cf79acULL, 0x3ffeU};
	default: return {};
	}
}

[[nodiscard]] X87Register packed_bcd_load(CpuState &cpu,
	std::span<const std::byte> bytes) noexcept
{
	std::uint64_t value{};
	std::uint64_t multiplier{1};
	for (std::size_t index = 0; index < 9; ++index) {
		const auto byte = std::to_integer<std::uint8_t>(bytes[index]);
		value += (byte & 0xfU) * multiplier;
		multiplier *= 10;
		value += (byte >> 4U) * multiplier;
		multiplier *= 10;
	}
	auto result = from_soft(ui64_to_extF80(value));
	if ((std::to_integer<std::uint8_t>(bytes[9]) & 0x80U) != 0)
		result.sign_exponent |= 0x8000U;
	(void)cpu;
	return result;
}

void packed_bcd_store(CpuState &cpu, X87Register value,
	std::span<std::byte, 10> bytes) noexcept
{
	auto state = soft_state(cpu);
	const bool negative = (value.sign_exponent & 0x8000U) != 0;
	value = from_soft(extF80_roundToInt(&state, to_soft(value),
		state.roundingMode, false));
	value.sign_exponent &= 0x7fffU;
	auto integer = extF80_to_ui64(&state, to_soft(value),
		state.roundingMode, false);
	commit_exceptions(cpu, state);
	for (std::size_t index = 0; index < 9; ++index) {
		const auto low = integer % 10U;
		integer /= 10U;
		const auto high = integer % 10U;
		integer /= 10U;
		bytes[index] = static_cast<std::byte>(low | (high << 4U));
	}
	bytes[9] = negative ? std::byte{0x80} : std::byte{};
}

} // namespace

[[nodiscard]] StepResult execute_extended_state(const Gadget &gadget,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory) noexcept
{
	constexpr std::size_t legacy_size = 512;
	constexpr std::size_t xsave_header_size = 64;
	constexpr std::size_t ymm_high_size = 16 * 16;
	constexpr std::size_t xsave_size = legacy_size +
		xsave_header_size + ymm_high_size;
	constexpr std::uint64_t x87_component = 1U << 0U;
	constexpr std::uint64_t sse_component = 1U << 1U;
	constexpr std::uint64_t avx_component = 1U << 2U;
	constexpr std::uint64_t supported_components =
		x87_component | sse_component | avx_component;

	const auto &instruction = gadget.instruction;
	const auto group = instruction.vector_group;
	const bool legacy = group <= 1;
	const bool restore = group == 1 || group == 5;
	const auto size = legacy ? legacy_size : xsave_size;
	const auto address = effective_address(
		gadget, cpu, instruction.destination.memory);
	std::array<std::byte, xsave_size> bytes{};
	if (!read_bytes(memory, address, std::span{bytes}.first(size)))
		return read_failure(memory, address);

	auto requested = supported_components;
	if (!legacy) {
		requested = (static_cast<std::uint64_t>(
			static_cast<std::uint32_t>(cpu.gpr(Register::rdx))) << 32U) |
			static_cast<std::uint32_t>(cpu.gpr(Register::rax));
		requested &= supported_components;
	}

	if (!restore) {
		if (!legacy) {
			std::memcpy(bytes.data() + legacy_size, &requested,
				sizeof(requested));
			std::fill(bytes.begin() + legacy_size + 8,
				bytes.begin() + legacy_size + xsave_header_size,
				std::byte{});
		}
		if ((requested & x87_component) != 0) {
			std::memcpy(bytes.data(), &cpu.x87_control,
				sizeof(cpu.x87_control));
			const auto status = status_word(cpu);
			std::memcpy(bytes.data() + 2, &status, sizeof(status));
			std::uint8_t abridged_tag{};
			for (std::uint8_t physical = 0; physical < 8; ++physical) {
				if (register_tag(cpu, physical) != tag_empty)
					abridged_tag |= static_cast<std::uint8_t>(1U << physical);
			}
			bytes[4] = static_cast<std::byte>(abridged_tag);
			std::fill(bytes.begin() + 6, bytes.begin() + 24, std::byte{});
			for (std::uint8_t logical = 0; logical < 8; ++logical) {
				const auto &value = cpu.x87[physical_register(cpu, logical)];
				auto *slot = bytes.data() + 32 + logical * 16U;
				std::memcpy(slot, &value.significand,
					sizeof(value.significand));
				std::memcpy(slot + 8, &value.sign_exponent,
					sizeof(value.sign_exponent));
			}
		}
		if ((requested & sse_component) != 0) {
			std::memcpy(bytes.data() + 24, &cpu.mxcsr, sizeof(cpu.mxcsr));
			constexpr std::uint32_t mxcsr_mask = 0xffffU;
			std::memcpy(bytes.data() + 28, &mxcsr_mask,
				sizeof(mxcsr_mask));
			for (std::size_t index = 0; index < cpu.vectors.size(); ++index)
				std::copy_n(cpu.vectors[index].begin(), 16,
					bytes.begin() + 160 + index * 16U);
		}
		if (!legacy && (requested & avx_component) != 0) {
			for (std::size_t index = 0; index < cpu.vectors.size(); ++index)
				std::copy_n(cpu.vectors[index].begin() + 16, 16,
					bytes.begin() + legacy_size + xsave_header_size +
						index * 16U);
		}
		if (!write_bytes(memory, address, std::span{bytes}.first(size)))
			return write_failure(memory, address);
		memory.invalidate_code(address);
		advance(gadget, cpu);
		return {StepAction::return_to_dispatch,
			::uml::cpu::EngineEvent::continued, {}};
	}

	std::uint64_t present = supported_components;
	if (!legacy) {
		std::memcpy(&present, bytes.data() + legacy_size, sizeof(present));
		present &= supported_components;
	}
	if ((requested & x87_component) != 0) {
		if ((present & x87_component) == 0) {
			initialize(cpu);
			std::fill(cpu.mmx.begin(), cpu.mmx.end(), 0);
			std::fill(cpu.x87.begin(), cpu.x87.end(), X87Register{});
		} else {
			std::memcpy(&cpu.x87_control, bytes.data(),
				sizeof(cpu.x87_control));
			std::uint16_t status{};
			std::memcpy(&status, bytes.data() + 2, sizeof(status));
			load_status_word(cpu, status);
			const auto abridged_tag = std::to_integer<std::uint8_t>(bytes[4]);
			cpu.x87_tag = 0xffffU;
			for (std::uint8_t logical = 0; logical < 8; ++logical) {
				const auto physical = physical_register(cpu, logical);
				auto &value = cpu.x87[physical];
				const auto *slot = bytes.data() + 32 + logical * 16U;
				std::memcpy(&value.significand, slot,
					sizeof(value.significand));
				std::memcpy(&value.sign_exponent, slot + 8,
					sizeof(value.sign_exponent));
				cpu.mmx[physical] = value.significand;
				if ((abridged_tag & (1U << physical)) != 0)
					set_register_tag(cpu, physical, classify_tag(value));
			}
		}
	}
	if ((requested & sse_component) != 0) {
		if ((present & sse_component) == 0) {
			cpu.mxcsr = 0x1f80U;
			for (auto &vector : cpu.vectors)
				std::fill_n(vector.begin(), 16, std::byte{});
		} else {
			std::memcpy(&cpu.mxcsr, bytes.data() + 24, sizeof(cpu.mxcsr));
			for (std::size_t index = 0; index < cpu.vectors.size(); ++index)
				std::copy_n(bytes.begin() + 160 + index * 16U, 16,
					cpu.vectors[index].begin());
		}
	}
	if (!legacy && (requested & avx_component) != 0) {
		for (std::size_t index = 0; index < cpu.vectors.size(); ++index) {
			if ((present & avx_component) == 0)
				std::fill_n(cpu.vectors[index].begin() + 16, 16, std::byte{});
			else
				std::copy_n(bytes.begin() + legacy_size + xsave_header_size +
					index * 16U, 16, cpu.vectors[index].begin() + 16);
		}
	}
	advance(gadget, cpu);
	return {};
}

[[nodiscard]] StepResult execute_x87(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto opcode = instruction.vector_opcode;
	const auto modrm = instruction.vector_control;
	const auto group = static_cast<std::uint8_t>((modrm >> 3U) & 7U);
	const auto index = static_cast<std::uint8_t>(modrm & 7U);
	const bool register_form = (modrm >> 6U) == 3;

	if (opcode == 0x0e || opcode == 0x77) {
		cpu.x87_tag = 0xffff;
		cpu.x87_top = 0;
		advance(gadget, cpu);
		return {};
	}

	if (!register_form) {
		const auto address = effective_address(
			gadget, cpu, instruction.source.memory);
		if (opcode == 0xd9 && group == 5) {
			std::array<std::byte, 2> bytes{};
			if (!read_bytes(memory, address, bytes))
				return read_failure(memory, address);
			std::memcpy(&cpu.x87_control, bytes.data(), bytes.size());
			advance(gadget, cpu);
			return {};
		}
		if (opcode == 0xd9 && group == 7) {
			std::array<std::byte, 2> bytes{};
			std::memcpy(bytes.data(), &cpu.x87_control, bytes.size());
			return write_memory_result(gadget, cpu, memory, address, bytes);
		}
		if (opcode == 0xdd && group == 7) {
			std::array<std::byte, 2> bytes{};
			const auto status = status_word(cpu);
			std::memcpy(bytes.data(), &status, bytes.size());
			return write_memory_result(gadget, cpu, memory, address, bytes);
		}
		if ((opcode == 0xd9 && (group == 4 || group == 6)) ||
			(opcode == 0xdd && (group == 4 || group == 6))) {
			const bool full_state = opcode == 0xdd;
			const auto environment_size = instruction.vector_prefix != 0 ? 14U : 28U;
			const auto size = environment_size + (full_state ? 80U : 0U);
			std::array<std::byte, 108> bytes{};
			if (group == 4) {
				if (!read_bytes(memory, address,
					std::span{bytes}.first(size)))
					return read_failure(memory, address);
				std::memcpy(&cpu.x87_control, bytes.data(), 2);
				std::uint16_t status{};
				std::uint16_t tag{};
				const auto status_offset = environment_size == 28 ? 4U : 2U;
				const auto tag_offset = environment_size == 28 ? 8U : 4U;
				std::memcpy(&status, bytes.data() + status_offset, 2);
				std::memcpy(&tag, bytes.data() + tag_offset, 2);
				load_status_word(cpu, status);
				cpu.x87_tag = tag;
				if (full_state) {
					for (std::size_t slot = 0; slot < 8; ++slot) {
						std::memcpy(&cpu.x87[slot].significand,
							bytes.data() + environment_size + slot * 10U, 8);
						std::memcpy(&cpu.x87[slot].sign_exponent,
							bytes.data() + environment_size + slot * 10U + 8U, 2);
						cpu.mmx[slot] = cpu.x87[slot].significand;
					}
				}
				advance(gadget, cpu);
				return {};
			}
			std::memcpy(bytes.data(), &cpu.x87_control, 2);
			const auto status = status_word(cpu);
			const auto status_offset = environment_size == 28 ? 4U : 2U;
			const auto tag_offset = environment_size == 28 ? 8U : 4U;
			std::memcpy(bytes.data() + status_offset, &status, 2);
			std::memcpy(bytes.data() + tag_offset, &cpu.x87_tag, 2);
			if (full_state) {
				for (std::size_t slot = 0; slot < 8; ++slot) {
					std::memcpy(bytes.data() + environment_size + slot * 10U,
						&cpu.x87[slot].significand, 8);
					std::memcpy(bytes.data() + environment_size + slot * 10U + 8U,
						&cpu.x87[slot].sign_exponent, 2);
				}
			}
			const auto written = write_memory_result(
				gadget, cpu, memory, address, std::span{bytes}.first(size));
			if (written.action != StepAction::exit) {
				if (full_state)
					initialize(cpu);
				else
					cpu.x87_control |= 0x3fU;
			}
			return written;
		}

		const auto size = number_size(opcode, group);
		std::array<std::byte, 10> bytes{};
		const bool load = (opcode == 0xd9 && group == 0) ||
			(opcode == 0xdb && (group == 0 || group == 5)) ||
			(opcode == 0xdd && group == 0) ||
			(opcode == 0xdf && (group == 0 || group == 4 || group == 5));
		const bool store = (opcode == 0xd9 && (group == 2 || group == 3)) ||
			(opcode == 0xdb && (group == 1 || group == 2 || group == 3 || group == 7)) ||
			(opcode == 0xdd && (group == 1 || group == 2 || group == 3)) ||
			(opcode == 0xdf && (group == 1 || group == 2 || group == 3 || group == 6 || group == 7));
		if (load) {
			if (!read_bytes(memory, address, std::span{bytes}.first(size)))
				return read_failure(memory, address);
			X87Register value{};
			if (size == 10 && opcode == 0xdf && group == 4)
				value = packed_bcd_load(cpu, bytes);
			else if (size == 10) {
				std::memcpy(&value.significand, bytes.data(), 8);
				std::memcpy(&value.sign_exponent, bytes.data() + 8, 2);
			} else
				value = memory_number(cpu, opcode, bytes);
			push(cpu, value);
			advance(gadget, cpu);
			return {};
		}
		if (store) {
			const auto value = load_stack(cpu, 0);
			const bool truncate = group == 1;
			if ((opcode == 0xd9 || opcode == 0xdd) && group >= 2) {
				if (opcode == 0xd9) {
					const auto converted = to_float32(cpu, value);
					std::memcpy(bytes.data(), &converted, 4);
				} else {
					const auto converted = to_float64(cpu, value);
					std::memcpy(bytes.data(), &converted, 8);
				}
			} else if (opcode == 0xdb && group == 7) {
				std::memcpy(bytes.data(), &value.significand, 8);
				std::memcpy(bytes.data() + 8, &value.sign_exponent, 2);
			} else if (opcode == 0xdf && group == 6) {
				packed_bcd_store(cpu, value,
					std::span<std::byte, 10>{bytes.data(), 10});
			} else {
				auto state = soft_state(cpu);
				const auto rounding = truncate ? softfloat_round_minMag :
					state.roundingMode;
				if (size == 2) {
					auto converted = extF80_to_i32(&state, to_soft(value),
						rounding, false);
					if (converted < std::numeric_limits<std::int16_t>::min() ||
						converted > std::numeric_limits<std::int16_t>::max()) {
						state.exceptionFlags |= softfloat_flag_invalid;
						converted = std::numeric_limits<std::int16_t>::min();
					}
					const auto narrowed = static_cast<std::int16_t>(converted);
					std::memcpy(bytes.data(), &narrowed, 2);
				} else if (size == 4) {
					const auto converted = static_cast<std::int32_t>(
						extF80_to_i32(&state, to_soft(value), rounding, false));
					std::memcpy(bytes.data(), &converted, 4);
				} else {
					const auto converted = static_cast<std::int64_t>(
						extF80_to_i64(&state, to_soft(value), rounding, false));
					std::memcpy(bytes.data(), &converted, 8);
				}
				commit_exceptions(cpu, state);
			}
			const auto written = write_memory_result(
				gadget, cpu, memory, address, std::span{bytes}.first(size));
			if (written.action != StepAction::exit &&
				(group == 1 || group == 3 || group == 6 || group == 7))
				pop(cpu);
			return written;
		}

		if (!read_bytes(memory, address, std::span{bytes}.first(size)))
			return read_failure(memory, address);
		const auto right = memory_number(cpu, opcode, bytes);
		const auto left = load_stack(cpu, 0);
		if (group == 2 || group == 3) {
			set_x87_compare(cpu, compare(cpu, left, right));
			if (group == 3)
				pop(cpu);
		} else {
			const auto operation = group == 0 ? BinaryOperation::add :
				group == 1 ? BinaryOperation::multiply :
				(group == 4 || group == 5) ? BinaryOperation::subtract :
				BinaryOperation::divide;
			const bool reverse = group == 5 || group == 7;
			store_stack(cpu, 0, binary(cpu, operation,
				reverse ? right : left, reverse ? left : right));
		}
		advance(gadget, cpu);
		return {};
	}

	if (opcode == 0xd8 || opcode == 0xdc || opcode == 0xde) {
		if ((opcode == 0xd8 || opcode == 0xdc) && (group == 2 || group == 3)) {
			set_x87_compare(cpu, compare(cpu,
				load_stack(cpu, 0), load_stack(cpu, index)));
			if (group == 3)
				pop(cpu);
			advance(gadget, cpu);
			return {};
		}
		if (opcode == 0xde && group == 3) {
			if (index != 1)
				return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
			set_x87_compare(cpu, compare(cpu,
				load_stack(cpu, 0), load_stack(cpu, 1)));
			pop(cpu);
			pop(cpu);
			advance(gadget, cpu);
			return {};
		}
		if (opcode == 0xde && group == 2) {
			set_x87_compare(cpu, compare(cpu,
				load_stack(cpu, 0), load_stack(cpu, index)));
			pop(cpu);
			advance(gadget, cpu);
			return {};
		}
		const auto operation = group == 0 ? BinaryOperation::add :
			group == 1 ? BinaryOperation::multiply :
			(group == 4 || group == 5) ? BinaryOperation::subtract :
			BinaryOperation::divide;
		std::uint8_t destination{};
		X87Register left{};
		X87Register right{};
		bool do_pop = false;
		if (opcode == 0xd8) {
			destination = 0;
			left = load_stack(cpu, 0);
			right = load_stack(cpu, index);
			if (group == 5 || group == 7)
				std::swap(left, right);
		} else {
			destination = index;
			left = load_stack(cpu, index);
			right = load_stack(cpu, 0);
			if (group == 4 || group == 6)
				std::swap(left, right);
			do_pop = opcode == 0xde;
		}
		store_stack(cpu, destination, binary(cpu, operation, left, right));
		if (do_pop)
			pop(cpu);
		advance(gadget, cpu);
		return {};
	}

	if (opcode == 0xd9) {
		if (group == 0) {
			const auto value = load_stack(cpu, index);
			push(cpu, value);
		} else if (group == 1) {
			const auto first = physical_register(cpu, 0);
			const auto second = physical_register(cpu, index);
			std::swap(cpu.x87[first], cpu.x87[second]);
			std::swap(cpu.mmx[first], cpu.mmx[second]);
			const auto first_tag = register_tag(cpu, first);
			const auto second_tag = register_tag(cpu, second);
			set_register_tag(cpu, first, second_tag);
			set_register_tag(cpu, second, first_tag);
		} else if (modrm == 0xd0) {
			// FNOP
		} else if (modrm == 0xe0 || modrm == 0xe1) {
			auto value = load_stack(cpu, 0);
			if (modrm == 0xe0)
				value.sign_exponent ^= 0x8000U;
			else
				value.sign_exponent &= 0x7fffU;
			store_stack(cpu, 0, value);
		} else if (modrm == 0xe4) {
			set_x87_compare(cpu, compare(cpu, load_stack(cpu, 0), {}));
		} else if (modrm == 0xe5) {
			const auto physical = physical_register(cpu, 0);
			const auto tag = register_tag(cpu, physical);
			const auto value = cpu.x87[physical];
			cpu.x87_status &= ~(status_c0 | status_c1 | status_c2 | status_c3);
			if ((value.sign_exponent & 0x8000U) != 0)
				cpu.x87_status |= status_c1;
			if (tag == tag_empty)
				cpu.x87_status |= status_c0 | status_c3;
			else
				cpu.x87_status |= status_c2;
		} else if (modrm >= 0xe8 && modrm <= 0xee) {
			push(cpu, constant(static_cast<std::uint8_t>(modrm - 0xe8U)));
		} else if (modrm == 0xf0) {
			store_stack(cpu, 0, f2xm1(cpu, load_stack(cpu, 0)));
		} else if (modrm == 0xf1 || modrm == 0xf9) {
			store_stack(cpu, 1, fyl2x(cpu, load_stack(cpu, 0),
				load_stack(cpu, 1), modrm == 0xf9));
			pop(cpu);
		} else if (modrm == 0xf2) {
			store_stack(cpu, 0, transcendental(cpu,
				TranscendentalOperation::tangent, load_stack(cpu, 0)));
			push(cpu, constant(0));
		} else if (modrm == 0xf3) {
			store_stack(cpu, 1, fpatan(cpu, load_stack(cpu, 1),
				load_stack(cpu, 0)));
			pop(cpu);
		} else if (modrm == 0xf4) {
			const auto input = load_stack(cpu, 0);
			store_stack(cpu, 0, extract_exponent(input));
			push(cpu, extract_significand(input));
		} else if (modrm == 0xf5 || modrm == 0xf8) {
			store_stack(cpu, 0, remainder(cpu, load_stack(cpu, 0),
				load_stack(cpu, 1), modrm == 0xf5));
			cpu.x87_status &= ~(status_c0 | status_c1 | status_c2 | status_c3);
		} else if (modrm == 0xf6 || modrm == 0xf7) {
			cpu.x87_top = static_cast<std::uint8_t>((cpu.x87_top +
				(modrm == 0xf6 ? 7U : 1U)) & 7U);
			cpu.x87_status &= ~status_c1;
		} else if (modrm == 0xfa) {
			auto state = soft_state(cpu);
			const auto result = extF80_sqrt(&state,
				to_soft(load_stack(cpu, 0)));
			commit_exceptions(cpu, state);
			store_stack(cpu, 0, from_soft(result));
		} else if (modrm == 0xfb) {
			const auto input = load_stack(cpu, 0);
			store_stack(cpu, 0, transcendental(cpu,
				TranscendentalOperation::sine, input));
			push(cpu, transcendental(cpu,
				TranscendentalOperation::cosine, input));
		} else if (modrm == 0xfc) {
			auto state = soft_state(cpu);
			const auto value = extF80_roundToInt(&state,
				to_soft(load_stack(cpu, 0)), state.roundingMode, false);
			commit_exceptions(cpu, state);
			store_stack(cpu, 0, from_soft(value));
		} else if (modrm == 0xfd) {
			store_stack(cpu, 0, scale(cpu, load_stack(cpu, 0),
				load_stack(cpu, 1)));
		} else if (modrm == 0xfe || modrm == 0xff) {
			store_stack(cpu, 0, transcendental(cpu,
				modrm == 0xfe ? TranscendentalOperation::sine :
					TranscendentalOperation::cosine,
				load_stack(cpu, 0)));
		} else {
			return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
		}
		advance(gadget, cpu);
		return {};
	}

	if (opcode == 0xda || opcode == 0xdb) {
		if (group <= 3) {
			const auto condition = opcode == 0xda ? group :
				static_cast<std::uint8_t>(group + 4U);
			if (condition_holds(cpu, condition))
				store_stack(cpu, 0, load_stack(cpu, index));
		} else if (opcode == 0xda && modrm == 0xe9) {
			set_x87_compare(cpu, compare(cpu,
				load_stack(cpu, 0), load_stack(cpu, 1)));
			pop(cpu);
			pop(cpu);
		} else if (opcode == 0xdb && modrm == 0xe2) {
			cpu.x87_status &= ~(status_exception_mask | status_error_summary);
		} else if (opcode == 0xdb && modrm == 0xe3) {
			initialize(cpu);
		} else if (opcode == 0xdb && (group == 5 || group == 6)) {
			set_integer_compare(cpu, compare(cpu,
				load_stack(cpu, 0), load_stack(cpu, index)));
		} else if (opcode == 0xdb && group == 7) {
			set_integer_compare(cpu, compare(cpu,
				load_stack(cpu, 0), load_stack(cpu, index)));
		} else {
			return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
		}
		advance(gadget, cpu);
		return {};
	}

	if (opcode == 0xdd || opcode == 0xdf) {
		if (group == 0) {
			set_register_tag(cpu, physical_register(cpu, index), tag_empty);
			if (opcode == 0xdf)
				pop(cpu);
		} else if (group == 1) {
			const auto first = physical_register(cpu, 0);
			const auto second = physical_register(cpu, index);
			std::swap(cpu.x87[first], cpu.x87[second]);
			std::swap(cpu.mmx[first], cpu.mmx[second]);
			const auto first_tag = register_tag(cpu, first);
			const auto second_tag = register_tag(cpu, second);
			set_register_tag(cpu, first, second_tag);
			set_register_tag(cpu, second, first_tag);
		} else if (group == 2 || group == 3) {
			store_stack(cpu, index, load_stack(cpu, 0));
			if (group == 3 || opcode == 0xdf)
				pop(cpu);
		} else if (opcode == 0xdd && (group == 4 || group == 5)) {
			set_x87_compare(cpu, compare(cpu,
				load_stack(cpu, 0), load_stack(cpu, index)));
			if (group == 5)
				pop(cpu);
		} else if (opcode == 0xdf && modrm == 0xe0) {
			cpu.gpr(Register::rax) = (cpu.gpr(Register::rax) & ~0xffffULL) |
				status_word(cpu);
		} else if (opcode == 0xdf && (group == 5 || group == 6)) {
			set_integer_compare(cpu, compare(cpu,
				load_stack(cpu, 0), load_stack(cpu, index)));
			pop(cpu);
		} else {
			return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
		}
		advance(gadget, cpu);
		return {};
	}

	return {StepAction::exit, ::uml::cpu::EngineEvent::fatal, {}};
}

} // namespace uml::cpu::x86_64::gadgets
