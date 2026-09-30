// SPDX-License-Identifier: GPL-2.0-or-later
#include "x87_math.hpp"

#include <SoftFloat-3e/platform.h>
#include <cephes_128bit.h>

extern "C" {
#include <SoftFloat-3e/softfloat.h>
}

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace uml::cpu::x86_64::gadgets {
namespace {

constexpr std::uint16_t status_invalid = 1U << 0U;
constexpr std::uint16_t status_zero_divide = 1U << 2U;
constexpr std::uint16_t status_overflow = 1U << 3U;
constexpr std::uint16_t status_underflow = 1U << 4U;
constexpr std::uint16_t status_precision = 1U << 5U;
constexpr std::uint16_t status_error_summary = 1U << 7U;
constexpr std::uint16_t status_exception_mask = 0x7fU;
/* The six exceptions the control word masks. */
constexpr std::uint16_t status_maskable_exceptions = 0x3fU;

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
	std::uint16_t raised{};
	if ((state.exceptionFlags & softfloat_flag_invalid) != 0)
		raised |= status_invalid;
	if ((state.exceptionFlags & softfloat_flag_infinite) != 0)
		raised |= status_zero_divide;
	if ((state.exceptionFlags & softfloat_flag_overflow) != 0)
		raised |= status_overflow;
	if ((state.exceptionFlags & softfloat_flag_underflow) != 0)
		raised |= status_underflow;
	if ((state.exceptionFlags & softfloat_flag_inexact) != 0)
		raised |= status_precision;
	/* While the gadgets check x87 exceptions, one the guest unmasked
	 * leaves the status word alone: the gadget leaves before it commits
	 * the result, and the runtime runs the instruction again, checked. */
	if ((cpu.exception_checks & exception_check_x87) != 0 &&
		(raised & ~cpu.x87_control & status_maskable_exceptions) != 0) {
		cpu.exception_raised = 1;
		return;
	}
	cpu.x87_status |= raised;
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
	/* FSIN and FPTAN return a zero operand unchanged, sign included; the
	 * 128-bit sine does not keep a negative zero's sign. */
	if (is_zero(value) && operation != TranscendentalOperation::cosine)
		return value;
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
			/* An exact remainder of zero takes the dividend's sign;
			 * the subtraction alone would give +0. */
			if (is_zero(from_soft(result)))
				result.signExp = static_cast<std::uint16_t>(
					dividend.sign_exponent & 0x8000U);
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

extern "C" void uml_x86_64_x87_from_f32(CpuState *cpu,
	std::uint32_t bits, X87Register *result) noexcept
{
	*result = from_float32(*cpu, bits);
}

extern "C" void uml_x86_64_x87_from_f64(CpuState *cpu,
	std::uint64_t bits, X87Register *result) noexcept
{
	*result = from_float64(*cpu, bits);
}

extern "C" void uml_x86_64_x87_from_i32(
	std::int32_t value, X87Register *result) noexcept
{
	*result = from_i32(value);
}

extern "C" void uml_x86_64_x87_from_i64(
	std::int64_t value, X87Register *result) noexcept
{
	*result = from_i64(value);
}

extern "C" void uml_x86_64_x87_from_bcd(CpuState *cpu,
	const void *source, X87Register *result) noexcept
{
	*result = packed_bcd_load(*cpu, {
		static_cast<const std::byte *>(source), 10});
}

extern "C" std::uint32_t uml_x86_64_x87_to_f32(CpuState *cpu,
	const X87Register *value) noexcept
{
	return to_float32(*cpu, *value);
}

extern "C" std::uint64_t uml_x86_64_x87_to_f64(CpuState *cpu,
	const X87Register *value) noexcept
{
	return to_float64(*cpu, *value);
}

namespace {

[[nodiscard]] softfloat_state integer_store_state(
	const CpuState &cpu, bool truncate) noexcept
{
	auto state = soft_state(cpu);
	if (truncate)
		state.roundingMode = softfloat_round_minMag;
	return state;
}

} // namespace

extern "C" std::uint32_t uml_x86_64_x87_to_i16(CpuState *cpu,
	const X87Register *value, std::uint32_t truncate) noexcept
{
	auto state = integer_store_state(*cpu, truncate != 0);
	auto converted = extF80_to_i32(&state, to_soft(*value),
		state.roundingMode, false);
	if (converted < std::numeric_limits<std::int16_t>::min() ||
		converted > std::numeric_limits<std::int16_t>::max()) {
		state.exceptionFlags |= softfloat_flag_invalid;
		converted = std::numeric_limits<std::int16_t>::min();
	}
	commit_exceptions(*cpu, state);
	return static_cast<std::uint16_t>(static_cast<std::int16_t>(converted));
}

extern "C" std::uint32_t uml_x86_64_x87_to_i32(CpuState *cpu,
	const X87Register *value, std::uint32_t truncate) noexcept
{
	auto state = integer_store_state(*cpu, truncate != 0);
	const auto converted = extF80_to_i32(&state, to_soft(*value),
		state.roundingMode, false);
	commit_exceptions(*cpu, state);
	return static_cast<std::uint32_t>(converted);
}

extern "C" std::uint64_t uml_x86_64_x87_to_i64(CpuState *cpu,
	const X87Register *value, std::uint32_t truncate) noexcept
{
	auto state = integer_store_state(*cpu, truncate != 0);
	const auto converted = extF80_to_i64(&state, to_soft(*value),
		state.roundingMode, false);
	commit_exceptions(*cpu, state);
	return static_cast<std::uint64_t>(converted);
}

extern "C" void uml_x86_64_x87_to_bcd(CpuState *cpu,
	const X87Register *value, void *destination) noexcept
{
	packed_bcd_store(*cpu, *value, std::span<std::byte, 10>{
		static_cast<std::byte *>(destination), 10});
}

#define UML_X86_64_DEFINE_X87_BINARY(name, operation) \
	extern "C" void uml_x86_64_x87_##name(CpuState *cpu, \
		const X87Register *left, const X87Register *right, \
		X87Register *result) noexcept \
	{ \
		*result = binary(*cpu, BinaryOperation::operation, *left, *right); \
	}
UML_X86_64_DEFINE_X87_BINARY(add, add)
UML_X86_64_DEFINE_X87_BINARY(multiply, multiply)
UML_X86_64_DEFINE_X87_BINARY(subtract, subtract)
UML_X86_64_DEFINE_X87_BINARY(divide, divide)
#undef UML_X86_64_DEFINE_X87_BINARY

extern "C" std::uint32_t uml_x86_64_x87_compare(CpuState *cpu,
	const X87Register *left, const X87Register *right) noexcept
{
	return static_cast<std::uint32_t>(compare(*cpu, *left, *right));
}

extern "C" void uml_x86_64_x87_f2xm1(CpuState *cpu,
	const X87Register *input, X87Register *result) noexcept
{
	*result = f2xm1(*cpu, *input);
}

extern "C" void uml_x86_64_x87_fyl2x(CpuState *cpu,
	const X87Register *value, const X87Register *multiplier,
	X87Register *result) noexcept
{
	*result = fyl2x(*cpu, *value, *multiplier, false);
}

extern "C" void uml_x86_64_x87_fyl2xp1(CpuState *cpu,
	const X87Register *value, const X87Register *multiplier,
	X87Register *result) noexcept
{
	*result = fyl2x(*cpu, *value, *multiplier, true);
}

extern "C" void uml_x86_64_x87_tangent(CpuState *cpu,
	const X87Register *input, X87Register *result) noexcept
{
	*result = transcendental(*cpu, TranscendentalOperation::tangent, *input);
}

extern "C" void uml_x86_64_x87_atan(CpuState *cpu,
	const X87Register *y, const X87Register *x,
	X87Register *result) noexcept
{
	*result = fpatan(*cpu, *y, *x);
}

extern "C" void uml_x86_64_x87_extract(const X87Register *input,
	X87Register *exponent_result, X87Register *significand_result) noexcept
{
	*exponent_result = extract_exponent(*input);
	*significand_result = extract_significand(*input);
}

extern "C" void uml_x86_64_x87_fprem(CpuState *cpu,
	const X87Register *dividend, const X87Register *divisor,
	X87Register *result) noexcept
{
	*result = remainder(*cpu, *dividend, *divisor, false);
}

extern "C" void uml_x86_64_x87_fprem1(CpuState *cpu,
	const X87Register *dividend, const X87Register *divisor,
	X87Register *result) noexcept
{
	*result = remainder(*cpu, *dividend, *divisor, true);
}

extern "C" void uml_x86_64_x87_sqrt(CpuState *cpu,
	const X87Register *input, X87Register *result) noexcept
{
	auto state = soft_state(*cpu);
	*result = from_soft(extF80_sqrt(&state, to_soft(*input)));
	commit_exceptions(*cpu, state);
}

extern "C" void uml_x86_64_x87_sincos(CpuState *cpu,
	const X87Register *input, X87Register *sine_result,
	X87Register *cosine_result) noexcept
{
	*sine_result = transcendental(
		*cpu, TranscendentalOperation::sine, *input);
	*cosine_result = transcendental(
		*cpu, TranscendentalOperation::cosine, *input);
}

extern "C" void uml_x86_64_x87_round(CpuState *cpu,
	const X87Register *input, X87Register *result) noexcept
{
	auto state = soft_state(*cpu);
	*result = from_soft(extF80_roundToInt(&state, to_soft(*input),
		state.roundingMode, false));
	commit_exceptions(*cpu, state);
}

extern "C" void uml_x86_64_x87_scale(CpuState *cpu,
	const X87Register *value, const X87Register *exponent,
	X87Register *result) noexcept
{
	*result = scale(*cpu, *value, *exponent);
}

extern "C" void uml_x86_64_x87_sine(CpuState *cpu,
	const X87Register *input, X87Register *result) noexcept
{
	*result = transcendental(*cpu, TranscendentalOperation::sine, *input);
}

extern "C" void uml_x86_64_x87_cosine(CpuState *cpu,
	const X87Register *input, X87Register *result) noexcept
{
	*result = transcendental(*cpu, TranscendentalOperation::cosine, *input);
}

} // namespace uml::cpu::x86_64::gadgets
