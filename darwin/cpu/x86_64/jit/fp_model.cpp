// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/x86_64/jit/fp_model.hpp"

#include <SoftFloat-3e/platform.h>

extern "C" {
#include <SoftFloat-3e/softfloat.h>
}

#include <algorithm>
#include <cstring>

namespace uml::cpu::x86_64::jit {
namespace {

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

/* `operation` rounded to odd: truncated, with its lowest bit set when that
 * lost anything. A single or double keeps at most 53 of binary128's 113
 * bits, so rounding it again rounds as the exact result would. */
template <typename Operation>
[[nodiscard]] float128_t soft_odd(Operation operation) noexcept
{
	softfloat_state state{};
	state.roundingMode = softfloat_round_minMag;
	auto result = operation(state);
	if ((state.exceptionFlags & softfloat_flag_inexact) != 0)
		result.v[0] |= 1U;
	return result;
}

/* `value` rounded to a single or a double as MXCSR.RC says. */
[[nodiscard]] std::uint64_t soft_round(float128_t value, bool dbl,
	std::uint32_t mxcsr) noexcept
{
	softfloat_state state{};
	state.detectTininess = softfloat_tininess_afterRounding;
	state.roundingMode = soft_rounding(mxcsr);
	return dbl ? f128_to_f64(&state, value).v : f128_to_f32(&state, value).v;
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
	if (reciprocal) {
		if (magnitude >= 0x7e80'0000U && magnitude <= 0x7f80'0000U)
			return x & f.sign();
		const auto wide = soft_wide(false, x);
		return soft_round(soft_odd([&](softfloat_state &state) {
			return f128_div(&state, one, wide);
		}), false, mxcsr);
	}
	if (f.negative(x) && magnitude != 0)
		return f.indefinite();
	const auto wide = soft_wide(false, x);
	const auto root = soft_round(soft_odd([&](softfloat_state &state) {
		return f128_sqrt(&state, wide);
	}), false, mxcsr);
	const auto wide_root = soft_wide(false, root);
	return soft_round(soft_odd([&](softfloat_state &state) {
		return f128_div(&state, one, wide_root);
	}), false, mxcsr);
}

} // namespace

std::uint32_t x86_fp_compute(const FpOperation &operation,
	std::uint32_t mxcsr, FpValues &values) noexcept
{
	const bool dbl = operation.element_bytes == 8;
	const auto lanes = std::min<std::size_t>(operation.lanes,
		16U / operation.element_bytes);
	switch (operation.kind) {
	case FpKind::rcp:
	case FpKind::rsqrt:
		for (std::size_t index = 0; index < lanes; ++index)
			set_lane(values.result, dbl, index,
				estimate(operation.kind == FpKind::rcp,
					lane(values.sources[0], dbl, index), mxcsr));
		return 0;
	default:
		return 0;
	}
}

} // namespace uml::cpu::x86_64::jit
