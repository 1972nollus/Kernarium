// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The semantic library's floating-point forms: the arithmetic, fused,
 * complex, compare, reduction and pairwise forms, the estimates, rounding
 * and trigonometric helpers, the precision and integer conversions, and the
 * BFloat16 and half-precision widening, dot-product and matrix forms.
 *
 * Every active element is computed by the host's own instruction for its
 * operation under the guest FPCR (without trap enables, and without AHP,
 * which FPUnpack() and FPConvertSVE() clear), so rounding, FZ, FZ16, DN and,
 * where the virtual CPU implements FEAT_AFP, FIZ, AH and NEP behave as the
 * hardware's, and the cumulative flags it raises go to FPSR. An inactive
 * element is never computed, so it raises nothing. The operations no host
 * instruction performs (FSCALE, FLOGB, FEXPA, FTSSEL's selection, FTSMUL's
 * sign and the half-precision FDOT's NaN selection) follow their pseudocode
 * around host operations. As in the engines (floating_controls.hpp), the
 * BFloat16 and FP16-to-FP32 widening forms use the shared software models
 * while the FEAT_AFP controls are clear and the host's instructions, which
 * implements_afp() guarantees, otherwise.
 */
#include "sve_semantics_internal.hpp"

#include "cpu/arm64/detail/bfloat_conversion.hpp"
#include "cpu/arm64/detail/bfloat_dot.hpp"
#include "cpu/arm64/detail/bfloat_multiply.hpp"
#include "cpu/arm64/detail/floating_bits.hpp"
#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/detail/half_fused_long.hpp"

#include <algorithm>

namespace uml::cpu::arm64::detail::sve_semantics {

namespace {

using Bits = std::uint64_t;
using Quad = std::uint8_t __attribute__((vector_size(16)));

inline constexpr std::uint32_t fpcr_fiz = 1U << 0;
inline constexpr std::uint32_t fpcr_ah = 1U << 1;
inline constexpr std::uint32_t fpcr_fz16 = 1U << 19;
inline constexpr std::uint32_t fpcr_rounding = 3U << 22;
inline constexpr std::uint32_t fpcr_fz = 1U << 24;
inline constexpr std::uint32_t fpcr_dn = 1U << 25;
inline constexpr std::uint32_t fpcr_ahp = 1U << 26;
inline constexpr std::uint32_t fpsr_invalid = 1U << 0;
inline constexpr std::uint32_t fpsr_input_denormal = 1U << 7;

[[nodiscard]] constexpr Bits mask_of(unsigned bytes) noexcept
{
	return floating_bits::low_mask(bytes * 8U);
}

/* Element `index` of `bytes` bytes, zero-extended. */
[[nodiscard]] Bits get(
	const Vector &vector, unsigned index, unsigned bytes) noexcept
{
	Bits value = 0;
	std::memcpy(&value, vector.bytes.data() + index * bytes, bytes);
	return value;
}

void put(Vector &vector, unsigned index, unsigned bytes, Bits value) noexcept
{
	std::memcpy(vector.bytes.data() + index * bytes, &value, bytes);
}

[[nodiscard]] Quad quad_of(const Vector &vector) noexcept
{
	Quad quad;
	std::memcpy(&quad, vector.bytes.data(), sizeof(quad));
	return quad;
}

[[nodiscard]] Vector vector_of(const Quad &quad) noexcept
{
	Vector vector;
	std::memcpy(vector.bytes.data(), &quad, sizeof(quad));
	return vector;
}

[[nodiscard]] constexpr Bits sign_of(unsigned bytes) noexcept
{
	return Bits{1} << (bytes * 8U - 1U);
}

[[nodiscard]] constexpr bool is_nan(unsigned bytes, Bits value) noexcept
{
	const auto format = floating_bits::format(bytes * 8U);
	const Bits exponent = floating_bits::low_mask(format.exponent_bits);
	return ((value >> format.fraction_bits) & exponent) == exponent &&
		(value & floating_bits::low_mask(format.fraction_bits)) != 0U;
}

/* FPOne(), FPPointFive() and FPTwo() with a clear sign, and FPInfinity(). */
[[nodiscard]] constexpr Bits one(unsigned bytes) noexcept
{
	return bytes == 2U  ? 0x3c00U :
		bytes == 4U ? 0x3f80'0000U :
			      0x3ff0'0000'0000'0000ULL;
}

[[nodiscard]] constexpr Bits point_five(unsigned bytes) noexcept
{
	return bytes == 2U  ? 0x3800U :
		bytes == 4U ? 0x3f00'0000U :
			      0x3fe0'0000'0000'0000ULL;
}

[[nodiscard]] constexpr Bits two(unsigned bytes) noexcept
{
	return bytes == 2U  ? 0x4000U :
		bytes == 4U ? 0x4000'0000U :
			      0x4000'0000'0000'0000ULL;
}

[[nodiscard]] constexpr Bits infinity(unsigned bytes, bool negative) noexcept
{
	const auto format = floating_bits::format(bytes * 8U);
	return (negative ? sign_of(bytes) : 0U) |
		floating_bits::low_mask(format.exponent_bits)
		<< format.fraction_bits;
}

/* FPDefaultNaN(): its sign is FPCR.AH. */
[[nodiscard]] constexpr Bits default_nan(
	unsigned bytes, std::uint32_t fpcr) noexcept
{
	const auto format = floating_bits::format(bytes * 8U);
	return infinity(bytes, (fpcr & fpcr_ah) != 0U) |
		Bits{1} << (format.fraction_bits - 1U);
}

/*
 * Runs body(), which issues host floating-point instructions as asm
 * volatile, with `fpcr` installed but for its trap enables and AHP, and
 * returns the cumulative flags it raised.
 */
template<typename Body>
[[nodiscard]] std::uint32_t raised_by(std::uint32_t fpcr, Body &&body) noexcept
{
	const auto captured =
		run_with_local_fpcr(host_fpcr(fpcr) & ~fpcr_ahp, [&] {
			body();
			return true;
		});
	return captured.fpsr & fpsr_exception_cumulative_mask;
}

/* body() under the guest FPCR, with the flags it raises accumulated into
 * FPSR. */
template<typename Body>
void under_guest_fpcr(Context &context, Body &&body) noexcept
{
	context.cpu.fpsr |= raised_by(context.cpu.fpcr, body);
}

/* Whether any FEAT_AFP control is set, which only a host that implements
 * the extensions the software models stand in for allows. */
[[nodiscard]] bool afp_controls(const Context &context) noexcept
{
	return (context.cpu.fpcr & fpcr_afp_controls) != 0U;
}

/*
 * The host's scalar instructions. Each takes and returns an element's bits
 * in the low `bytes` of a SIMD&FP register; the result is zero-extended.
 */
#define UML_SVE_HOST(h, s, d, ...)                                             \
	do {                                                                   \
		if (bytes == 2U)                                               \
			__asm__ volatile(h : "=w"(result) : __VA_ARGS__);      \
		else if (bytes == 4U)                                          \
			__asm__ volatile(s : "=w"(result) : __VA_ARGS__);      \
		else                                                           \
			__asm__ volatile(d : "=w"(result) : __VA_ARGS__);      \
	} while (false)
#define UML_SVE_BINARY(mnemonic)                                               \
	UML_SVE_HOST(mnemonic " %h0, %h1, %h2", mnemonic " %s0, %s1, %s2",     \
		mnemonic " %d0, %d1, %d2", "w"(x), "w"(y))
#define UML_SVE_UNARY(mnemonic)                                                \
	UML_SVE_HOST(mnemonic " %h0, %h1", mnemonic " %s0, %s1",               \
		mnemonic " %d0, %d1", "w"(x))
#define UML_SVE_ZERO(mnemonic)                                                 \
	UML_SVE_HOST(mnemonic " %h0, %h1, #0.0", mnemonic " %s0, %s1, #0.0",   \
		mnemonic " %d0, %d1, #0.0", "w"(x))

enum class Binary : std::uint8_t {
	add,
	subtract,
	multiply,
	divide,
	maximum,
	minimum,
	maximum_number,
	minimum_number,
	absolute_difference,
	multiply_extended,
	reciprocal_step,
	reciprocal_sqrt_step,
	equal,
	greater_equal,
	greater,
	absolute_greater_equal,
	absolute_greater,
};

[[nodiscard]] Bits binary(
	Binary operation, unsigned bytes, Bits first, Bits second) noexcept
{
	const double x = std::bit_cast<double>(first);
	const double y = std::bit_cast<double>(second);
	double result{};
	switch (operation) {
	case Binary::add:
		UML_SVE_BINARY("fadd");
		break;
	case Binary::subtract:
		UML_SVE_BINARY("fsub");
		break;
	case Binary::multiply:
		UML_SVE_BINARY("fmul");
		break;
	case Binary::divide:
		UML_SVE_BINARY("fdiv");
		break;
	case Binary::maximum:
		UML_SVE_BINARY("fmax");
		break;
	case Binary::minimum:
		UML_SVE_BINARY("fmin");
		break;
	case Binary::maximum_number:
		UML_SVE_BINARY("fmaxnm");
		break;
	case Binary::minimum_number:
		UML_SVE_BINARY("fminnm");
		break;
	case Binary::absolute_difference:
		UML_SVE_BINARY("fabd");
		break;
	case Binary::multiply_extended:
		UML_SVE_BINARY("fmulx");
		break;
	case Binary::reciprocal_step:
		UML_SVE_BINARY("frecps");
		break;
	case Binary::reciprocal_sqrt_step:
		UML_SVE_BINARY("frsqrts");
		break;
	case Binary::equal:
		UML_SVE_BINARY("fcmeq");
		break;
	case Binary::greater_equal:
		UML_SVE_BINARY("fcmge");
		break;
	case Binary::greater:
		UML_SVE_BINARY("fcmgt");
		break;
	case Binary::absolute_greater_equal:
		UML_SVE_BINARY("facge");
		break;
	case Binary::absolute_greater:
		UML_SVE_BINARY("facgt");
		break;
	}
	return std::bit_cast<Bits>(result) & mask_of(bytes);
}

enum class Unary : std::uint8_t {
	absolute,
	negate,
	square_root,
	reciprocal_estimate,
	reciprocal_sqrt_estimate,
	reciprocal_exponent,
	round_even,
	round_up,
	round_down,
	round_zero,
	round_away,
	round_exact,
	round_current,
	equal_zero,
	greater_equal_zero,
	greater_zero,
	less_equal_zero,
	less_zero,
};

[[nodiscard]] Bits unary(Unary operation, unsigned bytes, Bits value) noexcept
{
	const double x = std::bit_cast<double>(value);
	double result{};
	switch (operation) {
	case Unary::absolute:
		UML_SVE_UNARY("fabs");
		break;
	case Unary::negate:
		UML_SVE_UNARY("fneg");
		break;
	case Unary::square_root:
		UML_SVE_UNARY("fsqrt");
		break;
	case Unary::reciprocal_estimate:
		UML_SVE_UNARY("frecpe");
		break;
	case Unary::reciprocal_sqrt_estimate:
		UML_SVE_UNARY("frsqrte");
		break;
	case Unary::reciprocal_exponent:
		UML_SVE_UNARY("frecpx");
		break;
	case Unary::round_even:
		UML_SVE_UNARY("frintn");
		break;
	case Unary::round_up:
		UML_SVE_UNARY("frintp");
		break;
	case Unary::round_down:
		UML_SVE_UNARY("frintm");
		break;
	case Unary::round_zero:
		UML_SVE_UNARY("frintz");
		break;
	case Unary::round_away:
		UML_SVE_UNARY("frinta");
		break;
	case Unary::round_exact:
		UML_SVE_UNARY("frintx");
		break;
	case Unary::round_current:
		UML_SVE_UNARY("frinti");
		break;
	case Unary::equal_zero:
		UML_SVE_ZERO("fcmeq");
		break;
	case Unary::greater_equal_zero:
		UML_SVE_ZERO("fcmge");
		break;
	case Unary::greater_zero:
		UML_SVE_ZERO("fcmgt");
		break;
	case Unary::less_equal_zero:
		UML_SVE_ZERO("fcmle");
		break;
	case Unary::less_zero:
		UML_SVE_ZERO("fcmlt");
		break;
	}
	return std::bit_cast<Bits>(result) & mask_of(bytes);
}

/* FPMulAdd(addend, first, second): the host's FMADD. */
[[nodiscard]] Bits fused(
	unsigned bytes, Bits addend, Bits first, Bits second) noexcept
{
	const double a = std::bit_cast<double>(addend);
	const double x = std::bit_cast<double>(first);
	const double y = std::bit_cast<double>(second);
	double result{};
	UML_SVE_HOST("fmadd %h0, %h1, %h2, %h3", "fmadd %s0, %s1, %s2, %s3",
		"fmadd %d0, %d1, %d2, %d3", "w"(x), "w"(y), "w"(a));
	return std::bit_cast<Bits>(result) & mask_of(bytes);
}

/* FPCompareUN(): the host's quiet FCMP, unordered. */
[[nodiscard]] bool unordered(unsigned bytes, Bits first, Bits second) noexcept
{
	const double x = std::bit_cast<double>(first);
	const double y = std::bit_cast<double>(second);
	std::uint32_t result;
	if (bytes == 2U)
		__asm__ volatile("fcmp %h1, %h2\n\tcset %w0, vs"
			: "=r"(result)
			: "w"(x), "w"(y)
			: "cc");
	else if (bytes == 4U)
		__asm__ volatile("fcmp %s1, %s2\n\tcset %w0, vs"
			: "=r"(result)
			: "w"(x), "w"(y)
			: "cc");
	else
		__asm__ volatile("fcmp %d1, %d2\n\tcset %w0, vs"
			: "=r"(result)
			: "w"(x), "w"(y)
			: "cc");
	return result != 0U;
}

/* FPConvert() between precisions, and FCVTXN's round-to-odd FPConvert()
 * of a double to a single. */
[[nodiscard]] Bits convert(unsigned from, unsigned to, Bits value) noexcept
{
	const double x = std::bit_cast<double>(value);
	double result{};
	if (from == 2U && to == 4U)
		__asm__ volatile("fcvt %s0, %h1" : "=w"(result) : "w"(x));
	else if (from == 2U)
		__asm__ volatile("fcvt %d0, %h1" : "=w"(result) : "w"(x));
	else if (from == 4U && to == 2U)
		__asm__ volatile("fcvt %h0, %s1" : "=w"(result) : "w"(x));
	else if (from == 4U)
		__asm__ volatile("fcvt %d0, %s1" : "=w"(result) : "w"(x));
	else if (to == 2U)
		__asm__ volatile("fcvt %h0, %d1" : "=w"(result) : "w"(x));
	else
		__asm__ volatile("fcvt %s0, %d1" : "=w"(result) : "w"(x));
	return std::bit_cast<Bits>(result) & mask_of(to);
}

[[nodiscard]] Bits convert_odd(Bits value) noexcept
{
	const double x = std::bit_cast<double>(value);
	double result{};
	__asm__ volatile("fcvtxn %s0, %d1" : "=w"(result) : "w"(x));
	return std::bit_cast<Bits>(result) & mask_of(4U);
}

/* FPToFixed() with no fraction bits, rounding toward zero, from a `from`
 * byte element to a `to` byte integer. */
[[nodiscard]] Bits to_integer(
	bool is_unsigned, unsigned from, unsigned to, Bits value) noexcept
{
	const double x = std::bit_cast<double>(value);
	Bits result{};
	if (from == 2U && to == 2U) {
		double half{};
		if (is_unsigned)
			__asm__ volatile("fcvtzu %h0, %h1"
				: "=w"(half)
				: "w"(x));
		else
			__asm__ volatile("fcvtzs %h0, %h1"
				: "=w"(half)
				: "w"(x));
		return std::bit_cast<Bits>(half) & mask_of(2U);
	}
#define UML_SVE_TO_INTEGER(destination, source)                                \
	do {                                                                   \
		if (is_unsigned)                                               \
			__asm__ volatile("fcvtzu " destination ", " source     \
				: "=r"(result)                                 \
				: "w"(x));                                     \
		else                                                           \
			__asm__ volatile("fcvtzs " destination ", " source     \
				: "=r"(result)                                 \
				: "w"(x));                                     \
	} while (false)
	if (from == 2U && to == 4U)
		UML_SVE_TO_INTEGER("%w0", "%h1");
	else if (from == 2U)
		UML_SVE_TO_INTEGER("%x0", "%h1");
	else if (from == 4U && to == 4U)
		UML_SVE_TO_INTEGER("%w0", "%s1");
	else if (from == 4U)
		UML_SVE_TO_INTEGER("%x0", "%s1");
	else if (to == 4U)
		UML_SVE_TO_INTEGER("%w0", "%d1");
	else
		UML_SVE_TO_INTEGER("%x0", "%d1");
#undef UML_SVE_TO_INTEGER
	return result & mask_of(to);
}

/* FixedToFP() with no fraction bits, under the FPCR rounding mode, from a
 * `from` byte integer to a `to` byte element. */
[[nodiscard]] Bits to_floating(
	bool is_unsigned, unsigned from, unsigned to, Bits value) noexcept
{
	double result{};
	if (from == 2U) {
		const double x = std::bit_cast<double>(value);
		if (is_unsigned)
			__asm__ volatile("ucvtf %h0, %h1"
				: "=w"(result)
				: "w"(x));
		else
			__asm__ volatile("scvtf %h0, %h1"
				: "=w"(result)
				: "w"(x));
		return std::bit_cast<Bits>(result) & mask_of(2U);
	}
#define UML_SVE_TO_FLOATING(destination, source)                               \
	do {                                                                   \
		if (is_unsigned)                                               \
			__asm__ volatile("ucvtf " destination ", " source      \
				: "=w"(result)                                 \
				: "r"(value));                                 \
		else                                                           \
			__asm__ volatile("scvtf " destination ", " source      \
				: "=w"(result)                                 \
				: "r"(value));                                 \
	} while (false)
	if (from == 4U && to == 2U)
		UML_SVE_TO_FLOATING("%h0", "%w1");
	else if (from == 4U && to == 4U)
		UML_SVE_TO_FLOATING("%s0", "%w1");
	else if (from == 4U)
		UML_SVE_TO_FLOATING("%d0", "%w1");
	else if (to == 2U)
		UML_SVE_TO_FLOATING("%h0", "%x1");
	else if (to == 4U)
		UML_SVE_TO_FLOATING("%s0", "%x1");
	else
		UML_SVE_TO_FLOATING("%d0", "%x1");
#undef UML_SVE_TO_FLOATING
	return std::bit_cast<Bits>(result) & mask_of(to);
}

#undef UML_SVE_ZERO
#undef UML_SVE_UNARY
#undef UML_SVE_BINARY
#undef UML_SVE_HOST

/* FPNeg() and FPAbs(), which raise nothing and honour FPCR.AH. */
[[nodiscard]] Bits negate(unsigned bytes, Bits value) noexcept
{
	return unary(Unary::negate, bytes, value);
}

/*
 * FPScale(value, scale) as a chain of host multiplications by normal powers
 * of two. Only the last can round: every earlier one is exact, because its
 * product stays within the normal range, so the chain rounds the exact
 * product once, under every FPCR mode, as one multiplication by 2^scale
 * would. A NaN, infinity or zero is the host's product by one, and a
 * denormal the host flushes stays a signed zero through the chain. Once
 * the exact result's exponent falls below twice the minimum normal
 * exponent it is less than half the smallest denormal, so the chain stops
 * there with the same rounding and flags. FPClampScale() bounds the loops.
 */
[[nodiscard]] Bits scale(unsigned bytes, Bits value, Bits scale_bits) noexcept
{
	const auto format = floating_bits::format(bytes * 8U);
	const int fraction_bits = static_cast<int>(format.fraction_bits);
	const Bits exponent_ones =
		floating_bits::low_mask(format.exponent_bits);
	const Bits exponent_field =
		(value >> format.fraction_bits) & exponent_ones;
	const Bits fraction =
		value & floating_bits::low_mask(format.fraction_bits);
	if (exponent_field == exponent_ones ||
		(exponent_field == 0U && fraction == 0U))
		return binary(Binary::multiply, bytes, value, one(bytes));

	const unsigned bits = bytes * 8U;
	const auto requested =
		static_cast<std::int64_t>(scale_bits << (64U - bits)) >>
		(64U - bits);
	const auto field = static_cast<std::int64_t>(exponent_field);
	const std::int64_t minimum_scale = -(fraction_bits + 1);
	const std::int64_t maximum_scale =
		static_cast<std::int64_t>(exponent_ones) + fraction_bits + 1;
	const int clamped = static_cast<int>(std::max(minimum_scale - field,
		std::min(requested, maximum_scale - field)));

	const int bias = format.bias;
	const int minimum = 1 - bias;
	const int maximum = bias;
	const auto power = [&](int exponent) {
		return static_cast<Bits>(exponent + bias)
			<< format.fraction_bits;
	};
	const auto multiply = [&](Bits first, int exponent) {
		return binary(Binary::multiply, bytes, first, power(exponent));
	};
	if (clamped >= minimum && clamped <= maximum)
		return multiply(value, clamped);
	if (clamped > maximum) {
		Bits result = multiply(value, maximum);
		int remaining = clamped - maximum;
		for (; remaining > maximum; remaining -= maximum)
			result = multiply(result, maximum);
		return multiply(result, remaining);
	}
	const int exponent = exponent_field != 0U ?
		static_cast<int>(exponent_field) - bias :
		minimum - fraction_bits +
			static_cast<int>(std::bit_width(fraction)) - 1;
	/* Bring the value exactly to the minimum normal exponent. */
	Bits result = value;
	int step = minimum - exponent;
	if (step > 0)
		result = multiply(result, step);
	for (; step < 0; step -= std::max(step, minimum))
		result = multiply(result, std::max(step, minimum));
	return multiply(
		result, std::max(exponent + clamped - minimum, minimum));
}

/* FPLogB(): the exponent of the value FPUnpack() reads, as an integer. */
[[nodiscard]] Bits log_base_two(unsigned bytes, Bits value, std::uint32_t fpcr,
	std::uint32_t &flags) noexcept
{
	const auto format = floating_bits::format(bytes * 8U);
	const unsigned bits = bytes * 8U;
	const Bits minimum = Bits{1} << (bits - 1U);
	const Bits exponent_ones =
		floating_bits::low_mask(format.exponent_bits);
	const Bits exponent = (value >> format.fraction_bits) & exponent_ones;
	const Bits fraction =
		value & floating_bits::low_mask(format.fraction_bits);
	const bool alternative = (fpcr & fpcr_ah) != 0U;
	if (exponent == exponent_ones) {
		if (fraction == 0U)
			return minimum - 1U;
		flags |= fpsr_invalid;
		return minimum;
	}
	if (exponent != 0U)
		return static_cast<Bits>(static_cast<std::int64_t>(exponent) -
			       format.bias) &
			mask_of(bytes);
	bool zero = fraction == 0U;
	if (!zero && bits == 16U) {
		zero = (fpcr & fpcr_fz16) != 0U;
	} else if (!zero) {
		const bool flush = (fpcr & fpcr_fz) != 0U && !alternative;
		zero = flush || (fpcr & fpcr_fiz) != 0U;
		if (flush)
			flags |= fpsr_input_denormal;
	}
	if (zero) {
		flags |= fpsr_invalid;
		return minimum;
	}
	if (alternative && bits != 16U)
		flags |= fpsr_input_denormal;
	const int normalized = 1 - format.bias -
		static_cast<int>(format.fraction_bits) +
		static_cast<int>(std::bit_width(fraction)) - 1;
	return static_cast<Bits>(static_cast<std::int64_t>(normalized)) &
		mask_of(bytes);
}

/* FPExpA() and FPTrigMAdd()'s coefficients (FPExpCoefficient(),
 * FPTrigMAddCoefficient()). */
constexpr std::array<std::uint16_t, 32> exponential_16{
	0x0U,
	0x16U,
	0x2dU,
	0x45U,
	0x5dU,
	0x75U,
	0x8eU,
	0xa8U,
	0xc2U,
	0xdcU,
	0xf8U,
	0x114U,
	0x130U,
	0x14dU,
	0x16bU,
	0x189U,
	0x1a8U,
	0x1c8U,
	0x1e8U,
	0x209U,
	0x22bU,
	0x24eU,
	0x271U,
	0x295U,
	0x2baU,
	0x2e0U,
	0x306U,
	0x32eU,
	0x356U,
	0x37fU,
	0x3a9U,
	0x3d4U,
};
constexpr std::array<std::uint32_t, 64> exponential_32{
	0x0U,
	0x1'64d2U,
	0x2'cd87U,
	0x4'3a29U,
	0x5'aac3U,
	0x7'1f62U,
	0x8'980fU,
	0xa'14d5U,
	0xb'95c2U,
	0xd'1adfU,
	0xe'a43aU,
	0x10'31dcU,
	0x11'c3d3U,
	0x13'5a2bU,
	0x14'f4f0U,
	0x16'942dU,
	0x18'37f0U,
	0x19'e046U,
	0x1b'8d3aU,
	0x1d'3edaU,
	0x1e'f532U,
	0x20'b051U,
	0x22'7043U,
	0x24'3516U,
	0x25'fed7U,
	0x27'cd94U,
	0x29'a15bU,
	0x2b'7a3aU,
	0x2d'583fU,
	0x2f'3b79U,
	0x31'23f6U,
	0x33'11c4U,
	0x35'04f3U,
	0x36'fd92U,
	0x38'fbafU,
	0x3a'ff5bU,
	0x3d'08a4U,
	0x3f'179aU,
	0x41'2c4dU,
	0x43'46cdU,
	0x45'672aU,
	0x47'8d75U,
	0x49'b9beU,
	0x4b'ec15U,
	0x4e'248cU,
	0x50'6334U,
	0x52'a81eU,
	0x54'f35bU,
	0x57'44fdU,
	0x59'9d16U,
	0x5b'fbb8U,
	0x5e'60f5U,
	0x60'ccdfU,
	0x63'3f89U,
	0x65'b907U,
	0x68'396aU,
	0x6a'c0c7U,
	0x6d'4f30U,
	0x6f'e4baU,
	0x72'8177U,
	0x75'257dU,
	0x77'd0dfU,
	0x7a'83b3U,
	0x7d'3e0cU,
};
constexpr std::array<std::uint64_t, 64> exponential_64{
	0x0ULL,
	0x2c9a'3e77'8061ULL,
	0x59b0'd315'8574ULL,
	0x8745'1875'9bc8ULL,
	0xb558'6cf9'890fULL,
	0xe3ec'32d3'd1a2ULL,
	0x1'1301'd012'5b51ULL,
	0x1'429a'aea9'2de0ULL,
	0x1'72b8'3c7d'517bULL,
	0x1'a35b'eb6f'cb75ULL,
	0x1'd487'3168'b9aaULL,
	0x2'063b'8862'8cd6ULL,
	0x2'387a'6e75'6238ULL,
	0x2'6b45'65e2'7cddULL,
	0x2'9e9d'f51f'dee1ULL,
	0x2'd285'a6e4'030bULL,
	0x3'06fe'0a31'b715ULL,
	0x3'3c08'b264'16ffULL,
	0x3'71a7'373a'a9cbULL,
	0x3'a7db'34e5'9ff7ULL,
	0x3'dea6'4c12'3422ULL,
	0x4'160a'21f7'2e2aULL,
	0x4'4e08'6061'892dULL,
	0x4'86a2'b5c1'3cd0ULL,
	0x4'bfda'd536'2a27ULL,
	0x4'f9b2'769d'2ca7ULL,
	0x5'342b'569d'4f82ULL,
	0x5'6f47'36b5'27daULL,
	0x5'ab07'dd48'5429ULL,
	0x5'e76f'15ad'2148ULL,
	0x6'247e'b03a'5585ULL,
	0x6'6238'8255'2225ULL,
	0x6'a09e'667f'3bcdULL,
	0x6'dfb2'3c65'1a2fULL,
	0x7'1f75'e8ec'5f74ULL,
	0x7'5feb'5642'67c9ULL,
	0x7'a114'73eb'0187ULL,
	0x7'e2f3'36cf'4e62ULL,
	0x8'2589'994c'ce13ULL,
	0x8'68d9'9b44'92edULL,
	0x8'ace5'422a'a0dbULL,
	0x8'f1ae'9915'7736ULL,
	0x9'3737'b0cd'c5e5ULL,
	0x9'7d82'9fde'4e50ULL,
	0x9'c491'82a3'f090ULL,
	0xa'0c66'7b5d'e565ULL,
	0xa'5503'b23e'255dULL,
	0xa'9e6b'5579'fdbfULL,
	0xa'e89f'995a'd3adULL,
	0xb'33a2'b84f'15fbULL,
	0xb'7f76'f2fb'5e47ULL,
	0xb'cc1e'904b'c1d2ULL,
	0xc'199b'dd85'529cULL,
	0xc'67f1'2e57'd14bULL,
	0xc'b720'dcef'9069ULL,
	0xd'072d'4a07'897cULL,
	0xd'5818'dcfb'a487ULL,
	0xd'a9e6'03db'3285ULL,
	0xd'fc97'337b'9b5fULL,
	0xe'502e'e78b'3ff6ULL,
	0xe'a4af'a2a4'90daULL,
	0xe'fa1b'ee61'5a27ULL,
	0xf'5076'5b6e'4540ULL,
	0xf'a7c1'819e'90d8ULL,
};
constexpr std::array<std::uint16_t, 16> trig_16{
	0x3c00U,
	0xb155U,
	0x2030U,
	0x0000U,
	0x0000U,
	0x0000U,
	0x0000U,
	0x0000U,
	0x3c00U,
	0xb800U,
	0x293aU,
	0x0000U,
	0x0000U,
	0x0000U,
	0x0000U,
	0x0000U,
};
constexpr std::array<std::uint32_t, 16> trig_32{
	0x3f80'0000U,
	0xbe2a'aaabU,
	0x3c08'8886U,
	0xb950'08b9U,
	0x3636'9d6dU,
	0x0000'0000U,
	0x0000'0000U,
	0x0000'0000U,
	0x3f80'0000U,
	0xbf00'0000U,
	0x3d2a'aaa6U,
	0xbab6'0705U,
	0x37cd'37ccU,
	0x0000'0000U,
	0x0000'0000U,
	0x0000'0000U,
};
constexpr std::array<std::uint64_t, 16> trig_64{
	0x3ff0'0000'0000'0000ULL,
	0xbfc5'5555'5555'5543ULL,
	0x3f81'1111'1110'f30cULL,
	0xbf2a'01a0'19b9'2fc6ULL,
	0x3ec7'1de3'51f3'd22bULL,
	0xbe5a'e5e2'b60f'7b91ULL,
	0x3de5'd840'8868'552fULL,
	0x0000'0000'0000'0000ULL,
	0x3ff0'0000'0000'0000ULL,
	0xbfe0'0000'0000'0000ULL,
	0x3fa5'5555'5555'5536ULL,
	0xbf56'c16c'16c1'3a0bULL,
	0x3efa'01a0'19b1'e8d8ULL,
	0xbe92'7e4f'7282'f468ULL,
	0x3e21'ee96'd264'1b13ULL,
	0xbda8'f763'80fb'b401ULL,
};

[[nodiscard]] Bits exponential_accelerator(unsigned bytes, Bits value) noexcept
{
	switch (bytes) {
	case 2:
		return ((value >> 5U) & 0x1fU) << 10U |
			exponential_16[value & 0x1fU];
	case 4:
		return ((value >> 6U) & 0xffU) << 23U |
			exponential_32[value & 0x3fU];
	default:
		return ((value >> 6U) & 0x7ffU) << 52U |
			exponential_64[value & 0x3fU];
	}
}

[[nodiscard]] Bits trig_coefficient(unsigned bytes, unsigned index) noexcept
{
	return bytes == 2U  ? trig_16[index] :
		bytes == 4U ? trig_32[index] :
			      trig_64[index];
}

/* The two-operand forms with a governing predicate, destructive in Zdn:
 * FADD, FSUB, FMUL, FSUBR, FMAXNM, FMINNM, FMAX, FMIN, FABD, FSCALE, FMULX,
 * FDIVR and FDIV (vectors). */
SveResult binary_predicated(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const unsigned operation = context.field(16, 4);
	if (operation == 11U || operation >= 14U)
		return fatal();
	const Predicate &mask = context.p(context.field(10, 3));
	const unsigned dn = context.field(0, 5);
	const Vector left = context.z(dn);
	const Vector right = context.z(context.field(5, 5));
	Vector result = left;
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e) {
			if (!active(mask, e, bytes))
				continue;
			const Bits a = get(left, e, bytes);
			const Bits b = get(right, e, bytes);
			Bits r;
			switch (operation) {
			case 0:
				r = binary(Binary::add, bytes, a, b);
				break;
			case 1:
				r = binary(Binary::subtract, bytes, a, b);
				break;
			case 2:
				r = binary(Binary::multiply, bytes, a, b);
				break;
			case 3:
				r = binary(Binary::subtract, bytes, b, a);
				break;
			case 4:
				r = binary(Binary::maximum_number, bytes, a, b);
				break;
			case 5:
				r = binary(Binary::minimum_number, bytes, a, b);
				break;
			case 6:
				r = binary(Binary::maximum, bytes, a, b);
				break;
			case 7:
				r = binary(Binary::minimum, bytes, a, b);
				break;
			case 8:
				r = binary(Binary::absolute_difference, bytes,
					a, b);
				break;
			case 9:
				r = scale(bytes, a, b);
				break;
			case 10:
				r = binary(
					Binary::multiply_extended, bytes, a, b);
				break;
			case 12:
				r = binary(Binary::divide, bytes, b, a);
				break;
			default:
				r = binary(Binary::divide, bytes, a, b);
				break;
			}
			put(result, e, bytes, r);
		}
	});
	context.set_z(dn, result);
	return retired();
}

/* FADD, FSUB, FMUL, FSUBR, FMAXNM, FMINNM, FMAX and FMIN (immediate). */
SveResult binary_immediate(Context &context)
{
	static constexpr std::array operations{Binary::add, Binary::subtract,
		Binary::multiply, Binary::subtract, Binary::maximum_number,
		Binary::minimum_number, Binary::maximum, Binary::minimum};
	const unsigned bytes = esize_of(context.field(22, 2));
	const unsigned operation = context.field(16, 3);
	const bool i1 = context.bit(5);
	Bits immediate;
	if (operation == 2U)
		immediate = i1 ? two(bytes) : point_five(bytes);
	else if (operation >= 4U)
		immediate = i1 ? one(bytes) : 0U;
	else
		immediate = i1 ? one(bytes) : point_five(bytes);
	const Predicate &mask = context.p(context.field(10, 3));
	const unsigned dn = context.field(0, 5);
	Vector result = context.z(dn);
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e) {
			if (!active(mask, e, bytes))
				continue;
			const Bits a = get(result, e, bytes);
			put(result, e, bytes,
				operation == 3U ? binary(operations[operation],
							  bytes, immediate, a) :
						  binary(operations[operation],
							  bytes, a, immediate));
		}
	});
	context.set_z(dn, result);
	return retired();
}

/* FADD, FSUB, FMUL, FTSMUL, FRECPS and FRSQRTS (unpredicated). */
SveResult binary_unpredicated(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const unsigned operation = context.field(10, 3);
	if (operation == 4U || operation == 5U)
		return fatal();
	const Vector left = context.z(context.field(5, 5));
	const Vector right = context.z(context.field(16, 5));
	Vector result;
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e) {
			const Bits a = get(left, e, bytes);
			const Bits b = get(right, e, bytes);
			Bits r;
			switch (operation) {
			case 0:
				r = binary(Binary::add, bytes, a, b);
				break;
			case 1:
				r = binary(Binary::subtract, bytes, a, b);
				break;
			case 2:
				r = binary(Binary::multiply, bytes, a, b);
				break;
			case 3:
				/* FPTrigSMul(): op1 squared, with the sign of
				 * op2's bit 0 unless the product is a NaN. */
				r = binary(Binary::multiply, bytes, a, a);
				if (!is_nan(bytes, r))
					r = (r & ~sign_of(bytes)) |
						((b & 1U) != 0U ?
								sign_of(bytes) :
								0U);
				break;
			case 6:
				r = binary(
					Binary::reciprocal_step, bytes, a, b);
				break;
			default:
				r = binary(Binary::reciprocal_sqrt_step, bytes,
					a, b);
				break;
			}
			put(result, e, bytes, r);
		}
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* An element-wise conversion or one-operand operation with a governing
 * predicate, merging: element e of `bytes` bytes of Zd becomes
 * operation(element e of Zn). */
template<typename Operation>
SveResult unary_merging(Context &context, unsigned bytes, Operation &&operation)
{
	const Predicate &mask = context.p(context.field(10, 3));
	const unsigned d = context.field(0, 5);
	const Vector source = context.z(context.field(5, 5));
	Vector result = context.z(d);
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e)
			if (active(mask, e, bytes))
				put(result, e, bytes,
					operation(get(source, e, bytes)));
	});
	context.set_z(d, result);
	return retired();
}

SveResult unary_operation(Context &context, Unary operation)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	return unary_merging(context, bytes, [&](Bits value) {
		return unary(operation, bytes, value);
	});
}

/* FABS and FNEG. */
SveResult absolute_or_negate(Context &context)
{
	return unary_operation(
		context, context.bit(16) ? Unary::negate : Unary::absolute);
}

SveResult reciprocal_exponent(Context &context)
{
	return unary_operation(context, Unary::reciprocal_exponent);
}

SveResult square_root(Context &context)
{
	return unary_operation(context, Unary::square_root);
}

/* FRINTN, FRINTP, FRINTM, FRINTZ, FRINTA, FRINTX and FRINTI. */
SveResult round_to_integral(Context &context)
{
	static constexpr std::array operations{Unary::round_even,
		Unary::round_up, Unary::round_down, Unary::round_zero,
		Unary::round_away, Unary::round_away, Unary::round_exact,
		Unary::round_current};
	const unsigned operation = context.field(16, 3);
	if (operation == 5U)
		return fatal();
	return unary_operation(context, operations[operation]);
}

SveResult log_base_two_form(Context &context)
{
	const unsigned size = context.field(17, 2);
	if (size == 0U)
		return fatal();
	const unsigned bytes = esize_of(size);
	const std::uint32_t fpcr = context.cpu.fpcr;
	std::uint32_t flags = 0;
	const SveResult result = unary_merging(context, bytes, [&](Bits value) {
		return log_base_two(bytes, value, fpcr, flags);
	});
	context.cpu.fpsr |= flags;
	return result;
}

/* FRECPE and FRSQRTE (unpredicated). */
SveResult estimate(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const Unary operation = context.bit(16) ?
		Unary::reciprocal_sqrt_estimate :
		Unary::reciprocal_estimate;
	const Vector source = context.z(context.field(5, 5));
	Vector result;
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e)
			put(result, e, bytes,
				unary(operation, bytes, get(source, e, bytes)));
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* FEXPA: FPExpA() of every element; it raises nothing. */
SveResult exponential(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const Vector source = context.z(context.field(5, 5));
	Vector result;
	for (unsigned e = 0; e < vector_bytes / bytes; ++e)
		put(result, e, bytes,
			exponential_accelerator(bytes, get(source, e, bytes)));
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* FTSSEL: FPTrigSSel(); FPNeg() raises nothing. */
SveResult trig_select(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const Vector left = context.z(context.field(5, 5));
	const Vector right = context.z(context.field(16, 5));
	Vector result;
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e) {
			const Bits a = get(left, e, bytes);
			const Bits b = get(right, e, bytes);
			Bits r = a;
			if ((b & 1U) != 0U)
				r = one(bytes) |
					((b & 2U) != 0U ? sign_of(bytes) : 0U);
			else if ((b & 2U) != 0U)
				r = negate(bytes, a);
			put(result, e, bytes, r);
		}
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* FTMAD: FPTrigMAdd(), coefficient + Zdn * |Zm|. */
SveResult trig_multiply_add(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const unsigned immediate = context.field(16, 3);
	const unsigned dn = context.field(0, 5);
	const Vector left = context.z(dn);
	const Vector right = context.z(context.field(5, 5));
	Vector result;
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e) {
			const Bits a = get(left, e, bytes);
			const Bits b = get(right, e, bytes);
			const unsigned index = immediate +
				((b & sign_of(bytes)) != 0U ? 8U : 0U);
			put(result, e, bytes,
				fused(bytes, trig_coefficient(bytes, index), a,
					unary(Unary::absolute, bytes, b)));
		}
	});
	context.set_z(dn, result);
	return retired();
}

/* FCMEQ, FCMGT, FCMGE, FCMNE, FCMUO, FACGT and FACGE (vectors): a
 * zeroing predicate of the active elements' results. */
SveResult compare_vectors(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const unsigned kind = (context.bit(15) ? 4U : 0U) |
		(context.bit(13) ? 2U : 0U) | (context.bit(4) ? 1U : 0U);
	if (kind == 6U)
		return fatal();
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector left = context.z(context.field(5, 5));
	const Vector right = context.z(context.field(16, 5));
	Predicate result{};
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e) {
			if (!active(mask, e, bytes))
				continue;
			const Bits a = get(left, e, bytes);
			const Bits b = get(right, e, bytes);
			bool value;
			switch (kind) {
			case 0:
				value = binary(Binary::greater_equal, bytes, a,
						b) != 0U;
				break;
			case 1:
				value = binary(Binary::greater, bytes, a, b) !=
					0U;
				break;
			case 2:
				value = binary(Binary::equal, bytes, a, b) !=
					0U;
				break;
			case 3:
				value = binary(Binary::equal, bytes, a, b) ==
					0U;
				break;
			case 4:
				value = unordered(bytes, a, b);
				break;
			case 5:
				value = binary(Binary::absolute_greater_equal,
						bytes, a, b) != 0U;
				break;
			default:
				value = binary(Binary::absolute_greater, bytes,
						a, b) != 0U;
				break;
			}
			set_active(result, e, bytes, value);
		}
	});
	context.set_p(context.field(0, 4), result);
	return retired();
}

/* FCMEQ, FCMGT, FCMGE, FCMLT, FCMLE and FCMNE (zero). */
SveResult compare_zero(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const unsigned kind =
		context.field(16, 2) << 1U | (context.bit(4) ? 1U : 0U);
	if (kind == 5U || kind == 7U)
		return fatal();
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector source = context.z(context.field(5, 5));
	Predicate result{};
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e) {
			if (!active(mask, e, bytes))
				continue;
			const Bits a = get(source, e, bytes);
			bool value;
			switch (kind) {
			case 0:
				value = unary(Unary::greater_equal_zero, bytes,
						a) != 0U;
				break;
			case 1:
				value = unary(Unary::greater_zero, bytes, a) !=
					0U;
				break;
			case 2:
				value = unary(Unary::less_zero, bytes, a) != 0U;
				break;
			case 3:
				value = unary(Unary::less_equal_zero, bytes,
						a) != 0U;
				break;
			case 4:
				value = unary(Unary::equal_zero, bytes, a) !=
					0U;
				break;
			default:
				value = unary(Unary::equal_zero, bytes, a) ==
					0U;
				break;
			}
			set_active(result, e, bytes, value);
		}
	});
	context.set_p(context.field(0, 4), result);
	return retired();
}

/* FPReduce(): the halves reduced recursively, op(low, high). */
[[nodiscard]] Bits reduce(Binary operation, unsigned bytes,
	const std::array<Bits, 8> &values, unsigned first,
	unsigned count) noexcept
{
	if (count == 1U)
		return values[first];
	const unsigned half = count / 2U;
	const Bits high =
		reduce(operation, bytes, values, first + half, count - half);
	const Bits low = reduce(operation, bytes, values, first, half);
	return binary(operation, bytes, low, high);
}

/* FADDV, FMAXNMV, FMINNMV, FMAXV and FMINV: FPReducePredicated(), with
 * the operation's identity in each inactive element. */
SveResult reduce_vector(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const unsigned kind = context.field(16, 3);
	if (kind >= 1U && kind <= 3U)
		return fatal();
	Binary operation;
	Bits identity;
	switch (kind) {
	case 0:
		operation = Binary::add;
		identity = 0U;
		break;
	case 4:
		operation = Binary::maximum_number;
		identity = default_nan(bytes, context.cpu.fpcr);
		break;
	case 5:
		operation = Binary::minimum_number;
		identity = default_nan(bytes, context.cpu.fpcr);
		break;
	case 6:
		operation = Binary::maximum;
		identity = infinity(bytes, true);
		break;
	default:
		operation = Binary::minimum;
		identity = infinity(bytes, false);
		break;
	}
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector source = context.z(context.field(5, 5));
	const unsigned elements = vector_bytes / bytes;
	std::array<Bits, 8> values{};
	for (unsigned e = 0; e < elements; ++e)
		values[e] = active(mask, e, bytes) ? get(source, e, bytes) :
						     identity;
	Bits result = 0;
	under_guest_fpcr(context, [&] {
		result = reduce(operation, bytes, values, 0U, elements);
	});
	context.set_v_scalar(context.field(0, 5), result, bytes);
	return retired();
}

/* FADDA: the active elements added to Vdn in element order. */
SveResult add_strictly_ordered(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const Predicate &mask = context.p(context.field(10, 3));
	const unsigned dn = context.field(0, 5);
	const Vector source = context.z(context.field(5, 5));
	Bits result = get(context.z(dn), 0U, bytes);
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e)
			if (active(mask, e, bytes))
				result = binary(Binary::add, bytes, result,
					get(source, e, bytes));
	});
	context.set_v_scalar(dn, result, bytes);
	return retired();
}

/* FADDP, FMAXNMP, FMINNMP, FMAXP and FMINP: an active even element is
 * the pair of Zdn it starts, an odd one the pair of Zm it ends. */
SveResult pairwise(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const unsigned kind = context.field(16, 3);
	if (kind >= 1U && kind <= 3U)
		return fatal();
	const Binary operation = kind == 0U ? Binary::add :
		kind == 4U                  ? Binary::maximum_number :
		kind == 5U                  ? Binary::minimum_number :
		kind == 6U                  ? Binary::maximum :
					      Binary::minimum;
	const Predicate &mask = context.p(context.field(10, 3));
	const unsigned dn = context.field(0, 5);
	const Vector left = context.z(dn);
	const Vector right = context.z(context.field(5, 5));
	Vector result = left;
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e) {
			if (!active(mask, e, bytes))
				continue;
			const Vector &pair = e % 2U == 0U ? left : right;
			const unsigned base = e - e % 2U;
			put(result, e, bytes,
				binary(operation, bytes, get(pair, base, bytes),
					get(pair, base + 1U, bytes)));
		}
	});
	context.set_z(dn, result);
	return retired();
}

/* FMLA, FMLS, FNMLA and FNMLS (Zda += Zn * Zm), and FMAD, FMSB, FNMAD and
 * FNMSB (Zdn = Za + Zdn * Zm): FPMulAdd(op3, op1, op2) with op1 negated
 * when bit 14 differs from bit 13 and op3 negated when bit 14 is set. */
SveResult fused_predicated(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const bool multiplicand_form = context.bit(15);
	const bool negate_addend = context.bit(14);
	const bool negate_product = context.bit(14) != context.bit(13);
	const Predicate &mask = context.p(context.field(10, 3));
	const unsigned d = context.field(0, 5);
	const unsigned n = context.field(5, 5);
	const unsigned m = context.field(16, 5);
	/* op1, op2 and op3 of the pseudocode. */
	const Vector op1 = context.z(multiplicand_form ? d : n);
	const Vector op2 = context.z(multiplicand_form ? n : m);
	const Vector op3 = context.z(multiplicand_form ? m : d);
	Vector result = multiplicand_form ? op1 : op3;
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e) {
			if (!active(mask, e, bytes))
				continue;
			Bits a = get(op1, e, bytes);
			Bits c = get(op3, e, bytes);
			if (negate_product)
				a = negate(bytes, a);
			if (negate_addend)
				c = negate(bytes, c);
			put(result, e, bytes,
				fused(bytes, c, a, get(op2, e, bytes)));
		}
	});
	context.set_z(d, result);
	return retired();
}

/* The indexed element of FMLA, FMLS and FMUL (indexed): its size, its
 * index within each 128-bit segment, and Zm. */
struct IndexedOperand final {
	unsigned bytes;
	unsigned index;
	unsigned m;
};

[[nodiscard]] IndexedOperand indexed_operand(const Context &context) noexcept
{
	if (!context.bit(23))
		return {2U, context.field(22, 1) << 2U | context.field(19, 2),
			context.field(16, 3)};
	if (!context.bit(22))
		return {4U, context.field(19, 2), context.field(16, 3)};
	return {8U, context.field(20, 1), context.field(16, 4)};
}

/* FMLA and FMLS (indexed). */
SveResult fused_indexed(Context &context)
{
	const auto [bytes, index, m] = indexed_operand(context);
	const bool subtract = context.bit(10);
	const unsigned da = context.field(0, 5);
	const Vector left = context.z(context.field(5, 5));
	const Vector right = context.z(m);
	Vector result = context.z(da);
	const unsigned per_segment = 16U / bytes;
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e) {
			Bits a = get(left, e, bytes);
			if (subtract)
				a = negate(bytes, a);
			const Bits b =
				get(right, e - e % per_segment + index, bytes);
			put(result, e, bytes,
				fused(bytes, get(result, e, bytes), a, b));
		}
	});
	context.set_z(da, result);
	return retired();
}

/* FMUL (indexed). */
SveResult multiply_indexed(Context &context)
{
	const auto [bytes, index, m] = indexed_operand(context);
	const Vector left = context.z(context.field(5, 5));
	const Vector right = context.z(m);
	Vector result;
	const unsigned per_segment = 16U / bytes;
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e)
			put(result, e, bytes,
				binary(Binary::multiply, bytes,
					get(left, e, bytes),
					get(right, e - e % per_segment + index,
						bytes)));
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* FCADD: each active element of a pair adds the other part of Zm's pair,
 * the imaginary part negated for 90 degrees and the real part for 270. */
SveResult complex_add(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const bool rotate_270 = context.bit(16);
	const Predicate &mask = context.p(context.field(10, 3));
	const unsigned dn = context.field(0, 5);
	const Vector left = context.z(dn);
	const Vector right = context.z(context.field(5, 5));
	Vector result = left;
	under_guest_fpcr(context, [&] {
		for (unsigned p = 0; p < vector_bytes / bytes / 2U; ++p) {
			if (active(mask, 2U * p, bytes)) {
				Bits b = get(right, 2U * p + 1U, bytes);
				if (!rotate_270)
					b = negate(bytes, b);
				put(result, 2U * p, bytes,
					binary(Binary::add, bytes,
						get(left, 2U * p, bytes), b));
			}
			if (active(mask, 2U * p + 1U, bytes)) {
				Bits b = get(right, 2U * p, bytes);
				if (rotate_270)
					b = negate(bytes, b);
				put(result, 2U * p + 1U, bytes,
					binary(Binary::add, bytes,
						get(left, 2U * p + 1U, bytes),
						b));
			}
		}
	});
	context.set_z(dn, result);
	return retired();
}

/* One FCMLA pair: Zda's pair p plus Zn's element sel_a times Zm's pair
 * `pair`, rotated by `rotation`, for the parts `real` and `imaginary`
 * select. */
void complex_multiply_add_pair(Vector &result, const Vector &left,
	const Vector &right, unsigned bytes, unsigned p, unsigned pair,
	unsigned rotation, bool real, bool imaginary) noexcept
{
	const unsigned sel_a = rotation & 1U;
	const unsigned sel_b = sel_a ^ 1U;
	const bool negate_imaginary = (rotation & 2U) != 0U;
	const bool negate_real = (rotation & 1U) != (rotation >> 1U);
	const Bits a = get(left, 2U * p + sel_a, bytes);
	if (real) {
		Bits b = get(right, 2U * pair + sel_a, bytes);
		if (negate_real)
			b = negate(bytes, b);
		put(result, 2U * p, bytes,
			fused(bytes, get(result, 2U * p, bytes), a, b));
	}
	if (imaginary) {
		Bits b = get(right, 2U * pair + sel_b, bytes);
		if (negate_imaginary)
			b = negate(bytes, b);
		put(result, 2U * p + 1U, bytes,
			fused(bytes, get(result, 2U * p + 1U, bytes), a, b));
	}
}

/* FCMLA (vectors). */
SveResult complex_multiply_add(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const unsigned rotation = context.field(13, 2);
	const Predicate &mask = context.p(context.field(10, 3));
	const unsigned da = context.field(0, 5);
	const Vector left = context.z(context.field(5, 5));
	const Vector right = context.z(context.field(16, 5));
	Vector result = context.z(da);
	under_guest_fpcr(context, [&] {
		for (unsigned p = 0; p < vector_bytes / bytes / 2U; ++p)
			complex_multiply_add_pair(result, left, right, bytes, p,
				p, rotation, active(mask, 2U * p, bytes),
				active(mask, 2U * p + 1U, bytes));
	});
	context.set_z(da, result);
	return retired();
}

/* FCMLA (indexed). */
SveResult complex_multiply_add_indexed(Context &context)
{
	const bool half = !context.bit(22);
	const unsigned bytes = half ? 2U : 4U;
	const unsigned index =
		half ? context.field(19, 2) : context.field(20, 1);
	const unsigned m = half ? context.field(16, 3) : context.field(16, 4);
	const unsigned rotation = context.field(10, 2);
	const unsigned da = context.field(0, 5);
	const Vector left = context.z(context.field(5, 5));
	const Vector right = context.z(m);
	Vector result = context.z(da);
	const unsigned pairs_per_segment = 16U / (2U * bytes);
	under_guest_fpcr(context, [&] {
		for (unsigned p = 0; p < vector_bytes / bytes / 2U; ++p)
			complex_multiply_add_pair(result, left, right, bytes, p,
				p - p % pairs_per_segment + index, rotation,
				true, true);
	});
	context.set_z(da, result);
	return retired();
}

/* FCLAMP: FPMinNum(FPMaxNum(Zn, Zd), Zm). */
SveResult clamp(Context &context)
{
	const unsigned bytes = esize_of(context.field(22, 2));
	const unsigned d = context.field(0, 5);
	const Vector low = context.z(context.field(5, 5));
	const Vector high = context.z(context.field(16, 5));
	Vector result = context.z(d);
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / bytes; ++e) {
			const Bits bounded = binary(Binary::maximum_number,
				bytes, get(low, e, bytes),
				get(result, e, bytes));
			put(result, e, bytes,
				binary(Binary::minimum_number, bytes, bounded,
					get(high, e, bytes)));
		}
	});
	context.set_z(d, result);
	return retired();
}

/* FCVT between precisions, the element sizes by bits 23:16. */
SveResult convert_precision(Context &context)
{
	unsigned container;
	unsigned from;
	unsigned to;
	switch (context.field(16, 8)) {
	case 0x89:
		container = 4U, from = 2U, to = 4U;
		break;
	case 0xc9:
		container = 8U, from = 2U, to = 8U;
		break;
	case 0x88:
		container = 4U, from = 4U, to = 2U;
		break;
	case 0xcb:
		container = 8U, from = 4U, to = 8U;
		break;
	case 0xc8:
		container = 8U, from = 8U, to = 2U;
		break;
	case 0xca:
		container = 8U, from = 8U, to = 4U;
		break;
	default:
		return fatal();
	}
	return unary_merging(context, container, [&](Bits value) {
		return convert(from, to, value & mask_of(from));
	});
}

/* FCVTLT: each active element converts the top half of its own width. */
SveResult convert_long_top(Context &context)
{
	const unsigned container = context.bit(22) ? 8U : 4U;
	const unsigned half = container / 2U;
	const Predicate &mask = context.p(context.field(10, 3));
	const unsigned d = context.field(0, 5);
	const Vector source = context.z(context.field(5, 5));
	Vector result = context.z(d);
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / container; ++e)
			if (active(mask, e, container))
				put(result, e, container,
					convert(half, container,
						get(source, 2U * e + 1U,
							half)));
	});
	context.set_z(d, result);
	return retired();
}

/* FCVTNT, FCVTXNT and BFCVTNT: each active element's narrowed value goes
 * to the top half of its destination element, whose bottom half stays. */
template<typename Operation>
SveResult convert_narrow_top(
	Context &context, unsigned container, Operation &&operation)
{
	const unsigned half = container / 2U;
	const Predicate &mask = context.p(context.field(10, 3));
	const unsigned d = context.field(0, 5);
	const Vector source = context.z(context.field(5, 5));
	Vector result = context.z(d);
	under_guest_fpcr(context, [&] {
		for (unsigned e = 0; e < vector_bytes / container; ++e)
			if (active(mask, e, container))
				put(result, 2U * e + 1U, half,
					operation(get(source, e, container)));
	});
	context.set_z(d, result);
	return retired();
}

SveResult convert_narrow_top_form(Context &context)
{
	const unsigned container = context.bit(22) ? 8U : 4U;
	return convert_narrow_top(context, container, [&](Bits value) {
		return convert(container, container / 2U, value);
	});
}

/* FCVTX and FCVTXNT: FPConvert() of a double to a single, rounding to
 * odd. */
SveResult convert_odd_form(Context &context)
{
	return unary_merging(context, 8U, convert_odd);
}

SveResult convert_odd_top(Context &context)
{
	return convert_narrow_top(context, 8U, convert_odd);
}

/* FCVTZS and FCVTZU: the element sizes by bits 23:17, U by bit 16. */
SveResult convert_to_integer(Context &context)
{
	const bool is_unsigned = context.bit(16);
	unsigned container;
	unsigned from;
	unsigned to;
	switch (context.field(17, 7)) {
	case 0x2d:
		container = 2U, from = 2U, to = 2U;
		break;
	case 0x2e:
		container = 4U, from = 2U, to = 4U;
		break;
	case 0x2f:
		container = 8U, from = 2U, to = 8U;
		break;
	case 0x4e:
		container = 4U, from = 4U, to = 4U;
		break;
	case 0x6e:
		container = 8U, from = 4U, to = 8U;
		break;
	case 0x6c:
		container = 8U, from = 8U, to = 4U;
		break;
	case 0x6f:
		container = 8U, from = 8U, to = 8U;
		break;
	default:
		return fatal();
	}
	return unary_merging(context, container, [&](Bits value) {
		const Bits result = to_integer(is_unsigned, from, to, value);
		/* Extend(): a word result sign-extends into a doubleword
		 * element unless unsigned. */
		if (!is_unsigned && to == 4U && container == 8U)
			return static_cast<Bits>(static_cast<std::int64_t>(
				static_cast<std::int32_t>(result)));
		return result;
	});
}

/* SCVTF and UCVTF: the element sizes by bits 23:17, U by bit 16. */
SveResult convert_to_floating(Context &context)
{
	const bool is_unsigned = context.bit(16);
	unsigned container;
	unsigned from;
	unsigned to;
	switch (context.field(17, 7)) {
	case 0x29:
		container = 2U, from = 2U, to = 2U;
		break;
	case 0x2a:
		container = 4U, from = 4U, to = 2U;
		break;
	case 0x4a:
		container = 4U, from = 4U, to = 4U;
		break;
	case 0x68:
		container = 8U, from = 4U, to = 8U;
		break;
	case 0x2b:
		container = 8U, from = 8U, to = 2U;
		break;
	case 0x6a:
		container = 8U, from = 8U, to = 4U;
		break;
	case 0x6b:
		container = 8U, from = 8U, to = 8U;
		break;
	default:
		return fatal();
	}
	return unary_merging(context, container, [&](Bits value) {
		return to_floating(
			is_unsigned, from, to, value & mask_of(from));
	});
}

/*
 * FPConvertBF(): the host's BFCVT where a FEAT_AFP control may be set,
 * otherwise the shared model, whose flags go to `flags`. With the controls
 * clear FPUnpack() flushes a denormal input exactly under FZ, as the model
 * does.
 */
[[nodiscard]] Bits to_bfloat(Bits value, std::uint32_t fpcr, bool host,
	std::uint32_t &flags) noexcept
{
	if (host) {
		const auto x =
			std::bit_cast<float>(static_cast<std::uint32_t>(value));
		double result{};
		__asm__ volatile(".arch_extension bf16\n\t"
				 "bfcvt %h0, %s1\n\t"
				 ".arch_extension nobf16"
			: "=w"(result)
			: "w"(x));
		return std::bit_cast<Bits>(result) & mask_of(2U);
	}
	const auto converted =
		convert_to_bfloat(static_cast<std::uint32_t>(value), fpcr);
	flags |= converted.exceptions;
	return converted.bits;
}

/* BFCVT: the BFloat16 value zero-extended into its word element. */
SveResult convert_bfloat(Context &context)
{
	const std::uint32_t fpcr = context.cpu.fpcr;
	const bool host = afp_controls(context);
	std::uint32_t flags = 0;
	const SveResult result = unary_merging(context, 4U, [&](Bits value) {
		return to_bfloat(value, fpcr, host, flags);
	});
	context.cpu.fpsr |= flags;
	return result;
}

SveResult convert_bfloat_top(Context &context)
{
	const std::uint32_t fpcr = context.cpu.fpcr;
	const bool host = afp_controls(context);
	std::uint32_t flags = 0;
	const SveResult result =
		convert_narrow_top(context, 4U, [&](Bits value) {
			return to_bfloat(value, fpcr, host, flags);
		});
	context.cpu.fpsr |= flags;
	return result;
}

/* BFNeg(): with FPCR.AH set a NaN keeps its sign. */
[[nodiscard]] std::uint16_t negate_bfloat(
	std::uint16_t value, std::uint32_t fpcr) noexcept
{
	const bool nan = (value & 0x7f80U) == 0x7f80U && (value & 0x7fU) != 0U;
	if (nan && (fpcr & fpcr_ah) != 0U)
		return value;
	return static_cast<std::uint16_t>(value ^ 0x8000U);
}

/*
 * The widening multiply-adds of 16-bit elements into words: FMLALB,
 * FMLALT, FMLSLB and FMLSLT (FPMulAddH()) and BFMLALB, BFMLALT, BFMLSLB and
 * BFMLSLT (BFMulAddH()), by vector or by indexed element. Bit 22 selects
 * BFloat16, bit 13 subtracts, bit 10 takes the top halves.
 */
SveResult multiply_add_long(Context &context)
{
	const bool bfloat = context.bit(22);
	const bool subtract = context.bit(13);
	const unsigned top = context.bit(10) ? 1U : 0U;
	const bool indexed = !context.bit(15);
	const unsigned index =
		context.field(19, 2) << 1U | context.field(11, 1);
	const unsigned da = context.field(0, 5);
	const Vector left = context.z(context.field(5, 5));
	const Vector right = context.z(
		indexed ? context.field(16, 3) : context.field(16, 5));
	const std::uint32_t fpcr = context.cpu.fpcr;
	/* Each word element's two 16-bit operands. */
	Vector first;
	Vector second;
	for (unsigned e = 0; e < vector_bytes / 4U; ++e) {
		put(first, e, 2U, get(left, 2U * e + top, 2U));
		put(second, e, 2U,
			get(right,
				indexed ? 2U * (e - e % 4U) + index :
					  2U * e + top,
				2U));
	}
	Vector result = context.z(da);
	if (bfloat && !subtract && afp_controls(context)) {
		/* The host's BFMLALB of the gathered operands, which sit in
		 * the bottom halves. */
		Vector wide_first;
		Vector wide_second;
		for (unsigned e = 0; e < vector_bytes / 4U; ++e) {
			put(wide_first, 2U * e, 2U, get(first, e, 2U));
			put(wide_second, 2U * e, 2U, get(second, e, 2U));
		}
		Quad accumulator = quad_of(result);
		const Quad x = quad_of(wide_first);
		const Quad y = quad_of(wide_second);
		under_guest_fpcr(context, [&] {
			__asm__ volatile(".arch_extension bf16\n\t"
					 "bfmlalb %0.4s, %1.8h, %2.8h\n\t"
					 ".arch_extension nobf16"
				: "+w"(accumulator)
				: "w"(x), "w"(y));
		});
		result = vector_of(accumulator);
	} else if (!bfloat && afp_controls(context)) {
		/* The host's FMLAL or FMLSL of the gathered operands, which
		 * sit in the low four halves. */
		Quad accumulator = quad_of(result);
		const Quad x = quad_of(first);
		const Quad y = quad_of(second);
		under_guest_fpcr(context, [&] {
			if (subtract)
				__asm__ volatile(".arch_extension fp16fml\n\t"
						 "fmlsl %0.4s, %1.4h, %2.4h\n\t"
						 ".arch_extension nofp16fml"
					: "+w"(accumulator)
					: "w"(x), "w"(y));
			else
				__asm__ volatile(".arch_extension fp16fml\n\t"
						 "fmlal %0.4s, %1.4h, %2.4h\n\t"
						 ".arch_extension nofp16fml"
					: "+w"(accumulator)
					: "w"(x), "w"(y));
		});
		result = vector_of(accumulator);
	} else if (bfloat) {
		/* BFMulAddH() is FPMulAdd() of the exactly widened operands;
		 * under FPCR.AH it flushes denormals, rounds to nearest and
		 * raises nothing. */
		const bool alternative = (fpcr & fpcr_ah) != 0U;
		const std::uint32_t local = alternative ?
			((fpcr | fpcr_fiz | fpcr_fz) & ~fpcr_rounding) :
			fpcr;
		const std::uint32_t flags = raised_by(local, [&] {
			for (unsigned e = 0; e < vector_bytes / 4U; ++e) {
				auto a = static_cast<std::uint16_t>(
					get(first, e, 2U));
				if (subtract)
					a = negate_bfloat(a, fpcr);
				put(result, e, 4U,
					bfloat_multiply_add(
						static_cast<std::uint32_t>(
							get(result, e, 4U)),
						a,
						static_cast<std::uint16_t>(
							get(second, e, 2U))));
			}
		});
		if (!alternative)
			context.cpu.fpsr |= flags;
	} else {
		for (unsigned e = 0; e < vector_bytes / 4U; ++e) {
			const auto fused_long = half_fused_multiply_add(
				static_cast<std::uint32_t>(get(result, e, 4U)),
				static_cast<std::uint16_t>(get(first, e, 2U)),
				static_cast<std::uint16_t>(get(second, e, 2U)),
				subtract, fpcr);
			put(result, e, 4U, fused_long.bits);
			context.cpu.fpsr |= fused_long.exceptions &
				fpsr_exception_cumulative_mask;
		}
	}
	context.set_z(da, result);
	return retired();
}

/*
 * FPDotAdd(): addend + (a1 * a2 + b1 * b2) with the products summed exactly
 * and rounded once. Half-precision products are exact singles, so the host
 * adds them; FPProcessNaNs4() picks a NaN operand first.
 */
[[nodiscard]] std::uint32_t half_dot_add(std::uint32_t addend,
	const std::array<std::uint16_t, 4> &operands, std::uint32_t fpcr,
	std::uint32_t &flags) noexcept
{
	using floating_bits::is_nan;
	using floating_bits::is_signaling_nan;
	const auto selected = std::find_if(operands.begin(), operands.end(),
		is_signaling_nan<std::uint16_t>);
	const auto nan = selected != operands.end() ?
		selected :
		std::find_if(operands.begin(), operands.end(),
			is_nan<std::uint16_t>);
	Bits product;
	if (nan != operands.end()) {
		if (is_signaling_nan(*nan))
			flags |= fpsr_invalid;
		product = (fpcr & fpcr_dn) != 0U ?
			default_nan(4U, fpcr) :
			(Bits{*nan} & 0x8000U) << 16U | 0x7fc0'0000U |
				(Bits{*nan} & 0x1ffU) << 13U;
	} else {
		using half_fused_long_detail::expand_half;
		const Bits a = binary(Binary::multiply, 4U,
			expand_half(operands[0], fpcr),
			expand_half(operands[2], fpcr));
		const Bits b = binary(Binary::multiply, 4U,
			expand_half(operands[1], fpcr),
			expand_half(operands[3], fpcr));
		product = binary(Binary::add, 4U, a, b);
	}
	return static_cast<std::uint32_t>(
		binary(Binary::add, 4U, addend, product));
}

/* BFDOT and the half-precision FDOT, by vector or by indexed pair. */
SveResult dot_pair(Context &context)
{
	const bool bfloat = context.bit(22);
	const bool indexed = !context.bit(15);
	const unsigned index = context.field(19, 2);
	const unsigned da = context.field(0, 5);
	const Vector left = context.z(context.field(5, 5));
	const Vector right = context.z(
		indexed ? context.field(16, 3) : context.field(16, 5));
	const std::uint32_t fpcr = context.cpu.fpcr;
	Vector pairs;
	for (unsigned e = 0; e < vector_bytes / 4U; ++e)
		put(pairs, e, 4U,
			get(right, indexed ? e - e % 4U + index : e, 4U));
	Vector result = context.z(da);
	if (bfloat && afp_controls(context)) {
		Quad accumulator = quad_of(result);
		const Quad x = quad_of(left);
		const Quad y = quad_of(pairs);
		under_guest_fpcr(context, [&] {
			__asm__ volatile(".arch_extension bf16\n\t"
					 "bfdot %0.4s, %1.8h, %2.8h\n\t"
					 ".arch_extension nobf16"
				: "+w"(accumulator)
				: "w"(x), "w"(y));
		});
		result = vector_of(accumulator);
	} else if (bfloat) {
		for (unsigned e = 0; e < vector_bytes / 4U; ++e)
			put(result, e, 4U,
				bfloat_dot_add(static_cast<std::uint32_t>(
						       get(result, e, 4U)),
					static_cast<std::uint16_t>(
						get(left, 2U * e, 2U)),
					static_cast<std::uint16_t>(
						get(left, 2U * e + 1U, 2U)),
					static_cast<std::uint16_t>(
						get(pairs, 2U * e, 2U)),
					static_cast<std::uint16_t>(
						get(pairs, 2U * e + 1U, 2U))));
	} else {
		std::uint32_t flags = 0;
		under_guest_fpcr(context, [&] {
			for (unsigned e = 0; e < vector_bytes / 4U; ++e) {
				const std::array operands{
					static_cast<std::uint16_t>(
						get(left, 2U * e, 2U)),
					static_cast<std::uint16_t>(
						get(left, 2U * e + 1U, 2U)),
					static_cast<std::uint16_t>(
						get(pairs, 2U * e, 2U)),
					static_cast<std::uint16_t>(
						get(pairs, 2U * e + 1U, 2U))};
				put(result, e, 4U,
					half_dot_add(
						static_cast<std::uint32_t>(
							get(result, e, 4U)),
						operands, fpcr, flags));
			}
		});
		context.cpu.fpsr |= flags;
	}
	context.set_z(da, result);
	return retired();
}

/* BFMMLA: BFMatMulAddH() of the one 128-bit segment. */
SveResult bfloat_matrix(Context &context)
{
	const unsigned da = context.field(0, 5);
	const Vector left = context.z(context.field(5, 5));
	const Vector right = context.z(context.field(16, 5));
	Vector result = context.z(da);
	if (afp_controls(context)) {
		Quad accumulator = quad_of(result);
		const Quad x = quad_of(left);
		const Quad y = quad_of(right);
		under_guest_fpcr(context, [&] {
			__asm__ volatile(".arch_extension bf16\n\t"
					 "bfmmla %0.4s, %1.8h, %2.8h\n\t"
					 ".arch_extension nobf16"
				: "+w"(accumulator)
				: "w"(x), "w"(y));
		});
		result = vector_of(accumulator);
	} else {
		const auto half = [](const Vector &vector, unsigned index) {
			return static_cast<std::uint16_t>(
				get(vector, index, 2U));
		};
		for (unsigned i = 0; i < 2U; ++i)
			for (unsigned j = 0; j < 2U; ++j) {
				auto sum = static_cast<std::uint32_t>(
					get(result, 2U * i + j, 4U));
				for (unsigned k = 0; k < 2U; ++k)
					sum = bfloat_dot_add(sum,
						half(left, 4U * i + 2U * k),
						half(left,
							4U * i + 2U * k + 1U),
						half(right, 4U * j + 2U * k),
						half(right,
							4U * j + 2U * k + 1U));
				put(result, 2U * i + j, 4U, sum);
			}
	}
	context.set_z(da, result);
	return retired();
}

} // namespace

Handler floating_handler(std::uint16_t id) noexcept
{
	switch (id) {
	case form::fadd_z_p_zz:
	case form::fsub_z_p_zz:
	case form::fmul_z_p_zz:
	case form::fsubr_z_p_zz:
	case form::fmaxnm_z_p_zz:
	case form::fminnm_z_p_zz:
	case form::fmax_z_p_zz:
	case form::fmin_z_p_zz:
	case form::fabd_z_p_zz:
	case form::fscale_z_p_zz:
	case form::fmulx_z_p_zz:
	case form::fdivr_z_p_zz:
	case form::fdiv_z_p_zz:
		return binary_predicated;
	case form::fadd_z_p_zs:
	case form::fsub_z_p_zs:
	case form::fmul_z_p_zs:
	case form::fsubr_z_p_zs:
	case form::fmaxnm_z_p_zs:
	case form::fminnm_z_p_zs:
	case form::fmax_z_p_zs:
	case form::fmin_z_p_zs:
		return binary_immediate;
	case form::fadd_z_zz:
	case form::fsub_z_zz:
	case form::fmul_z_zz:
	case form::ftsmul_z_zz:
	case form::frecps_z_zz:
	case form::frsqrts_z_zz:
		return binary_unpredicated;
	case form::fabs_z_p_z_m:
	case form::fneg_z_p_z_m:
		return absolute_or_negate;
	case form::frecpx_z_p_z_m:
		return reciprocal_exponent;
	case form::fsqrt_z_p_z_m:
		return square_root;
	case form::frintx_z_p_z_m:
		return round_to_integral;
	case form::flogb_z_p_z_m:
		return log_base_two_form;
	case form::frecpe_z_z:
	case form::frsqrte_z_z:
		return estimate;
	case form::fexpa_z_z:
		return exponential;
	case form::ftssel_z_zz:
		return trig_select;
	case form::ftmad_z_zzi:
		return trig_multiply_add;
	case form::fcmeq_p_p_zz:
	case form::facgt_p_p_zz:
		return compare_vectors;
	case form::fcmeq_p_p_z0:
		return compare_zero;
	case form::faddv_v_p_z:
	case form::fmaxnmv_v_p_z:
	case form::fminnmv_v_p_z:
	case form::fmaxv_v_p_z:
	case form::fminv_v_p_z:
		return reduce_vector;
	case form::fadda_v_p_z:
		return add_strictly_ordered;
	case form::faddp_z_p_zz:
	case form::fmaxnmp_z_p_zz:
	case form::fminnmp_z_p_zz:
	case form::fmaxp_z_p_zz:
	case form::fminp_z_p_zz:
		return pairwise;
	case form::fmla_z_p_zzz:
	case form::fmls_z_p_zzz:
	case form::fnmla_z_p_zzz:
	case form::fnmls_z_p_zzz:
	case form::fmad_z_p_zzz:
	case form::fmsb_z_p_zzz:
	case form::fnmad_z_p_zzz:
	case form::fnmsb_z_p_zzz:
		return fused_predicated;
	case form::fmla_z_zzzi_h:
	case form::fmls_z_zzzi_h:
		return fused_indexed;
	case form::fmul_z_zzi_h:
		return multiply_indexed;
	case form::fcadd_z_p_zz:
		return complex_add;
	case form::fcmla_z_p_zzz:
		return complex_multiply_add;
	case form::fcmla_z_zzzi_h:
		return complex_multiply_add_indexed;
	case form::fclamp_z_zz:
		return clamp;
	case form::fcvt_z_p_z_h2s:
		return convert_precision;
	case form::fcvtlt_z_p_z_h2s:
		return convert_long_top;
	case form::fcvtnt_z_p_z_s2h:
		return convert_narrow_top_form;
	case form::fcvtx_z_p_z_d2s:
		return convert_odd_form;
	case form::fcvtxnt_z_p_z_d2s:
		return convert_odd_top;
	case form::fcvtzs_z_p_z_fp162h:
	case form::fcvtzu_z_p_z_fp162h:
		return convert_to_integer;
	case form::scvtf_z_p_z_h2fp16:
	case form::ucvtf_z_p_z_h2fp16:
		return convert_to_floating;
	case form::bfcvt_z_p_z_s2bf:
		return convert_bfloat;
	case form::bfcvtnt_z_p_z_s2bf:
		return convert_bfloat_top;
	case form::fmlalb_z_zzz:
	case form::fmlalt_z_zzz:
	case form::fmlslb_z_zzz:
	case form::fmlslt_z_zzz:
	case form::fmlalb_z_zzzi_s:
	case form::fmlalt_z_zzzi_s:
	case form::fmlslb_z_zzzi_s:
	case form::fmlslt_z_zzzi_s:
	case form::bfmlalb_z_zzz:
	case form::bfmlalt_z_zzz:
	case form::bfmlslb_z_zzz:
	case form::bfmlslt_z_zzz:
	case form::bfmlalb_z_zzzi:
	case form::bfmlalt_z_zzzi:
	case form::bfmlslb_z_zzzi:
	case form::bfmlslt_z_zzzi:
		return multiply_add_long;
	case form::bfdot_z_zzz:
	case form::bfdot_z_zzzi:
	case form::fdot_z_zzz:
	case form::fdot_z_zzzi:
		return dot_pair;
	case form::bfmmla_z_zzz:
		return bfloat_matrix;
	default:
		return nullptr;
	}
}

} // namespace uml::cpu::arm64::detail::sve_semantics
