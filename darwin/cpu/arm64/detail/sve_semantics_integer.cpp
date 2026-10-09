// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The semantic library's integer forms that keep their element size:
 * arithmetic, logical and shift operations, predicated and not, with
 * vectors or immediates; multiply-add; compares; reductions; and the SVE2
 * forms of the same shape (halving, saturating, rounding and complex
 * additions, the bitwise ternaries, histograms, matches and carries).
 */
#include "sve_semantics_internal.hpp"

namespace uml::cpu::arm64::detail::sve_semantics {

namespace {

enum class Binary : std::uint8_t {
	add,
	sub,
	subr,
	smax,
	umax,
	smin,
	umin,
	sabd,
	uabd,
	mul,
	smulh,
	umulh,
	sdiv,
	udiv,
	sdivr,
	udivr,
	orr,
	eor,
	and_,
	bic,
	asr,
	lsr,
	lsl,
	asrr,
	lsrr,
	lslr,
	shadd,
	uhadd,
	shsub,
	uhsub,
	shsubr,
	uhsubr,
	srhadd,
	urhadd,
	sqadd,
	uqadd,
	sqsub,
	uqsub,
	sqsubr,
	uqsubr,
	suqadd,
	usqadd,
	sqshl,
	uqshl,
	sqrshl,
	uqrshl,
	srshl,
	urshl,
	sqshlr,
	uqshlr,
	sqrshlr,
	uqrshlr,
	srshlr,
	urshlr,
	sqdmulh,
	sqrdmulh,
	pmul,
};

/* ShiftSat(): a shift amount clamped to plus or minus (esize + 1). */
[[nodiscard]] constexpr std::int64_t clamp_shift(
	std::int64_t shift, unsigned esize) noexcept
{
	const auto limit = static_cast<std::int64_t>(esize) + 1;
	return shift > limit ? limit : shift < -limit ? -limit : shift;
}

/*
 * An element shifted left by a signed amount, or right by its negation,
 * as the SVE2 shift-by-vector forms do: optionally rounding a right shift,
 * and saturating the result (signed or unsigned per `saturate_signed`), or
 * keeping its low bits.
 */
template<typename T>
[[nodiscard]] T shift_by(T element, std::int64_t shift_amount, bool is_signed,
	bool rounding, bool saturating, bool saturate_as_signed) noexcept
{
	const std::int64_t shift = clamp_shift(shift_amount, bits_of<T>);
	const __int128 value = value_of(element, is_signed);
	__int128 result;
	if (shift >= 0) {
		if (value == 0)
			return 0;
		if (shift >= bits_of<T>) {
			if (!saturating)
				return 0;
			return saturate<T>(value > 0 ? (__int128{1} << 100) :
						       -(__int128{1} << 100),
				saturate_as_signed);
		}
		result = value << shift;
	} else {
		const std::int64_t right = -shift;
		result = rounding ?
			(value + (__int128{1} << (right - 1))) >> right :
			value >> right;
	}
	return saturating ? saturate<T>(result, saturate_as_signed) :
			    static_cast<T>(result);
}

template<typename T>
[[nodiscard]] T multiply_high(T left, T right, bool is_signed) noexcept
{
	const __int128 product =
		value_of(left, is_signed) * value_of(right, is_signed);
	return static_cast<T>(product >> bits_of<T>);
}

/* SQDMULH and SQRDMULH: the high half of twice the product, saturated. */
template<typename T>
[[nodiscard]] T doubling_multiply_high(T left, T right, bool rounding) noexcept
{
	if constexpr (sizeof(T) == 8) {
		const __int128 product =
			static_cast<__int128>(to_signed(left)) *
			to_signed(right);
		/* 2 * product + round, shifted right by 64, without
		 * overflowing: only -2^63 * -2^63 saturates. */
		if (to_signed(left) ==
				std::numeric_limits<std::int64_t>::min() &&
			to_signed(right) ==
				std::numeric_limits<std::int64_t>::min())
			return static_cast<T>(
				std::numeric_limits<std::int64_t>::max());
		const __int128 rounded =
			product + (rounding ? (__int128{1} << 62) : 0);
		return static_cast<T>(static_cast<std::int64_t>(rounded >> 63));
	} else {
		const __int128 product = 2 *
				static_cast<__int128>(to_signed(left)) *
				to_signed(right) +
			(rounding ? (__int128{1} << (bits_of<T> - 1)) : 0);
		return saturate_signed<T>(product >> bits_of<T>);
	}
}

/* Polynomial multiply, the low half of the carry-less product. */
template<typename T>
[[nodiscard]] T polynomial_multiply(T left, T right) noexcept
{
	T result = 0;
	for (unsigned bit = 0; bit < bits_of<T>; ++bit)
		if (((right >> bit) & 1U) != 0U)
			result = static_cast<T>(result ^ (left << bit));
	return result;
}

template<typename T>
[[nodiscard]] T apply(Binary op, T a, T b) noexcept
{
	using S = std::make_signed_t<T>;
	constexpr unsigned esize = bits_of<T>;
	const auto sa = static_cast<__int128>(static_cast<S>(a));
	const auto sb = static_cast<__int128>(static_cast<S>(b));
	const auto ua = static_cast<__int128>(a);
	const auto ub = static_cast<__int128>(b);
	switch (op) {
	case Binary::add:
		return static_cast<T>(a + b);
	case Binary::sub:
		return static_cast<T>(a - b);
	case Binary::subr:
		return static_cast<T>(b - a);
	case Binary::smax:
		return sa > sb ? a : b;
	case Binary::umax:
		return a > b ? a : b;
	case Binary::smin:
		return sa < sb ? a : b;
	case Binary::umin:
		return a < b ? a : b;
	case Binary::sabd:
		return static_cast<T>(sa > sb ? sa - sb : sb - sa);
	case Binary::uabd:
		return static_cast<T>(a > b ? a - b : b - a);
	case Binary::mul:
		return static_cast<T>(ua * ub);
	case Binary::smulh:
		return multiply_high(a, b, true);
	case Binary::umulh:
		return multiply_high(a, b, false);
	case Binary::sdiv:
	case Binary::sdivr: {
		const __int128 dividend = op == Binary::sdiv ? sa : sb;
		const __int128 divisor = op == Binary::sdiv ? sb : sa;
		return divisor == 0 ? T{0} : static_cast<T>(dividend / divisor);
	}
	case Binary::udiv:
	case Binary::udivr: {
		const T dividend = op == Binary::udiv ? a : b;
		const T divisor = op == Binary::udiv ? b : a;
		return divisor == 0 ? T{0} : static_cast<T>(dividend / divisor);
	}
	case Binary::orr:
		return static_cast<T>(a | b);
	case Binary::eor:
		return static_cast<T>(a ^ b);
	case Binary::and_:
		return static_cast<T>(a & b);
	case Binary::bic:
		return static_cast<T>(a & ~b);
	case Binary::asr:
	case Binary::asrr: {
		const T value = op == Binary::asr ? a : b;
		const T amount = op == Binary::asr ? b : a;
		const unsigned shift = amount >= esize ?
			esize - 1U :
			static_cast<unsigned>(amount);
		return static_cast<T>(static_cast<S>(value) >> shift);
	}
	case Binary::lsr:
	case Binary::lsrr: {
		const T value = op == Binary::lsr ? a : b;
		const T amount = op == Binary::lsr ? b : a;
		return amount >= esize ? T{0} : static_cast<T>(value >> amount);
	}
	case Binary::lsl:
	case Binary::lslr: {
		const T value = op == Binary::lsl ? a : b;
		const T amount = op == Binary::lsl ? b : a;
		return amount >= esize ? T{0} : static_cast<T>(value << amount);
	}
	case Binary::shadd:
		return static_cast<T>((sa + sb) >> 1);
	case Binary::uhadd:
		return static_cast<T>((ua + ub) >> 1);
	case Binary::shsub:
		return static_cast<T>((sa - sb) >> 1);
	case Binary::uhsub:
		return static_cast<T>((ua - ub) >> 1);
	case Binary::shsubr:
		return static_cast<T>((sb - sa) >> 1);
	case Binary::uhsubr:
		return static_cast<T>((ub - ua) >> 1);
	case Binary::srhadd:
		return static_cast<T>((sa + sb + 1) >> 1);
	case Binary::urhadd:
		return static_cast<T>((ua + ub + 1) >> 1);
	case Binary::sqadd:
		return saturate_signed<T>(sa + sb);
	case Binary::uqadd:
		return saturate_unsigned<T>(ua + ub);
	case Binary::sqsub:
		return saturate_signed<T>(sa - sb);
	case Binary::uqsub:
		return saturate_unsigned<T>(ua - ub);
	case Binary::sqsubr:
		return saturate_signed<T>(sb - sa);
	case Binary::uqsubr:
		return saturate_unsigned<T>(ub - ua);
	case Binary::suqadd:
		return saturate_signed<T>(sa + ub);
	case Binary::usqadd:
		return saturate_unsigned<T>(ua + sb);
	case Binary::sqshl:
		return shift_by(a, static_cast<std::int64_t>(sb), true, false,
			true, true);
	case Binary::uqshl:
		return shift_by(a, static_cast<std::int64_t>(sb), false, false,
			true, false);
	case Binary::sqrshl:
		return shift_by(a, static_cast<std::int64_t>(sb), true, true,
			true, true);
	case Binary::uqrshl:
		return shift_by(a, static_cast<std::int64_t>(sb), false, true,
			true, false);
	case Binary::srshl:
		return shift_by(a, static_cast<std::int64_t>(sb), true, true,
			false, true);
	case Binary::urshl:
		return shift_by(a, static_cast<std::int64_t>(sb), false, true,
			false, false);
	case Binary::sqshlr:
		return shift_by(b, static_cast<std::int64_t>(sa), true, false,
			true, true);
	case Binary::uqshlr:
		return shift_by(b, static_cast<std::int64_t>(sa), false, false,
			true, false);
	case Binary::sqrshlr:
		return shift_by(b, static_cast<std::int64_t>(sa), true, true,
			true, true);
	case Binary::uqrshlr:
		return shift_by(b, static_cast<std::int64_t>(sa), false, true,
			true, false);
	case Binary::srshlr:
		return shift_by(b, static_cast<std::int64_t>(sa), true, true,
			false, true);
	case Binary::urshlr:
		return shift_by(b, static_cast<std::int64_t>(sa), false, true,
			false, false);
	case Binary::sqdmulh:
		return doubling_multiply_high(a, b, false);
	case Binary::sqrdmulh:
		return doubling_multiply_high(a, b, true);
	case Binary::pmul:
		return polynomial_multiply(a, b);
	}
	return 0;
}

[[nodiscard]] Binary predicated_binary_op(std::uint16_t id) noexcept
{
	switch (id) {
	case form::add_z_p_zz:
		return Binary::add;
	case form::sub_z_p_zz:
		return Binary::sub;
	case form::subr_z_p_zz:
		return Binary::subr;
	case form::smax_z_p_zz:
		return Binary::smax;
	case form::umax_z_p_zz:
		return Binary::umax;
	case form::smin_z_p_zz:
		return Binary::smin;
	case form::umin_z_p_zz:
		return Binary::umin;
	case form::sabd_z_p_zz:
		return Binary::sabd;
	case form::uabd_z_p_zz:
		return Binary::uabd;
	case form::mul_z_p_zz:
		return Binary::mul;
	case form::smulh_z_p_zz:
		return Binary::smulh;
	case form::umulh_z_p_zz:
		return Binary::umulh;
	case form::sdiv_z_p_zz:
		return Binary::sdiv;
	case form::udiv_z_p_zz:
		return Binary::udiv;
	case form::sdivr_z_p_zz:
		return Binary::sdivr;
	case form::udivr_z_p_zz:
		return Binary::udivr;
	case form::orr_z_p_zz:
		return Binary::orr;
	case form::eor_z_p_zz:
		return Binary::eor;
	case form::and_z_p_zz:
		return Binary::and_;
	case form::bic_z_p_zz:
		return Binary::bic;
	case form::asr_z_p_zz:
		return Binary::asr;
	case form::lsr_z_p_zz:
		return Binary::lsr;
	case form::lsl_z_p_zz:
		return Binary::lsl;
	case form::asrr_z_p_zz:
		return Binary::asrr;
	case form::lsrr_z_p_zz:
		return Binary::lsrr;
	case form::lslr_z_p_zz:
		return Binary::lslr;
	case form::shadd_z_p_zz:
		return Binary::shadd;
	case form::uhadd_z_p_zz:
		return Binary::uhadd;
	case form::shsub_z_p_zz:
		return Binary::shsub;
	case form::uhsub_z_p_zz:
		return Binary::uhsub;
	case form::shsubr_z_p_zz:
		return Binary::shsubr;
	case form::uhsubr_z_p_zz:
		return Binary::uhsubr;
	case form::srhadd_z_p_zz:
		return Binary::srhadd;
	case form::urhadd_z_p_zz:
		return Binary::urhadd;
	case form::sqadd_z_p_zz:
		return Binary::sqadd;
	case form::uqadd_z_p_zz:
		return Binary::uqadd;
	case form::sqsub_z_p_zz:
		return Binary::sqsub;
	case form::uqsub_z_p_zz:
		return Binary::uqsub;
	case form::sqsubr_z_p_zz:
		return Binary::sqsubr;
	case form::uqsubr_z_p_zz:
		return Binary::uqsubr;
	case form::suqadd_z_p_zz:
		return Binary::suqadd;
	case form::usqadd_z_p_zz:
		return Binary::usqadd;
	case form::sqshl_z_p_zz:
		return Binary::sqshl;
	case form::uqshl_z_p_zz:
		return Binary::uqshl;
	case form::sqrshl_z_p_zz:
		return Binary::sqrshl;
	case form::uqrshl_z_p_zz:
		return Binary::uqrshl;
	case form::srshl_z_p_zz:
		return Binary::srshl;
	case form::urshl_z_p_zz:
		return Binary::urshl;
	case form::sqshlr_z_p_zz:
		return Binary::sqshlr;
	case form::uqshlr_z_p_zz:
		return Binary::uqshlr;
	case form::sqrshlr_z_p_zz:
		return Binary::sqrshlr;
	case form::uqrshlr_z_p_zz:
		return Binary::uqrshlr;
	case form::srshlr_z_p_zz:
		return Binary::srshlr;
	default:
		return Binary::urshlr;
	}
}

/* The destructive predicated forms <op> Zdn, Pg/M, Zdn, Zm. */
SveResult binary_predicated(Context &context)
{
	const Binary op = predicated_binary_op(context.row.form);
	const unsigned size = context.field(22, 2);
	const unsigned dn = context.field(0, 5);
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector m = context.z(context.field(5, 5));
	Vector result = context.z(dn);
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>; ++element)
			if (active(mask, element, sizeof(T)))
				result.set<T>(element,
					apply(op, result.get<T>(element),
						m.get<T>(element)));
	});
	context.set_z(dn, result);
	return retired();
}

/* ASR, LSR and LSL (wide elements, predicated): each element shifted by
 * the 64-bit element of Zm that overlaps it. */
SveResult shift_wide_predicated(Context &context)
{
	const Binary op = context.row.form == form::asr_z_p_zw ? Binary::asr :
		context.row.form == form::lsr_z_p_zw           ? Binary::lsr :
								 Binary::lsl;
	const unsigned size = context.field(22, 2);
	const unsigned dn = context.field(0, 5);
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector m = context.z(context.field(5, 5));
	Vector result = context.z(dn);
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			if (!active(mask, element, sizeof(T)))
				continue;
			const std::uint64_t amount =
				m.get<std::uint64_t>(element * sizeof(T) / 8U);
			const T clamped = amount >= bits_of<T> ?
				static_cast<T>(bits_of<T>) :
				static_cast<T>(amount);
			result.set<T>(element,
				apply(op, result.get<T>(element), clamped));
		}
	});
	context.set_z(dn, result);
	return retired();
}

/* ASR, LSR and LSL (wide elements, unpredicated): opc in bits 11:10. */
SveResult shift_wide(Context &context)
{
	const unsigned opc = context.field(10, 2);
	const Binary op = opc == 0U ? Binary::asr :
		opc == 1U           ? Binary::lsr :
				      Binary::lsl;
	const unsigned size = context.field(22, 2);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result;
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			const std::uint64_t amount =
				m.get<std::uint64_t>(element * sizeof(T) / 8U);
			const T clamped = amount >= bits_of<T> ?
				static_cast<T>(bits_of<T>) :
				static_cast<T>(amount);
			result.set<T>(
				element, apply(op, n.get<T>(element), clamped));
		}
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* The unpredicated forms <op> Zd, Zn, Zm. */
template<Binary Op>
SveResult binary_unpredicated(Context &context)
{
	const unsigned size = Op == Binary::and_ || Op == Binary::orr ||
			Op == Binary::eor || Op == Binary::bic ?
		3U :
		context.field(22, 2);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result;
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>; ++element)
			result.set<T>(element,
				apply(Op, n.get<T>(element),
					m.get<T>(element)));
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* ADD, SUB, SUBR, SQADD, UQADD, SQSUB and UQSUB (immediate): an unsigned
 * imm8 in bits 12:5, shifted left 8 when sh (bit 13) is set. */
template<Binary Op>
SveResult binary_immediate_shifted(Context &context)
{
	const unsigned size = context.field(22, 2);
	const unsigned dn = context.field(0, 5);
	const std::uint64_t immediate = std::uint64_t{context.field(5, 8)}
		<< (context.bit(13) ? 8U : 0U);
	Vector result = context.z(dn);
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			const T value = result.get<T>(element);
			/* The immediate is unsigned: SQADD and SQSUB saturate
			 * the signed element plus or minus its full value. */
			if constexpr (Op == Binary::sqadd)
				result.set<T>(element,
					saturate_signed<T>(to_signed(value) +
						static_cast<__int128>(
							immediate)));
			else if constexpr (Op == Binary::sqsub)
				result.set<T>(element,
					saturate_signed<T>(to_signed(value) -
						static_cast<__int128>(
							immediate)));
			else
				result.set<T>(element,
					apply(Op, value,
						static_cast<T>(immediate)));
		}
	});
	context.set_z(dn, result);
	return retired();
}

/* SMAX, SMIN, MUL (signed imm8) and UMAX, UMIN (unsigned imm8), with the
 * immediate in bits 12:5. */
template<Binary Op>
SveResult binary_immediate(Context &context)
{
	const unsigned size = context.field(22, 2);
	const unsigned dn = context.field(0, 5);
	const bool is_signed = Op != Binary::umax && Op != Binary::umin;
	const std::uint64_t immediate = is_signed ?
		static_cast<std::uint64_t>(
			sign_extend(context.field(5, 8), 8)) :
		context.field(5, 8);
	Vector result = context.z(dn);
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>; ++element)
			result.set<T>(element,
				apply(Op, result.get<T>(element),
					static_cast<T>(immediate)));
	});
	context.set_z(dn, result);
	return retired();
}

/* AND, ORR and EOR (immediate): opc in bits 23:22. */
SveResult logical_immediate_form(Context &context)
{
	const auto immediate = logical_immediate_of(context.word);
	if (!immediate.has_value())
		return undefined();
	const unsigned dn = context.field(0, 5);
	Vector result = context.z(dn);
	const unsigned opc = context.field(22, 2);
	for (unsigned element = 0; element < 2U; ++element) {
		const std::uint64_t value = result.get<std::uint64_t>(element);
		result.set<std::uint64_t>(element,
			opc == 0U         ? value | *immediate :
				opc == 1U ? value ^ *immediate :
					    value & *immediate);
	}
	context.set_z(dn, result);
	return retired();
}

SveResult duplicate_mask_immediate(Context &context)
{
	const auto immediate = logical_immediate_of(context.word);
	if (!immediate.has_value())
		return undefined();
	Vector result;
	result.set<std::uint64_t>(0, *immediate);
	result.set<std::uint64_t>(1, *immediate);
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* The element size and shift amount of a tsize:imm3 shift immediate,
 * tsize being the four bits tszh:tszl: right shifts count down from
 * 2 * esize, left shifts up from esize. */
struct ShiftImmediate final {
	unsigned size;
	unsigned shift;
};

[[nodiscard]] ShiftImmediate shift_immediate(
	unsigned tsize, unsigned imm3, bool left) noexcept
{
	const unsigned size = static_cast<unsigned>(std::bit_width(tsize)) - 1U;
	const unsigned esize = 8U << size;
	const unsigned value = tsize << 3U | imm3;
	return {size, left ? value - esize : 2U * esize - value};
}

/* ASR, LSR and LSL (immediate, unpredicated): tszh in bits 23:22, tszl in
 * 20:19, imm3 in 18:16, opc in 11:10. */
SveResult shift_immediate_unpredicated(Context &context)
{
	const unsigned opc = context.field(10, 2);
	const ShiftImmediate immediate = shift_immediate(
		context.field(22, 2) << 2U | context.field(19, 2),
		context.field(16, 3), opc == 3U);
	const Vector n = context.z(context.field(5, 5));
	Vector result;
	with_size(immediate.size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			const T value = n.get<T>(element);
			const auto shift = static_cast<T>(immediate.shift);
			result.set<T>(element,
				opc == 0U ? apply(Binary::asr, value, shift) :
					opc == 1U ?
					    apply(Binary::lsr, value, shift) :
					    apply(Binary::lsl, value, shift));
		}
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* ASR, LSR, LSL, ASRD, SQSHL, UQSHL, SRSHR, URSHR and SQSHLU (immediate,
 * predicated): tszh in bits 23:22, opc in 19:16, tszl in 9:8, imm3 in 7:5. */
SveResult shift_immediate_predicated(Context &context)
{
	const unsigned opc = context.field(16, 4);
	const bool left = opc == 3U || opc == 6U || opc == 7U || opc == 15U;
	const ShiftImmediate immediate = shift_immediate(
		context.field(22, 2) << 2U | context.field(8, 2),
		context.field(5, 3), left);
	const unsigned dn = context.field(0, 5);
	const Predicate &mask = context.p(context.field(10, 3));
	Vector result = context.z(dn);
	with_size(immediate.size, [&]<typename T>() {
		const auto shift = static_cast<std::int64_t>(immediate.shift);
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			if (!active(mask, element, sizeof(T)))
				continue;
			const T value = result.get<T>(element);
			T out;
			switch (opc) {
			case 0: /* ASR */
				out = apply(Binary::asr, value,
					static_cast<T>(shift));
				break;
			case 1: /* LSR */
				out = apply(Binary::lsr, value,
					static_cast<T>(shift));
				break;
			case 3: /* LSL */
				out = apply(Binary::lsl, value,
					static_cast<T>(shift));
				break;
			case 4: { /* ASRD */
				__int128 signed_value = to_signed(value);
				if (signed_value < 0)
					signed_value +=
						(__int128{1} << shift) - 1;
				out = static_cast<T>(signed_value >> shift);
				break;
			}
			case 6: /* SQSHL */
				out = shift_by(
					value, shift, true, false, true, true);
				break;
			case 7: /* UQSHL */
				out = shift_by(value, shift, false, false, true,
					false);
				break;
			case 12: /* SRSHR */
				out = shift_by(
					value, -shift, true, true, false, true);
				break;
			case 13: /* URSHR */
				out = shift_by(value, -shift, false, true,
					false, false);
				break;
			default: /* SQSHLU */
				out = shift_by(
					value, shift, true, false, true, false);
				break;
			}
			result.set<T>(element, out);
		}
	});
	context.set_z(dn, result);
	return retired();
}

/* SSRA, USRA, SRSRA and URSRA: right shift by immediate and accumulate,
 * tszh in bits 23:22, tszl in 20:19, imm3 in 18:16, R in 11, U in 10. */
SveResult shift_right_accumulate(Context &context)
{
	const ShiftImmediate immediate = shift_immediate(
		context.field(22, 2) << 2U | context.field(19, 2),
		context.field(16, 3), false);
	const bool rounding = context.bit(11);
	const bool is_signed = !context.bit(10);
	const unsigned da = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	Vector result = context.z(da);
	with_size(immediate.size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			const T shifted = shift_by(n.get<T>(element),
				-static_cast<std::int64_t>(immediate.shift),
				is_signed, rounding, false, is_signed);
			result.set<T>(element,
				static_cast<T>(
					result.get<T>(element) + shifted));
		}
	});
	context.set_z(da, result);
	return retired();
}

/* SRI and SLI: shift and insert, tszh in bits 23:22, tszl in 20:19, imm3
 * in 18:16, op (left) in 10. */
SveResult shift_insert(Context &context)
{
	const bool left = context.bit(10);
	const ShiftImmediate immediate = shift_immediate(
		context.field(22, 2) << 2U | context.field(19, 2),
		context.field(16, 3), left);
	const unsigned d = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	Vector result = context.z(d);
	with_size(immediate.size, [&]<typename T>() {
		const unsigned shift = immediate.shift;
		const T all = std::numeric_limits<T>::max();
		const T mask = left         ? static_cast<T>(all << shift) :
			shift >= bits_of<T> ? T{0} :
					      static_cast<T>(all >> shift);
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			const T value = n.get<T>(element);
			const T shifted = left ?
				static_cast<T>(value << shift) :
				shift >= bits_of<T> ?
				T{0} :
				static_cast<T>(value >> shift);
			result.set<T>(element,
				static_cast<T>(
					(result.get<T>(element) & ~mask) |
					(shifted & mask)));
		}
	});
	context.set_z(d, result);
	return retired();
}

enum class Unary : std::uint8_t {
	abs,
	neg,
	cls,
	clz,
	cnt,
	cnot,
	not_,
	sxtb,
	uxtb,
	sxth,
	uxth,
	sxtw,
	uxtw,
	rbit,
	revb,
	revh,
	revw,
	sqabs,
	sqneg,
	urecpe,
	ursqrte,
};

/* RecipEstimate() and RecipSqrtEstimate() without increased precision:
 * the 9-bit estimates URECPE and URSQRTE return for an operand whose top
 * nine bits are `a`. */
[[nodiscard]] std::uint32_t reciprocal_estimate(std::uint32_t a) noexcept
{
	const std::uint32_t b = (1U << 19) / (a * 2U + 1U);
	return (b + 1U) / 2U;
}

[[nodiscard]] std::uint32_t reciprocal_square_root_estimate(
	std::uint32_t a) noexcept
{
	const std::uint64_t scaled =
		a < 256U ? a * 2U + 1U : ((a >> 1U << 1U) + 1U) * 2U;
	std::uint64_t b = 512;
	while (scaled * (b + 1U) * (b + 1U) < (std::uint64_t{1} << 28))
		++b;
	return static_cast<std::uint32_t>((b + 1U) / 2U);
}

template<typename T>
[[nodiscard]] T apply_unary(Unary op, T value) noexcept
{
	using S = std::make_signed_t<T>;
	constexpr unsigned esize = bits_of<T>;
	switch (op) {
	case Unary::abs:
		return to_signed(value) < 0 ? static_cast<T>(T{0} - value) :
					      value;
	case Unary::neg:
		return static_cast<T>(T{0} - value);
	case Unary::cls: {
		const T flipped =
			to_signed(value) < 0 ? static_cast<T>(~value) : value;
		return static_cast<T>(std::countl_zero(flipped) - 1);
	}
	case Unary::clz:
		return static_cast<T>(std::countl_zero(value));
	case Unary::cnt:
		return static_cast<T>(std::popcount(value));
	case Unary::cnot:
		return value == 0 ? T{1} : T{0};
	case Unary::not_:
		return static_cast<T>(~value);
	case Unary::sxtb:
		return static_cast<T>(
			static_cast<S>(static_cast<std::int8_t>(value)));
	case Unary::uxtb:
		return static_cast<T>(static_cast<std::uint8_t>(value));
	case Unary::sxth:
		return static_cast<T>(
			static_cast<S>(static_cast<std::int16_t>(value)));
	case Unary::uxth:
		return static_cast<T>(static_cast<std::uint16_t>(value));
	case Unary::sxtw:
		return static_cast<T>(
			static_cast<S>(static_cast<std::int32_t>(value)));
	case Unary::uxtw:
		return static_cast<T>(static_cast<std::uint32_t>(value));
	case Unary::rbit: {
		T result = 0;
		for (unsigned bit = 0; bit < esize; ++bit)
			if (((value >> bit) & 1U) != 0U)
				result = static_cast<T>(
					result | (T{1} << (esize - 1U - bit)));
		return result;
	}
	case Unary::revb:
	case Unary::revh:
	case Unary::revw: {
		const unsigned swap = op == Unary::revb ? 8U :
			op == Unary::revh               ? 16U :
							  32U;
		T result = 0;
		for (unsigned position = 0; position < esize;
			position += swap) {
			const T part = static_cast<T>((value >> position) &
				((T{1} << (swap - 1U) << 1U) - 1U));
			result = static_cast<T>(
				result | (part << (esize - swap - position)));
		}
		return result;
	}
	case Unary::sqabs:
		return saturate_signed<T>(to_signed(value) < 0 ?
				-static_cast<__int128>(to_signed(value)) :
				static_cast<__int128>(to_signed(value)));
	case Unary::sqneg:
		return saturate_signed<T>(
			-static_cast<__int128>(to_signed(value)));
	case Unary::urecpe: {
		const auto operand = static_cast<std::uint32_t>(value);
		if ((operand >> 31U) == 0U)
			return static_cast<T>(0xffff'ffffU);
		return static_cast<T>(
			reciprocal_estimate(operand >> 23U) << 23U);
	}
	case Unary::ursqrte: {
		const auto operand = static_cast<std::uint32_t>(value);
		if ((operand >> 30U) == 0U)
			return static_cast<T>(0xffff'ffffU);
		return static_cast<T>(
			reciprocal_square_root_estimate(operand >> 23U) << 23U);
	}
	}
	return value;
}

[[nodiscard]] Unary unary_op(const Context &context) noexcept
{
	switch (context.row.form) {
	case form::abs_z_p_z_m:
		return Unary::abs;
	case form::neg_z_p_z_m:
		return Unary::neg;
	case form::cls_z_p_z_m:
		return Unary::cls;
	case form::clz_z_p_z_m:
		return Unary::clz;
	case form::cnt_z_p_z_m:
		return Unary::cnt;
	case form::cnot_z_p_z_m:
		return Unary::cnot;
	case form::not_z_p_z_m:
		return Unary::not_;
	case form::sxtb_z_p_z_m: {
		static constexpr std::array extensions{
			Unary::sxtb, Unary::sxth, Unary::sxtw};
		return extensions[context.field(17, 2) % 3U];
	}
	case form::uxtb_z_p_z_m: {
		static constexpr std::array extensions{
			Unary::uxtb, Unary::uxth, Unary::uxtw};
		return extensions[context.field(17, 2) % 3U];
	}
	case form::rbit_z_p_z_m:
		return Unary::rbit;
	case form::revb_z_z_m: {
		static constexpr std::array reversals{
			Unary::revb, Unary::revh, Unary::revw, Unary::revw};
		return reversals[context.field(16, 2)];
	}
	case form::sqabs_z_p_z_m:
		return Unary::sqabs;
	case form::sqneg_z_p_z_m:
		return Unary::sqneg;
	case form::urecpe_z_p_z_m:
		return Unary::urecpe;
	default:
		return Unary::ursqrte;
	}
}

/* The merging unary forms <op> Zd, Pg/M, Zn. */
SveResult unary_predicated(Context &context)
{
	const Unary op = unary_op(context);
	const unsigned size = context.field(22, 2);
	const unsigned d = context.field(0, 5);
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector n = context.z(context.field(5, 5));
	Vector result = context.z(d);
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>; ++element)
			if (active(mask, element, sizeof(T)))
				result.set<T>(element,
					apply_unary(op, n.get<T>(element)));
	});
	context.set_z(d, result);
	return retired();
}

/* REVD: each 128-bit element's doublewords swapped. */
SveResult reverse_doublewords(Context &context)
{
	const unsigned d = context.field(0, 5);
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector n = context.z(context.field(5, 5));
	Vector result = context.z(d);
	if (active(mask, 0, 16)) {
		result.set<std::uint64_t>(0, n.get<std::uint64_t>(1));
		result.set<std::uint64_t>(1, n.get<std::uint64_t>(0));
	}
	context.set_z(d, result);
	return retired();
}

/* MLA and MLS (Zda += or -= Zn * Zm, op in bit 13) and MAD and MSB (Zdn =
 * Za +or- Zdn * Zm), predicated. */
SveResult multiply_add_predicated(Context &context)
{
	const bool accumulate_form = context.row.form == form::mla_z_p_zzz ||
		context.row.form == form::mls_z_p_zzz;
	const bool subtract = context.bit(13);
	const unsigned size = context.field(22, 2);
	const unsigned d = context.field(0, 5);
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector first = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result = context.z(d);
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			if (!active(mask, element, sizeof(T)))
				continue;
			/* MLA: acc Zda, factors Zn and Zm. MAD: acc Za (bits
			 * 9:5), factors Zdn and Zm. */
			const T addend = accumulate_form ?
				result.get<T>(element) :
				first.get<T>(element);
			const T factor = accumulate_form ?
				first.get<T>(element) :
				result.get<T>(element);
			const T product =
				static_cast<T>(static_cast<__int128>(factor) *
					m.get<T>(element));
			result.set<T>(element,
				static_cast<T>(subtract ? addend - product :
							  addend + product));
		}
	});
	context.set_z(d, result);
	return retired();
}

/* An indexed form's element size, Zm and index: H (size 0x) has i3h in
 * bit 22, i3l in 20:19 and Zm in 18:16; S (10) i2 in 20:19 and Zm in
 * 18:16; D (11) i1 in 20 and Zm in 19:16. */
struct Indexed final {
	unsigned size;
	unsigned m;
	unsigned index;
};

[[nodiscard]] Indexed indexed_operands(const Context &context) noexcept
{
	if (!context.bit(23))
		return {1, context.field(16, 3),
			context.field(22, 1) << 2U | context.field(19, 2)};
	if (!context.bit(22))
		return {2, context.field(16, 3), context.field(19, 2)};
	return {3, context.field(16, 4), context.field(20, 1)};
}

/* MLA, MLS (op in bit 10) and MUL (indexed), SQDMULH and SQRDMULH (indexed,
 * R in bit 10), SQRDMLAH and SQRDMLSH (indexed, S in bit 10): each element
 * with the indexed element of its 128-bit segment of Zm. */
SveResult multiply_indexed(Context &context)
{
	const Indexed indexed = indexed_operands(context);
	const unsigned d = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(indexed.m);
	Vector result = context.z(d);
	const std::uint16_t id = context.row.form;
	with_size(indexed.size, [&]<typename T>() {
		constexpr unsigned segment = 16U / sizeof(T);
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			const T left = n.get<T>(element);
			const T right = m.get<T>(
				element - element % segment + indexed.index);
			const T accumulator = result.get<T>(element);
			T out;
			if (id == form::mul_z_zzi_h) {
				out = static_cast<T>(
					static_cast<__int128>(left) * right);
			} else if (id == form::mla_z_zzzi_h ||
				id == form::mls_z_zzzi_h) {
				const T product = static_cast<T>(
					static_cast<__int128>(left) * right);
				out = static_cast<T>(context.bit(10) ?
						accumulator - product :
						accumulator + product);
			} else if (id == form::sqdmulh_z_zzi_h ||
				id == form::sqrdmulh_z_zzi_h) {
				out = doubling_multiply_high(
					left, right, context.bit(10));
			} else {
				out = rounding_doubling_accumulate(accumulator,
					static_cast<__int128>(to_signed(left)) *
						to_signed(right),
					context.bit(10));
			}
			result.set<T>(element, out);
		}
	});
	context.set_z(d, result);
	return retired();
}

/* SQRDMLAH and SQRDMLSH (vectors): S in bit 10. */
SveResult rounding_doubling_multiply_add(Context &context)
{
	const unsigned size = context.field(22, 2);
	const unsigned da = context.field(0, 5);
	const bool subtract = context.bit(10);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result = context.z(da);
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>; ++element)
			result.set<T>(element,
				rounding_doubling_accumulate(
					result.get<T>(element),
					static_cast<__int128>(
						to_signed(n.get<T>(element))) *
						to_signed(m.get<T>(element)),
					subtract));
	});
	context.set_z(da, result);
	return retired();
}

enum class Compare : std::uint8_t {
	eq,
	ne,
	ge,
	gt,
	lt,
	le,
	hs,
	hi,
	lo,
	ls,
};

[[nodiscard]] bool compare(Compare op, __int128 left, __int128 right) noexcept
{
	switch (op) {
	case Compare::eq:
		return left == right;
	case Compare::ne:
		return left != right;
	case Compare::ge:
	case Compare::hs:
		return left >= right;
	case Compare::gt:
	case Compare::hi:
		return left > right;
	case Compare::lt:
	case Compare::lo:
		return left < right;
	case Compare::le:
	case Compare::ls:
		return left <= right;
	}
	return false;
}

[[nodiscard]] bool unsigned_compare(Compare op) noexcept
{
	return op == Compare::hs || op == Compare::hi || op == Compare::lo ||
		op == Compare::ls;
}

/* The comparison of a compare form: vectors (op in bit 15, o2 in 13, ne
 * in 4), wide elements (bits 15:13 and ne), or an immediate, signed (bit
 * 15, bit 13 and ne) or unsigned (lt in bit 13 and ne). */
[[nodiscard]] Compare compare_op(const Context &context) noexcept
{
	const bool ne = context.bit(4);
	switch (context.row.form) {
	case form::cmpeq_p_p_zz: {
		const unsigned bits =
			context.field(15, 1) << 1U | context.field(13, 1);
		if (bits == 0U)
			return ne ? Compare::hi : Compare::hs;
		if (bits == 2U)
			return ne ? Compare::gt : Compare::ge;
		return ne ? Compare::ne : Compare::eq;
	}
	case form::cmpeq_p_p_zw:
		switch (context.field(13, 3)) {
		case 1:
			return ne ? Compare::ne : Compare::eq;
		case 2:
			return ne ? Compare::gt : Compare::ge;
		case 3:
			return ne ? Compare::le : Compare::lt;
		case 6:
			return ne ? Compare::hi : Compare::hs;
		default:
			return ne ? Compare::ls : Compare::lo;
		}
	default:
		/* The signed immediate forms have bit 24 set, the unsigned
		 * ones clear. */
		if (!context.bit(24)) {
			if (context.bit(13))
				return ne ? Compare::ls : Compare::lo;
			return ne ? Compare::hi : Compare::hs;
		}
		if (context.bit(15))
			return ne ? Compare::ne : Compare::eq;
		if (context.bit(13))
			return ne ? Compare::le : Compare::lt;
		return ne ? Compare::gt : Compare::ge;
	}
}

/* CMP<cc> (vectors, wide elements, immediate): Pd = the active elements
 * whose comparison holds, and NZCV from PredTest(). */
SveResult compare_form(Context &context)
{
	const Compare op = compare_op(context);
	const bool is_unsigned = unsigned_compare(op);
	const unsigned size = context.field(22, 2);
	const unsigned esize = esize_of(size);
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	const std::uint16_t id = context.row.form;
	Predicate result{};
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			if (!active(mask, element, sizeof(T)))
				continue;
			const __int128 left =
				value_of(n.get<T>(element), !is_unsigned);
			__int128 right;
			if (id == form::cmpeq_p_p_zz)
				right = value_of(
					m.get<T>(element), !is_unsigned);
			else if (id == form::cmpeq_p_p_zw)
				right = value_of(m.get<std::uint64_t>(element *
							 sizeof(T) / 8U),
					!is_unsigned);
			else if (context.bit(24))
				right = sign_extend(context.field(16, 5), 5);
			else
				right = context.field(14, 7);
			set_active(result, element, sizeof(T),
				compare(op, left, right));
		}
	});
	context.cpu.nzcv = predicate_test(mask, result, esize);
	context.set_p(context.field(0, 4), result);
	return retired();
}

/* SADDV, UADDV (to 64 bits), SMAXV, UMAXV, SMINV, UMINV, ANDV, ORV and
 * EORV: the active elements reduced into a scalar in Vd. */
SveResult reduce(Context &context)
{
	const unsigned size = context.field(22, 2);
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector n = context.z(context.field(5, 5));
	const std::uint16_t id = context.row.form;
	std::uint64_t result = 0;
	unsigned bytes = 8;
	with_size(size, [&]<typename T>() {
		using S = std::make_signed_t<T>;
		if (id == form::saddv_r_p_z || id == form::uaddv_r_p_z) {
			std::uint64_t sum = 0;
			const bool is_signed = id == form::saddv_r_p_z;
			for (unsigned element = 0; element < elements_of<T>;
				++element)
				if (active(mask, element, sizeof(T)))
					sum += static_cast<std::uint64_t>(
						value_of(n.get<T>(element),
							is_signed));
			result = sum;
			return;
		}
		bytes = sizeof(T);
		T value;
		Binary op;
		if (id == form::smaxv_r_p_z) {
			value = static_cast<T>(std::numeric_limits<S>::min());
			op = Binary::smax;
		} else if (id == form::umaxv_r_p_z) {
			value = 0;
			op = Binary::umax;
		} else if (id == form::sminv_r_p_z) {
			value = static_cast<T>(std::numeric_limits<S>::max());
			op = Binary::smin;
		} else if (id == form::uminv_r_p_z) {
			value = std::numeric_limits<T>::max();
			op = Binary::umin;
		} else if (id == form::andv_r_p_z) {
			value = std::numeric_limits<T>::max();
			op = Binary::and_;
		} else if (id == form::orv_r_p_z) {
			value = 0;
			op = Binary::orr;
		} else {
			value = 0;
			op = Binary::eor;
		}
		for (unsigned element = 0; element < elements_of<T>; ++element)
			if (active(mask, element, sizeof(T)))
				value = apply(op, value, n.get<T>(element));
		result = value;
	});
	context.set_v_scalar(context.field(0, 5), result, bytes);
	return retired();
}

/* ADDP, SMAXP, UMAXP, SMINP and UMINP: pairwise, the even result
 * elements from pairs of Zdn and the odd ones from pairs of Zm. */
SveResult pairwise(Context &context)
{
	const std::uint16_t id = context.row.form;
	const Binary op = id == form::addp_z_p_zz ? Binary::add :
		id == form::smaxp_z_p_zz          ? Binary::smax :
		id == form::umaxp_z_p_zz          ? Binary::umax :
		id == form::sminp_z_p_zz          ? Binary::smin :
						    Binary::umin;
	const unsigned size = context.field(22, 2);
	const unsigned dn = context.field(0, 5);
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector first = context.z(dn);
	const Vector m = context.z(context.field(5, 5));
	Vector result = first;
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			if (!active(mask, element, sizeof(T)))
				continue;
			const bool even = element % 2U == 0U;
			const Vector &source = even ? first : m;
			const unsigned base = even ? element : element - 1U;
			result.set<T>(element,
				apply(op, source.get<T>(base),
					source.get<T>(base + 1U)));
		}
	});
	context.set_z(dn, result);
	return retired();
}

/* EOR3, BCAX, BSL, BSL1N, BSL2N and NBSL: opc in bits 23:22, o2 in 10. */
SveResult bitwise_ternary(Context &context)
{
	const unsigned dn = context.field(0, 5);
	const Vector first = context.z(dn);
	const Vector m = context.z(context.field(16, 5));
	const Vector k = context.z(context.field(5, 5));
	const unsigned op = context.field(22, 2) << 1U | context.field(10, 1);
	Vector result;
	for (unsigned element = 0; element < 2U; ++element) {
		const std::uint64_t a = first.get<std::uint64_t>(element);
		const std::uint64_t b = m.get<std::uint64_t>(element);
		const std::uint64_t c = k.get<std::uint64_t>(element);
		std::uint64_t value;
		switch (op) {
		case 0: /* EOR3 */
			value = a ^ b ^ c;
			break;
		case 2: /* BCAX */
			value = a ^ (b & ~c);
			break;
		case 1: /* BSL */
			value = (a & c) | (b & ~c);
			break;
		case 3: /* BSL1N */
			value = (~a & c) | (b & ~c);
			break;
		case 5: /* BSL2N */
			value = (a & c) | (~b & ~c);
			break;
		default: /* NBSL */
			value = ~((a & c) | (b & ~c));
			break;
		}
		result.set<std::uint64_t>(element, value);
	}
	context.set_z(dn, result);
	return retired();
}

/* XAR: Zdn EOR Zm rotated right, tszh in bits 23:22, tszl in 20:19, imm3
 * in 18:16. */
SveResult exclusive_or_rotate(Context &context)
{
	const ShiftImmediate immediate = shift_immediate(
		context.field(22, 2) << 2U | context.field(19, 2),
		context.field(16, 3), false);
	const unsigned dn = context.field(0, 5);
	const Vector first = context.z(dn);
	const Vector m = context.z(context.field(5, 5));
	Vector result;
	with_size(immediate.size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			const T value = static_cast<T>(
				first.get<T>(element) ^ m.get<T>(element));
			result.set<T>(element,
				std::rotr(value,
					static_cast<int>(immediate.shift)));
		}
	});
	context.set_z(dn, result);
	return retired();
}

/* SABA and UABA: absolute difference and accumulate, U in bit 10. */
SveResult absolute_difference_accumulate(Context &context)
{
	const unsigned size = context.field(22, 2);
	const unsigned da = context.field(0, 5);
	const Binary op = context.bit(10) ? Binary::uabd : Binary::sabd;
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result = context.z(da);
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>; ++element)
			result.set<T>(element,
				static_cast<T>(result.get<T>(element) +
					apply(op, n.get<T>(element),
						m.get<T>(element))));
	});
	context.set_z(da, result);
	return retired();
}

/* SCLAMP and UCLAMP: Zd clamped to [Zn, Zm], U in bit 10. */
SveResult clamp(Context &context)
{
	const unsigned size = context.field(22, 2);
	const unsigned d = context.field(0, 5);
	const bool is_signed = !context.bit(10);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result = context.z(d);
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			const __int128 value =
				value_of(result.get<T>(element), is_signed);
			const __int128 low =
				value_of(n.get<T>(element), is_signed);
			const __int128 high =
				value_of(m.get<T>(element), is_signed);
			const __int128 clamped =
				std::min(std::max(value, low), high);
			result.set<T>(element, static_cast<T>(clamped));
		}
	});
	context.set_z(d, result);
	return retired();
}

/* ADCLB, ADCLT, SBCLB and SBCLT: sz in bit 22, subtract in 23, T in 10. */
SveResult add_with_carry_long(Context &context)
{
	const unsigned da = context.field(0, 5);
	const bool subtract = context.bit(23);
	const unsigned top = context.field(10, 1);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result = context.z(da);
	with_size(context.bit(22) ? 3U : 2U, [&]<typename T>() {
		for (unsigned pair = 0; pair < elements_of<T> / 2U; ++pair) {
			const T accumulator = result.get<T>(2U * pair);
			T operand = n.get<T>(2U * pair + top);
			if (subtract)
				operand = static_cast<T>(~operand);
			const T carry_in = m.get<T>(2U * pair + 1U) & 1U;
			const __int128 sum =
				static_cast<__int128>(accumulator) + operand +
				carry_in;
			result.set<T>(2U * pair, static_cast<T>(sum));
			result.set<T>(2U * pair + 1U,
				static_cast<T>(sum >> bits_of<T>));
		}
	});
	context.set_z(da, result);
	return retired();
}

/* SADALP and UADALP: pairs of Zn's half-width elements summed into Zda,
 * U in bit 16. */
SveResult add_pairwise_accumulate(Context &context)
{
	const unsigned size = context.field(22, 2);
	const unsigned da = context.field(0, 5);
	const bool is_signed = !context.bit(16);
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector n = context.z(context.field(5, 5));
	Vector result = context.z(da);
	with_size(size, [&]<typename T>() {
		if constexpr (sizeof(T) > 1) {
			using H = narrower_of<T>;
			for (unsigned element = 0; element < elements_of<T>;
				++element) {
				if (!active(mask, element, sizeof(T)))
					continue;
				const __int128 sum =
					value_of(n.get<H>(2U * element),
						is_signed) +
					value_of(n.get<H>(2U * element + 1U),
						is_signed);
				result.set<T>(element,
					static_cast<T>(result.get<T>(element) +
						static_cast<T>(sum)));
			}
		}
	});
	context.set_z(da, result);
	return retired();
}

/* CADD and SQCADD: complex add with rotate 90 (rot clear) or 270, with
 * saturation for SQCADD (bit 16). */
SveResult complex_add(Context &context)
{
	const unsigned size = context.field(22, 2);
	const unsigned dn = context.field(0, 5);
	const bool rotate_270 = context.bit(10);
	const bool saturating = context.bit(16);
	const Vector first = context.z(dn);
	const Vector m = context.z(context.field(5, 5));
	Vector result;
	with_size(size, [&]<typename T>() {
		for (unsigned pair = 0; pair < elements_of<T> / 2U; ++pair) {
			__int128 real = to_signed(first.get<T>(2U * pair));
			__int128 imaginary =
				to_signed(first.get<T>(2U * pair + 1U));
			const __int128 m_real = to_signed(m.get<T>(2U * pair));
			const __int128 m_imaginary =
				to_signed(m.get<T>(2U * pair + 1U));
			if (!rotate_270) {
				real -= m_imaginary;
				imaginary += m_real;
			} else {
				real += m_imaginary;
				imaginary -= m_real;
			}
			result.set<T>(2U * pair,
				saturating ? saturate_signed<T>(real) :
					     static_cast<T>(real));
			result.set<T>(2U * pair + 1U,
				saturating ? saturate_signed<T>(imaginary) :
					     static_cast<T>(imaginary));
		}
	});
	context.set_z(dn, result);
	return retired();
}

/* HISTCNT: for each active element, the active elements of Zm at or below
 * it that equal it. */
SveResult histogram_count(Context &context)
{
	const unsigned size = context.field(22, 2);
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result;
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			T count = 0;
			if (active(mask, element, sizeof(T)))
				for (unsigned other = 0; other <= element;
					++other)
					if (active(mask, other, sizeof(T)) &&
						n.get<T>(element) ==
							m.get<T>(other))
						++count;
			result.set<T>(element, count);
		}
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* HISTSEG: for each byte, the bytes of its 128-bit segment of Zm that
 * equal it. */
SveResult histogram_segment(Context &context)
{
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result;
	for (unsigned element = 0; element < 16U; ++element) {
		std::uint8_t count = 0;
		for (unsigned other = 0; other < 16U; ++other)
			if (n.get<std::uint8_t>(element) ==
				m.get<std::uint8_t>(other))
				++count;
		result.set<std::uint8_t>(element, count);
	}
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* MATCH and NMATCH (bit 4): whether each active element equals any element
 * of its 128-bit segment of Zm; sz in bit 22. */
SveResult match(Context &context)
{
	const unsigned size = context.field(22, 1);
	const unsigned esize = esize_of(size);
	const bool negate = context.bit(4);
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Predicate result{};
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			if (!active(mask, element, sizeof(T)))
				continue;
			bool found = false;
			for (unsigned other = 0; other < elements_of<T>;
				++other)
				found = found ||
					n.get<T>(element) == m.get<T>(other);
			set_active(result, element, sizeof(T), found != negate);
		}
	});
	context.cpu.nzcv = predicate_test(mask, result, esize);
	context.set_p(context.field(0, 4), result);
	return retired();
}

} // namespace

Handler integer_handler(std::uint16_t id) noexcept
{
	switch (id) {
	case form::add_z_p_zz:
	case form::sub_z_p_zz:
	case form::subr_z_p_zz:
	case form::smax_z_p_zz:
	case form::umax_z_p_zz:
	case form::smin_z_p_zz:
	case form::umin_z_p_zz:
	case form::sabd_z_p_zz:
	case form::uabd_z_p_zz:
	case form::mul_z_p_zz:
	case form::smulh_z_p_zz:
	case form::umulh_z_p_zz:
	case form::sdiv_z_p_zz:
	case form::udiv_z_p_zz:
	case form::sdivr_z_p_zz:
	case form::udivr_z_p_zz:
	case form::orr_z_p_zz:
	case form::eor_z_p_zz:
	case form::and_z_p_zz:
	case form::bic_z_p_zz:
	case form::asr_z_p_zz:
	case form::lsr_z_p_zz:
	case form::lsl_z_p_zz:
	case form::asrr_z_p_zz:
	case form::lsrr_z_p_zz:
	case form::lslr_z_p_zz:
	case form::shadd_z_p_zz:
	case form::uhadd_z_p_zz:
	case form::shsub_z_p_zz:
	case form::uhsub_z_p_zz:
	case form::shsubr_z_p_zz:
	case form::uhsubr_z_p_zz:
	case form::srhadd_z_p_zz:
	case form::urhadd_z_p_zz:
	case form::sqadd_z_p_zz:
	case form::uqadd_z_p_zz:
	case form::sqsub_z_p_zz:
	case form::uqsub_z_p_zz:
	case form::sqsubr_z_p_zz:
	case form::uqsubr_z_p_zz:
	case form::suqadd_z_p_zz:
	case form::usqadd_z_p_zz:
	case form::sqshl_z_p_zz:
	case form::uqshl_z_p_zz:
	case form::sqrshl_z_p_zz:
	case form::uqrshl_z_p_zz:
	case form::srshl_z_p_zz:
	case form::urshl_z_p_zz:
	case form::sqshlr_z_p_zz:
	case form::uqshlr_z_p_zz:
	case form::sqrshlr_z_p_zz:
	case form::uqrshlr_z_p_zz:
	case form::srshlr_z_p_zz:
	case form::urshlr_z_p_zz:
		return binary_predicated;
	case form::asr_z_p_zw:
	case form::lsr_z_p_zw:
	case form::lsl_z_p_zw:
		return shift_wide_predicated;
	case form::asr_z_zw:
	case form::lsr_z_zw:
	case form::lsl_z_zw:
		return shift_wide;
	case form::add_z_zz:
		return binary_unpredicated<Binary::add>;
	case form::sub_z_zz:
		return binary_unpredicated<Binary::sub>;
	case form::sqadd_z_zz:
		return binary_unpredicated<Binary::sqadd>;
	case form::uqadd_z_zz:
		return binary_unpredicated<Binary::uqadd>;
	case form::sqsub_z_zz:
		return binary_unpredicated<Binary::sqsub>;
	case form::uqsub_z_zz:
		return binary_unpredicated<Binary::uqsub>;
	case form::and_z_zz:
		return binary_unpredicated<Binary::and_>;
	case form::orr_z_zz:
		return binary_unpredicated<Binary::orr>;
	case form::eor_z_zz:
		return binary_unpredicated<Binary::eor>;
	case form::bic_z_zz:
		return binary_unpredicated<Binary::bic>;
	case form::mul_z_zz:
		return binary_unpredicated<Binary::mul>;
	case form::smulh_z_zz:
		return binary_unpredicated<Binary::smulh>;
	case form::umulh_z_zz:
		return binary_unpredicated<Binary::umulh>;
	case form::pmul_z_zz:
		return binary_unpredicated<Binary::pmul>;
	case form::sqdmulh_z_zz:
		return binary_unpredicated<Binary::sqdmulh>;
	case form::sqrdmulh_z_zz:
		return binary_unpredicated<Binary::sqrdmulh>;
	case form::add_z_zi:
		return binary_immediate_shifted<Binary::add>;
	case form::sub_z_zi:
		return binary_immediate_shifted<Binary::sub>;
	case form::subr_z_zi:
		return binary_immediate_shifted<Binary::subr>;
	case form::sqadd_z_zi:
		return binary_immediate_shifted<Binary::sqadd>;
	case form::uqadd_z_zi:
		return binary_immediate_shifted<Binary::uqadd>;
	case form::sqsub_z_zi:
		return binary_immediate_shifted<Binary::sqsub>;
	case form::uqsub_z_zi:
		return binary_immediate_shifted<Binary::uqsub>;
	case form::smax_z_zi:
		return binary_immediate<Binary::smax>;
	case form::umax_z_zi:
		return binary_immediate<Binary::umax>;
	case form::smin_z_zi:
		return binary_immediate<Binary::smin>;
	case form::umin_z_zi:
		return binary_immediate<Binary::umin>;
	case form::mul_z_zi:
		return binary_immediate<Binary::mul>;
	case form::and_z_zi:
	case form::orr_z_zi:
	case form::eor_z_zi:
		return logical_immediate_form;
	case form::dupm_z_i:
		return duplicate_mask_immediate;
	case form::asr_z_zi:
	case form::lsr_z_zi:
	case form::lsl_z_zi:
		return shift_immediate_unpredicated;
	case form::asr_z_p_zi:
	case form::lsr_z_p_zi:
	case form::lsl_z_p_zi:
	case form::asrd_z_p_zi:
	case form::sqshl_z_p_zi:
	case form::uqshl_z_p_zi:
	case form::srshr_z_p_zi:
	case form::urshr_z_p_zi:
	case form::sqshlu_z_p_zi:
		return shift_immediate_predicated;
	case form::ssra_z_zi:
	case form::usra_z_zi:
	case form::srsra_z_zi:
	case form::ursra_z_zi:
		return shift_right_accumulate;
	case form::sri_z_zzi:
	case form::sli_z_zzi:
		return shift_insert;
	case form::abs_z_p_z_m:
	case form::neg_z_p_z_m:
	case form::cls_z_p_z_m:
	case form::clz_z_p_z_m:
	case form::cnt_z_p_z_m:
	case form::cnot_z_p_z_m:
	case form::not_z_p_z_m:
	case form::sxtb_z_p_z_m:
	case form::uxtb_z_p_z_m:
	case form::rbit_z_p_z_m:
	case form::revb_z_z_m:
	case form::sqabs_z_p_z_m:
	case form::sqneg_z_p_z_m:
	case form::urecpe_z_p_z_m:
	case form::ursqrte_z_p_z_m:
		return unary_predicated;
	case form::revd_z_p_z_m:
		return reverse_doublewords;
	case form::mla_z_p_zzz:
	case form::mls_z_p_zzz:
	case form::mad_z_p_zzz:
	case form::msb_z_p_zzz:
		return multiply_add_predicated;
	case form::mla_z_zzzi_h:
	case form::mls_z_zzzi_h:
	case form::mul_z_zzi_h:
	case form::sqdmulh_z_zzi_h:
	case form::sqrdmulh_z_zzi_h:
	case form::sqrdmlah_z_zzzi_h:
	case form::sqrdmlsh_z_zzzi_h:
		return multiply_indexed;
	case form::sqrdmlah_z_zzz:
	case form::sqrdmlsh_z_zzz:
		return rounding_doubling_multiply_add;
	case form::cmpeq_p_p_zz:
	case form::cmpeq_p_p_zw:
	case form::cmpeq_p_p_zi:
		return compare_form;
	case form::saddv_r_p_z:
	case form::uaddv_r_p_z:
	case form::smaxv_r_p_z:
	case form::umaxv_r_p_z:
	case form::sminv_r_p_z:
	case form::uminv_r_p_z:
	case form::andv_r_p_z:
	case form::orv_r_p_z:
	case form::eorv_r_p_z:
		return reduce;
	case form::addp_z_p_zz:
	case form::smaxp_z_p_zz:
	case form::umaxp_z_p_zz:
	case form::sminp_z_p_zz:
	case form::uminp_z_p_zz:
		return pairwise;
	case form::eor3_z_zzz:
	case form::bcax_z_zzz:
	case form::bsl_z_zzz:
	case form::bsl1n_z_zzz:
	case form::bsl2n_z_zzz:
	case form::nbsl_z_zzz:
		return bitwise_ternary;
	case form::xar_z_zzi:
		return exclusive_or_rotate;
	case form::saba_z_zzz:
	case form::uaba_z_zzz:
		return absolute_difference_accumulate;
	case form::sclamp_z_zz:
	case form::uclamp_z_zz:
		return clamp;
	case form::adclb_z_zzz:
	case form::adclt_z_zzz:
	case form::sbclb_z_zzz:
	case form::sbclt_z_zzz:
		return add_with_carry_long;
	case form::sadalp_z_p_z:
	case form::uadalp_z_p_z:
		return add_pairwise_accumulate;
	case form::cadd_z_zz:
	case form::sqcadd_z_zz:
		return complex_add;
	case form::histcnt_z_p_zz:
		return histogram_count;
	case form::histseg_z_zz:
		return histogram_segment;
	case form::match_p_p_zz:
	case form::nmatch_p_p_zz:
		return match;
	default:
		return nullptr;
	}
}

} // namespace uml::cpu::arm64::detail::sve_semantics
