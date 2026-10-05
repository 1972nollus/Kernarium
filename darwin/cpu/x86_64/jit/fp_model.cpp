// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/x86_64/jit/fp_model.hpp"

#include <SoftFloat-3e/platform.h>

extern "C" {
#include <SoftFloat-3e/softfloat.h>
}

#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace uml::cpu::x86_64::jit {
namespace {

constexpr std::uint32_t invalid = 0x01U;
constexpr std::uint32_t denormal = 0x02U;
constexpr std::uint32_t divide = 0x04U;
constexpr std::uint32_t overflow = 0x08U;
constexpr std::uint32_t underflow = 0x10U;
constexpr std::uint32_t precision = 0x20U;
constexpr std::uint32_t mask_shift = 7U;
constexpr std::uint32_t mxcsr_denormals_are_zero = 0x40U;
constexpr std::uint32_t mxcsr_flush_to_zero = 0x8000U;

/* An IEEE binary32 or binary64 lane's classes. */
struct Format final {
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
	[[nodiscard]] std::uint64_t quiet_bit() const noexcept
	{
		return dbl ? 1ULL << 51U : 1ULL << 22U;
	}
	[[nodiscard]] std::uint64_t one() const noexcept
	{
		return dbl ? 0x3ff0'0000'0000'0000ULL : 0x3f80'0000ULL;
	}
	/* x86's default NaN, the indefinite. */
	[[nodiscard]] std::uint64_t indefinite() const noexcept
	{
		return sign() | exponent() | quiet_bit();
	}
	[[nodiscard]] bool nan(std::uint64_t x) const noexcept
	{
		return (x & magnitude()) > exponent();
	}
	[[nodiscard]] bool signalling(std::uint64_t x) const noexcept
	{
		return nan(x) && (x & quiet_bit()) == 0;
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
	[[nodiscard]] std::uint64_t quiet(std::uint64_t x) const noexcept
	{
		return x | quiet_bit();
	}
	/* `x` with a denormal taken as a zero of its sign. */
	[[nodiscard]] std::uint64_t flush(std::uint64_t x) const noexcept
	{
		return denormal(x) ? x & sign() : x;
	}
	/* Ordered a < b, for numbers that are not both zeros. */
	[[nodiscard]] bool less(std::uint64_t a, std::uint64_t b) const noexcept
	{
		const auto key = [this](std::uint64_t x) {
			const auto value = static_cast<std::int64_t>(x & magnitude());
			return negative(x) ? -value : value;
		};
		return key(a) < key(b);
	}
};

[[nodiscard]] std::uint64_t lane(const FpVector &vector, bool dbl,
	std::size_t index) noexcept
{
	if (dbl) {
		std::uint64_t value{};
		std::memcpy(&value, vector.data() + index * 8U, 8U);
		return value;
	}
	std::uint32_t value{};
	std::memcpy(&value, vector.data() + index * 4U, 4U);
	return value;
}

void set_lane(FpVector &vector, bool dbl, std::size_t index,
	std::uint64_t value) noexcept
{
	if (dbl) {
		std::memcpy(vector.data() + index * 8U, &value, 8U);
		return;
	}
	const auto narrow = static_cast<std::uint32_t>(value);
	std::memcpy(vector.data() + index * 4U, &narrow, 4U);
}

/* SoftFloat's rounding mode for MXCSR.RC. */
[[nodiscard]] std::uint8_t soft_rounding(std::uint32_t mxcsr) noexcept
{
	switch ((mxcsr >> 13U) & 3U) {
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
		return f32_to_f128(&state, float32_t{static_cast<std::uint32_t>(x)});
	return extF80_to_f128(&state, f64_to_extF80(&state, float64_t{x}));
}

[[nodiscard]] bool soft_zero(float128_t x) noexcept
{
	return x.v[0] == 0 && (x.v[1] << 1U) == 0;
}

/* `operation` rounded to odd: truncated, with its lowest bit set when that
 * lost anything. A single or double keeps at most 53 of binary128's 113
 * bits, so rounding it again rounds as the exact result would. An exact
 * zero takes its sign from the rounding mode, `mode`. SoftFloat's flags
 * for the operation land in `flags`. */
template <typename Operation>
[[nodiscard]] float128_t soft_odd(Operation operation, std::uint8_t mode,
	std::uint8_t &flags) noexcept
{
	softfloat_state state{};
	state.roundingMode = softfloat_round_minMag;
	auto result = operation(state);
	flags = state.exceptionFlags;
	if ((flags & softfloat_flag_inexact) != 0) {
		result.v[0] |= 1U;
	} else if (soft_zero(result)) {
		softfloat_state exact{};
		exact.roundingMode = mode;
		result = operation(exact);
	}
	return result;
}

/* A result rounded as x86 does: whether it is tiny after rounding, whether
 * it is inexact and whether it overflowed, and the result; and whether it
 * is inexact rounded to full precision as if the exponent were unbounded,
 * which is what an unmasked underflow or overflow reports as precision. */
struct Rounded final {
	std::uint64_t bits{};
	bool tiny{};
	bool inexact{};
	bool overflow{};
	bool significand_inexact{};
};

[[nodiscard]] Rounded soft_round(float128_t value, bool dbl,
	std::uint32_t mxcsr) noexcept
{
	softfloat_state state{};
	state.detectTininess = softfloat_tininess_afterRounding;
	state.roundingMode = soft_rounding(mxcsr);
	Rounded result{};
	result.bits = dbl ? f128_to_f64(&state, value).v :
		f128_to_f32(&state, value).v;
	/* SoftFloat raises underflow for a tiny inexact result; an exact one
	 * is a denormal. */
	result.tiny = (state.exceptionFlags & softfloat_flag_underflow) != 0 ||
		Format{dbl}.denormal(result.bits);
	result.inexact = (state.exceptionFlags & softfloat_flag_inexact) != 0;
	result.overflow = (state.exceptionFlags & softfloat_flag_overflow) != 0;
	/* Of 112 fraction bits, a double keeps 52 and drops 60, and a single
	 * keeps 23 and drops 89. */
	result.significand_inexact = dbl ?
		(value.v[0] & ((1ULL << 60U) - 1U)) != 0 :
		value.v[0] != 0 || (value.v[1] & ((1ULL << 25U) - 1U)) != 0;
	return result;
}

/* One lane's result and the flags it raises before and after computing. */
struct Lane final {
	std::uint64_t bits{};
	std::uint32_t before{};
	std::uint32_t after{};
};

/* A rounded result's lane. A tiny result raises underflow when it is
 * inexact, or regardless while MXCSR unmasks underflow. While it masks
 * underflow, FTZ flushes a tiny result to zero, which raises underflow and
 * precision even when it was exact. Beside an unmasked underflow or
 * overflow, which faults, precision follows the significand alone. */
[[nodiscard]] Lane finish(const Format &f, const Rounded &rounded,
	std::uint32_t mxcsr, std::uint32_t before) noexcept
{
	const bool underflow_unmasked = (mxcsr & (underflow << mask_shift)) == 0;
	const bool overflow_unmasked = (mxcsr & (overflow << mask_shift)) == 0;
	Lane result{rounded.bits, before, 0};
	bool inexact = rounded.inexact;
	if (rounded.tiny && (mxcsr & mxcsr_flush_to_zero) != 0 &&
		!underflow_unmasked) {
		result.bits &= f.sign();
		result.after |= underflow;
		inexact = true;
	} else if (rounded.tiny && underflow_unmasked) {
		result.after |= underflow;
		inexact = rounded.significand_inexact;
	} else if (rounded.tiny && rounded.inexact) {
		result.after |= underflow;
	} else if (rounded.overflow) {
		result.after |= overflow;
		if (overflow_unmasked)
			inexact = rounded.significand_inexact;
	}
	if (inexact)
		result.after |= precision;
	return result;
}

/* ADD, SUB, MUL and DIV. A NaN result is the first source's when it is a
 * NaN, quieted, and else the second's; an invalid operation gives the
 * indefinite. Invalid operation and division by zero take precedence over a
 * denormal operand. */
[[nodiscard]] Lane binary(const Format &f, FpKind kind, std::uint64_t a,
	std::uint64_t b, std::uint32_t mxcsr) noexcept
{
	const bool daz = (mxcsr & mxcsr_denormals_are_zero) != 0;
	if (daz) {
		a = f.flush(a);
		b = f.flush(b);
	}
	Lane result{};
	if (f.signalling(a) || f.signalling(b))
		result.before |= invalid;
	if (f.nan(a) || f.nan(b)) {
		result.bits = f.nan(a) ? f.quiet(a) : f.quiet(b);
		return result;
	}
	const auto x = soft_wide(f.dbl, a);
	const auto y = soft_wide(f.dbl, b);
	std::uint8_t flags{};
	const auto value = soft_odd([&](softfloat_state &state) {
		switch (kind) {
		case FpKind::add: return f128_add(&state, x, y);
		case FpKind::sub: return f128_sub(&state, x, y);
		case FpKind::mul: return f128_mul(&state, x, y);
		default: return f128_div(&state, x, y);
		}
	}, soft_rounding(mxcsr), flags);
	if ((flags & softfloat_flag_invalid) != 0) {
		result.bits = f.indefinite();
		result.before |= invalid;
		return result;
	}
	if ((flags & softfloat_flag_infinite) != 0) {
		result.bits = ((a ^ b) & f.sign()) | f.exponent();
		result.before |= divide;
		return result;
	}
	if (!daz && (f.denormal(a) || f.denormal(b)))
		result.before |= denormal;
	return finish(f, soft_round(value, f.dbl, mxcsr), mxcsr, result.before);
}

/* SQRT: the square root of a negative number but -0 is invalid. */
[[nodiscard]] Lane square_root(const Format &f, std::uint64_t b,
	std::uint32_t mxcsr) noexcept
{
	const bool daz = (mxcsr & mxcsr_denormals_are_zero) != 0;
	if (daz)
		b = f.flush(b);
	Lane result{};
	if (f.nan(b)) {
		result.bits = f.quiet(b);
		result.before = f.signalling(b) ? invalid : 0U;
		return result;
	}
	if (f.negative(b) && !f.zero(b)) {
		result.bits = f.indefinite();
		result.before = invalid;
		return result;
	}
	if (!daz && f.denormal(b))
		result.before |= denormal;
	const auto x = soft_wide(f.dbl, b);
	std::uint8_t flags{};
	const auto value = soft_odd([&](softfloat_state &state) {
		return f128_sqrt(&state, x);
	}, soft_rounding(mxcsr), flags);
	return finish(f, soft_round(value, f.dbl, mxcsr), mxcsr, result.before);
}

/* MIN and MAX return the second source for a NaN, quiet or not, which is
 * invalid, and for two zeros. They round nothing, so FTZ leaves a denormal
 * result. */
[[nodiscard]] Lane minimum_maximum(const Format &f, bool maximum,
	std::uint64_t a, std::uint64_t b, std::uint32_t mxcsr) noexcept
{
	const bool daz = (mxcsr & mxcsr_denormals_are_zero) != 0;
	if (daz) {
		a = f.flush(a);
		b = f.flush(b);
	}
	Lane result{b, 0, 0};
	if (f.nan(a) || f.nan(b)) {
		result.before = invalid;
		return result;
	}
	if (!daz && (f.denormal(a) || f.denormal(b)))
		result.before = denormal;
	if (f.zero(a) && f.zero(b))
		return result;
	if (maximum ? f.less(b, a) : f.less(a, b))
		result.bits = a;
	return result;
}

/* An FMA lane, a * b + c with the product and the addend negated as asked.
 * A NaN result is the first of a, b and c that is a NaN, quieted and with
 * its own sign. Invalid operation is a signalling NaN, infinity times zero
 * plus anything but a quiet NaN, or infinities of opposite signs added. */
[[nodiscard]] Lane fused(const Format &f, std::uint64_t a, std::uint64_t b,
	std::uint64_t c, bool negate_product, bool negate_addend,
	std::uint32_t mxcsr) noexcept
{
	const bool daz = (mxcsr & mxcsr_denormals_are_zero) != 0;
	if (daz) {
		a = f.flush(a);
		b = f.flush(b);
		c = f.flush(c);
	}
	const bool product_negative = f.negative(a ^ b) != negate_product;
	const bool addend_negative = f.negative(c) != negate_addend;
	bool invalid_operation = false;
	if (f.signalling(a) || f.signalling(b) || f.signalling(c))
		invalid_operation = true;
	else if ((f.zero(a) && f.infinite(b)) || (f.infinite(a) && f.zero(b)))
		invalid_operation = !f.nan(c);
	else if (!f.nan(a) && !f.nan(b) && !f.nan(c))
		invalid_operation = (f.infinite(a) || f.infinite(b)) &&
			f.infinite(c) && product_negative != addend_negative;
	Lane result{};
	if (invalid_operation)
		result.before = invalid;
	for (const auto value : {a, b, c}) {
		if (f.nan(value)) {
			result.bits = f.quiet(value);
			return result;
		}
	}
	if (invalid_operation) {
		result.bits = f.indefinite();
		return result;
	}
	if (!daz && (f.denormal(a) || f.denormal(b) || f.denormal(c)))
		result.before |= denormal;
	const auto x = soft_wide(f.dbl, negate_product ? a ^ f.sign() : a);
	const auto y = soft_wide(f.dbl, b);
	const auto z = soft_wide(f.dbl, negate_addend ? c ^ f.sign() : c);
	/* Exact: a double's product has 106 bits. */
	softfloat_state exact{};
	const auto product = f128_mul(&exact, x, y);
	std::uint8_t flags{};
	const auto value = soft_odd([&](softfloat_state &state) {
		return f128_add(&state, product, z);
	}, soft_rounding(mxcsr), flags);
	return finish(f, soft_round(value, f.dbl, mxcsr), mxcsr, result.before);
}

/* CVTSD2SS and CVTPD2PS. A NaN keeps its sign and the top of its payload,
 * quieted. */
[[nodiscard]] Lane narrow(std::uint64_t x, std::uint32_t mxcsr) noexcept
{
	const Format wide{true};
	const Format single{false};
	const bool daz = (mxcsr & mxcsr_denormals_are_zero) != 0;
	if (daz)
		x = wide.flush(x);
	Lane result{};
	if (wide.nan(x)) {
		result.bits = ((x >> 32U) & 0x8000'0000U) | 0x7fc0'0000U |
			((x >> 29U) & 0x003f'ffffU);
		result.before = wide.signalling(x) ? invalid : 0U;
		return result;
	}
	if (!daz && wide.denormal(x))
		result.before = denormal;
	return finish(single, soft_round(soft_wide(true, x), false, mxcsr), mxcsr,
		result.before);
}

/* RCPPS, RCPSS, RSQRTPS and RSQRTSS, which ignore MXCSR but its rounding and
 * raise nothing: a denormal input is a zero, a reciprocal of 2^126 or more is
 * a zero of the input's sign, and the reciprocal square root of a negative
 * number is the indefinite. Elsewhere the correctly rounded quotient, of the
 * rounded square root for RSQRT, lies within x86's error, and is what the
 * host computes for the lanes that need nothing more. */
[[nodiscard]] std::uint64_t estimate(bool reciprocal, std::uint64_t x,
	std::uint32_t mxcsr) noexcept
{
	const Format f{false};
	x = f.flush(x);
	if (f.nan(x))
		return f.quiet(x);
	const auto magnitude = x & f.magnitude();
	const auto one = soft_wide(false, f.one());
	const auto mode = soft_rounding(mxcsr);
	std::uint8_t flags{};
	if (reciprocal) {
		if (magnitude >= 0x7e80'0000U && magnitude <= 0x7f80'0000U)
			return x & f.sign();
		const auto wide = soft_wide(false, x);
		return soft_round(soft_odd([&](softfloat_state &state) {
			return f128_div(&state, one, wide);
		}, mode, flags), false, mxcsr).bits;
	}
	if (f.negative(x) && magnitude != 0)
		return f.indefinite();
	const auto wide = soft_wide(false, x);
	const auto root = soft_round(soft_odd([&](softfloat_state &state) {
		return f128_sqrt(&state, wide);
	}, mode, flags), false, mxcsr).bits;
	const auto wide_root = soft_wide(false, root);
	return soft_round(soft_odd([&](softfloat_state &state) {
		return f128_div(&state, one, wide_root);
	}, mode, flags), false, mxcsr).bits;
}

/* DPPS or DPPD on one 128-bit part: the selected products, zero
 * elsewhere, then their sums, with x86's rules at every step. Each result
 * lane sums in its own order, which picks among NaNs as x86 hardware does:
 * for DPPS lane i adds p[i ^ 1] + p[i] and then that sum and the other
 * pair's, for DPPD p[i] + p[i ^ 1]. The steps raise their flags in turn:
 * the products, the sums of pairs, and for DPPS their sum. An unselected
 * product is zero and raises nothing. */
[[nodiscard]] FpSteps dot_product(const Format &f, std::uint8_t control,
	const FpValues &values, FpVector &result, std::uint32_t mxcsr) noexcept
{
	FpSteps steps{};
	const std::size_t count = f.dbl ? 2U : 4U;
	std::array<std::uint64_t, 4> products{};
	for (std::size_t index = 0; index < count; ++index) {
		if (((control >> (4U + index)) & 1U) == 0)
			continue;
		const auto product = binary(f, FpKind::mul,
			lane(values.sources[0], f.dbl, index),
			lane(values.sources[1], f.dbl, index), mxcsr);
		products[index] = product.bits;
		steps.before[0] |= product.before;
		steps.after[0] |= product.after;
	}
	const auto add = [&](std::uint64_t a, std::uint64_t b, std::size_t step) {
		const auto sum = binary(f, FpKind::add, a, b, mxcsr);
		steps.before[step] |= sum.before;
		steps.after[step] |= sum.after;
		return sum.bits;
	};
	std::array<std::uint64_t, 4> sums{};
	if (f.dbl) {
		for (std::size_t index = 0; index < 2; ++index)
			sums[index] = add(products[index], products[index ^ 1U], 1);
	} else {
		std::array<std::uint64_t, 4> pairs{};
		for (std::size_t index = 0; index < 4; ++index)
			pairs[index] = add(products[index ^ 1U], products[index], 1);
		for (std::size_t index = 0; index < 4; ++index)
			sums[index] = add(pairs[index], pairs[index ^ 2U], 2);
	}
	for (std::size_t index = 0; index < count; ++index)
		set_lane(result, f.dbl, index,
			((control >> index) & 1U) != 0 ? sums[index] : 0U);
	return steps;
}

} // namespace

FpSteps &FpSteps::operator|=(const FpSteps &other) noexcept
{
	for (std::size_t step = 0; step < count; ++step) {
		before[step] |= other.before[step];
		after[step] |= other.after[step];
	}
	return *this;
}

FpSteps x86_fp_compute(const FpOperation &operation, std::uint32_t mxcsr,
	FpValues &values) noexcept
{
	const Format f{operation.element_bytes == 8};
	/* A conversion narrows up to four doubles from two sources. */
	const auto lanes = std::min<std::size_t>(operation.lanes,
		operation.kind == FpKind::narrow ? 4U : 16U / operation.element_bytes);
	const auto &first = values.sources[0];
	const auto &second = values.sources[1];
	FpSteps steps{};
	const auto record = [&](std::size_t index, const Lane &result) {
		set_lane(values.result, f.dbl, index, result.bits);
		steps.before[0] |= result.before;
		steps.after[0] |= result.after;
	};
	switch (operation.kind) {
	case FpKind::add:
	case FpKind::sub:
	case FpKind::mul:
	case FpKind::div:
		for (std::size_t index = 0; index < lanes; ++index)
			record(index, binary(f, operation.kind, lane(first, f.dbl, index),
				lane(second, f.dbl, index), mxcsr));
		break;
	case FpKind::min:
	case FpKind::max:
		for (std::size_t index = 0; index < lanes; ++index)
			record(index, minimum_maximum(f, operation.kind == FpKind::max,
				lane(first, f.dbl, index), lane(second, f.dbl, index),
				mxcsr));
		break;
	case FpKind::sqrt:
		for (std::size_t index = 0; index < lanes; ++index)
			record(index, square_root(f, lane(first, f.dbl, index), mxcsr));
		break;
	case FpKind::rcp:
	case FpKind::rsqrt:
		for (std::size_t index = 0; index < lanes; ++index)
			set_lane(values.result, f.dbl, index,
				estimate(operation.kind == FpKind::rcp,
					lane(first, f.dbl, index), mxcsr));
		break;
	case FpKind::hadd:
	case FpKind::hsub: {
		/* Pairs of the first source, then of the second, the lower
		 * element first. */
		const auto kind = operation.kind == FpKind::hadd ? FpKind::add :
			FpKind::sub;
		const std::size_t half = lanes / 2U;
		for (std::size_t index = 0; index < lanes; ++index) {
			const auto &source = index < half ? first : second;
			const std::size_t pair = 2U * (index % half);
			record(index, binary(f, kind, lane(source, f.dbl, pair),
				lane(source, f.dbl, pair + 1U), mxcsr));
		}
		break;
	}
	case FpKind::addsub:
		for (std::size_t index = 0; index < lanes; ++index)
			record(index, binary(f, (index & 1U) == 0 ? FpKind::sub :
				FpKind::add, lane(first, f.dbl, index),
				lane(second, f.dbl, index), mxcsr));
		break;
	case FpKind::dpp:
		steps = dot_product(f, operation.immediate, values, values.result,
			mxcsr);
		break;
	case FpKind::narrow:
		for (std::size_t index = 0; index < lanes; ++index) {
			const auto result = narrow(lane(index < 2 ? first : second, true,
				index % 2U), mxcsr);
			set_lane(values.result, false, index, result.bits);
			steps.before[0] |= result.before;
			steps.after[0] |= result.after;
		}
		break;
	case FpKind::fma: {
		const bool negate_product =
			(operation.flags & fp_fma_negate_product) != 0;
		const unsigned addends = (operation.flags >> 1U) & 3U;
		for (std::size_t index = 0; index < lanes; ++index) {
			const bool negate_addend = addends == 1U ||
				(addends == 2U && (index & 1U) == 0) ||
				(addends == 3U && (index & 1U) != 0);
			record(index, fused(f, lane(first, f.dbl, index),
				lane(second, f.dbl, index),
				lane(values.sources[2], f.dbl, index), negate_product,
				negate_addend, mxcsr));
		}
		break;
	}
	}
	return steps;
}

FpOutcome x86_fp_settle(const FpSteps &steps, std::uint32_t mxcsr) noexcept
{
	const std::uint32_t unmasked = ~(mxcsr >> mask_shift) & 0x3fU;
	FpOutcome outcome{};
	for (std::size_t step = 0; step < FpSteps::count; ++step) {
		outcome.flags |= steps.before[step];
		if ((steps.before[step] & unmasked) != 0) {
			outcome.fault = true;
			return outcome;
		}
		outcome.flags |= steps.after[step];
		if ((steps.after[step] & unmasked) != 0) {
			outcome.fault = true;
			return outcome;
		}
	}
	return outcome;
}

} // namespace uml::cpu::x86_64::jit
