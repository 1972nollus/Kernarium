// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The semantic library's predicate and control forms: the predicate logical
 * operations, breaks, permutes, FFR, the WHILE and PTRUE generators, the
 * element counts and the vector-length reads. A predicate here is its packed
 * value, bit k for byte k, which is what the architecture moves.
 */
#include "sve_semantics_internal.hpp"

namespace uml::cpu::arm64::detail::sve_semantics {

namespace {

constexpr std::uint32_t all_bits = 0xffffU;

[[nodiscard]] std::uint32_t pbits(const Context &context, unsigned n) noexcept
{
	return predicate_bits(context.p(n));
}

void set_pbits(Context &context, unsigned n, std::uint32_t bits) noexcept
{
	context.set_p(n, predicate_of_bits(bits));
}

[[nodiscard]] bool element_set(
	std::uint32_t bits, unsigned element, unsigned esize) noexcept
{
	return ((bits >> (element * esize)) & 1U) != 0U;
}

/* The packed predicate whose first `count` elements of `esize` bytes are
 * active. */
[[nodiscard]] std::uint32_t leading(unsigned count, unsigned esize) noexcept
{
	std::uint32_t bits = 0;
	for (unsigned element = 0; element < count && element < 16U / esize;
		++element)
		bits |= 1U << (element * esize);
	return bits;
}

[[nodiscard]] std::uint32_t test(
	std::uint32_t mask, std::uint32_t result, unsigned esize) noexcept
{
	return predicate_test(
		predicate_of_bits(mask), predicate_of_bits(result), esize);
}

/* The number of elements of `esize` bytes active in `bits`. */
[[nodiscard]] unsigned count_active(std::uint32_t bits, unsigned esize) noexcept
{
	unsigned count = 0;
	for (unsigned element = 0; element < 16U / esize; ++element)
		count += element_set(bits, element, esize) ? 1U : 0U;
	return count;
}

/* LastActive(): whether the last element `mask` makes active is active in
 * `bits`, false when none is. */
[[nodiscard]] bool last_active(
	std::uint32_t mask, std::uint32_t bits, unsigned esize) noexcept
{
	bool last = false;
	for (unsigned element = 0; element < 16U / esize; ++element)
		if (element_set(mask, element, esize))
			last = element_set(bits, element, esize);
	return last;
}

/* The esize-bit chunk of a packed predicate that holds element `element`. */
[[nodiscard]] std::uint32_t chunk(
	std::uint32_t bits, unsigned element, unsigned esize) noexcept
{
	return (bits >> (element * esize)) & ((1U << esize) - 1U);
}

/* AND, BIC, EOR, NAND, NOR, ORN, ORR and SEL (predicates), and their
 * flag-setting forms: op in bit 23, S in 22, o2 in 9, o3 in 4. */
SveResult predicate_logical(Context &context)
{
	const std::uint32_t g = pbits(context, context.field(10, 4));
	const std::uint32_t n = pbits(context, context.field(5, 4));
	const std::uint32_t m = pbits(context, context.field(16, 4));
	const unsigned op = context.field(23, 1) << 2U |
		context.field(9, 1) << 1U | context.field(4, 1);
	std::uint32_t result;
	switch (op) {
	case 0: /* AND */
		result = g & n & m;
		break;
	case 1: /* BIC */
		result = g & n & ~m;
		break;
	case 2: /* EOR */
		result = g & (n ^ m);
		break;
	case 3: /* SEL */
		result = (g & n) | (~g & m);
		break;
	case 4: /* ORR */
		result = g & (n | m);
		break;
	case 5: /* ORN */
		result = g & (n | ~m);
		break;
	case 6: /* NOR */
		result = g & ~(n | m);
		break;
	default: /* NAND */
		result = g & ~(n & m);
		break;
	}
	result &= all_bits;
	if (context.bit(22))
		context.cpu.nzcv = test(g, result, 1);
	set_pbits(context, context.field(0, 4), result);
	return retired();
}

/* BRKA, BRKB and BRKAS, BRKBS: B in bit 23, S in 22, M in 4. */
SveResult predicate_break(Context &context)
{
	const std::uint32_t g = pbits(context, context.field(10, 4));
	const std::uint32_t n = pbits(context, context.field(5, 4));
	const unsigned d = context.field(0, 4);
	const bool before = context.bit(23);
	const bool merging = context.bit(4);
	const std::uint32_t previous = merging ? pbits(context, d) : 0U;
	bool broken = false;
	std::uint32_t result = 0;
	for (unsigned element = 0; element < 16U; ++element) {
		if (element_set(g, element, 1)) {
			if (before)
				broken = broken || element_set(n, element, 1);
			if (!broken)
				result |= 1U << element;
			if (!before)
				broken = broken || element_set(n, element, 1);
		} else {
			result |= previous & (1U << element);
		}
	}
	if (context.bit(22))
		context.cpu.nzcv = test(g, result, 1);
	set_pbits(context, d, result);
	return retired();
}

/* BRKN and BRKNS: S in bit 22. */
SveResult predicate_break_next(Context &context)
{
	const std::uint32_t g = pbits(context, context.field(10, 4));
	const std::uint32_t n = pbits(context, context.field(5, 4));
	const unsigned dm = context.field(0, 4);
	const std::uint32_t result =
		last_active(g, n, 1) ? pbits(context, dm) : 0U;
	if (context.bit(22))
		context.cpu.nzcv = test(all_bits, result, 1);
	set_pbits(context, dm, result);
	return retired();
}

/* BRKPA, BRKPB and BRKPAS, BRKPBS: S in bit 22, B in 4. */
SveResult predicate_break_propagate(Context &context)
{
	const std::uint32_t g = pbits(context, context.field(10, 4));
	const std::uint32_t n = pbits(context, context.field(5, 4));
	const std::uint32_t m = pbits(context, context.field(16, 4));
	const bool before = context.bit(4);
	bool last = last_active(g, n, 1);
	std::uint32_t result = 0;
	for (unsigned element = 0; element < 16U; ++element) {
		if (!element_set(g, element, 1))
			continue;
		if (before)
			last = last && !element_set(m, element, 1);
		if (last)
			result |= 1U << element;
		if (!before)
			last = last && !element_set(m, element, 1);
	}
	if (context.bit(22))
		context.cpu.nzcv = test(g, result, 1);
	set_pbits(context, context.field(0, 4), result);
	return retired();
}

SveResult predicate_false(Context &context)
{
	set_pbits(context, context.field(0, 4), 0);
	return retired();
}

/* PTRUE and PTRUES: S in bit 16. */
SveResult predicate_true(Context &context)
{
	const unsigned esize = esize_of(context.field(22, 2));
	const std::uint32_t result =
		leading(predicate_count(context.field(5, 5), esize), esize);
	if (context.bit(16))
		context.cpu.nzcv = test(result, result, esize);
	set_pbits(context, context.field(0, 4), result);
	return retired();
}

SveResult predicate_test_form(Context &context)
{
	context.cpu.nzcv = test(pbits(context, context.field(10, 4)),
		pbits(context, context.field(5, 4)), 1);
	return retired();
}

SveResult predicate_first(Context &context)
{
	const std::uint32_t g = pbits(context, context.field(5, 4));
	const unsigned dn = context.field(0, 4);
	std::uint32_t result = pbits(context, dn);
	if (g != 0U)
		result |= g & (~g + 1U);
	context.cpu.nzcv = test(g, result, 1);
	set_pbits(context, dn, result);
	return retired();
}

SveResult predicate_next(Context &context)
{
	const unsigned esize = esize_of(context.field(22, 2));
	const unsigned elements = 16U / esize;
	const std::uint32_t mask = pbits(context, context.field(5, 4));
	const unsigned dn = context.field(0, 4);
	const std::uint32_t operand = pbits(context, dn);
	unsigned next = 0;
	for (unsigned element = 0; element < elements; ++element)
		if (element_set(operand, element, esize))
			next = element + 1U;
	while (next < elements && !element_set(mask, next, esize))
		++next;
	const std::uint32_t result =
		next < elements ? 1U << (next * esize) : 0U;
	context.cpu.nzcv = test(mask, result, esize);
	set_pbits(context, dn, result);
	return retired();
}

/* PUNPKHI and PUNPKLO: hi in bit 16. */
SveResult predicate_unpack(Context &context)
{
	const std::uint32_t n = pbits(context, context.field(5, 4));
	const unsigned offset = context.bit(16) ? 8U : 0U;
	std::uint32_t result = 0;
	for (unsigned element = 0; element < 8U; ++element)
		if (element_set(n, element + offset, 1))
			result |= 1U << (element * 2U);
	set_pbits(context, context.field(0, 4), result);
	return retired();
}

SveResult predicate_reverse(Context &context)
{
	const unsigned esize = esize_of(context.field(22, 2));
	const unsigned elements = 16U / esize;
	const std::uint32_t n = pbits(context, context.field(5, 4));
	std::uint32_t result = 0;
	for (unsigned element = 0; element < elements; ++element)
		result |= chunk(n, elements - 1U - element, esize)
			<< (element * esize);
	set_pbits(context, context.field(0, 4), result);
	return retired();
}

/* ZIP1, ZIP2, UZP1, UZP2, TRN1 and TRN2 (predicates): opc in bits 12:11,
 * part in 10. */
SveResult predicate_permute(Context &context)
{
	const unsigned esize = esize_of(context.field(22, 2));
	const unsigned pairs = 16U / (esize * 2U);
	const std::uint32_t n = pbits(context, context.field(5, 4));
	const std::uint32_t m = pbits(context, context.field(16, 4));
	const unsigned part = context.field(10, 1);
	std::uint32_t result = 0;
	const auto put = [&](unsigned element, std::uint32_t value) {
		result |= value << (element * esize);
	};
	switch (context.field(11, 2)) {
	case 0: /* ZIP */
		for (unsigned pair = 0; pair < pairs; ++pair) {
			put(2U * pair, chunk(n, part * pairs + pair, esize));
			put(2U * pair + 1U,
				chunk(m, part * pairs + pair, esize));
		}
		break;
	case 1: /* UZP */
		for (unsigned pair = 0; pair < pairs; ++pair) {
			put(pair, chunk(n, 2U * pair + part, esize));
			put(pairs + pair, chunk(m, 2U * pair + part, esize));
		}
		break;
	default: /* TRN */
		for (unsigned pair = 0; pair < pairs; ++pair) {
			put(2U * pair, chunk(n, 2U * pair + part, esize));
			put(2U * pair + 1U, chunk(m, 2U * pair + part, esize));
		}
		break;
	}
	set_pbits(context, context.field(0, 4), result);
	return retired();
}

/* RDFFR (predicated) and RDFFRS: S in bit 22. */
SveResult read_ffr_predicated(Context &context)
{
	const std::uint32_t g = pbits(context, context.field(5, 4));
	const std::uint32_t result = predicate_bits(context.cpu.sve_ffr) & g;
	if (context.bit(22))
		context.cpu.nzcv = test(g, result, 1);
	set_pbits(context, context.field(0, 4), result);
	return retired();
}

SveResult read_ffr(Context &context)
{
	context.set_p(context.field(0, 4), context.cpu.sve_ffr);
	return retired();
}

SveResult set_ffr(Context &context)
{
	context.cpu.sve_ffr = predicate_of_bits(all_bits);
	return retired();
}

SveResult write_ffr(Context &context)
{
	context.cpu.sve_ffr = context.p(context.field(5, 4));
	return retired();
}

SveResult count_predicate(Context &context)
{
	const unsigned esize = esize_of(context.field(22, 2));
	context.set_x(context.field(0, 5),
		count_active(pbits(context, context.field(10, 4)) &
				pbits(context, context.field(5, 4)),
			esize));
	return retired();
}

/* INCP and DECP (scalar and vector): D in bit 16, scalar in 11. */
SveResult increment_by_predicate(Context &context)
{
	const unsigned size = context.field(22, 2);
	const unsigned esize = esize_of(size);
	const std::uint64_t count =
		count_active(pbits(context, context.field(5, 4)), esize);
	const bool decrement = context.bit(16);
	const unsigned dn = context.field(0, 5);
	if (context.bit(11)) {
		const std::uint64_t operand = context.x(dn);
		context.set_x(
			dn, decrement ? operand - count : operand + count);
		return retired();
	}
	Vector vector = context.z(dn);
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			const T value = vector.get<T>(element);
			vector.set<T>(element,
				static_cast<T>(decrement ? value - count :
							   value + count));
		}
	});
	context.set_z(dn, vector);
	return retired();
}

/* The saturating scalar update of SQINC*, UQINC*, SQDEC*, UQDEC* and
 * their predicate-count forms: `delta` added to or subtracted from the
 * low `wide ? 64 : 32` bits of Xdn, saturated, and extended to 64 bits. */
[[nodiscard]] std::uint64_t saturating_scalar(std::uint64_t operand,
	std::uint64_t delta, bool decrement, bool is_signed, bool wide) noexcept
{
	const __int128 change = decrement ? -static_cast<__int128>(delta) :
					    static_cast<__int128>(delta);
	if (wide) {
		const __int128 value = value_of(operand, is_signed) + change;
		return saturate<std::uint64_t>(value, is_signed);
	}
	const auto low = static_cast<std::uint32_t>(operand);
	const std::uint32_t result = saturate<std::uint32_t>(
		value_of(low, is_signed) + change, is_signed);
	return is_signed ? static_cast<std::uint64_t>(static_cast<std::int64_t>(
				   static_cast<std::int32_t>(result))) :
			   result;
}

/* Each element of Zdn plus or minus `delta`, saturated. */
void saturating_vector(Context &context, unsigned size, std::uint64_t delta,
	bool decrement, bool is_signed)
{
	const unsigned dn = context.field(0, 5);
	Vector vector = context.z(dn);
	const __int128 change = decrement ? -static_cast<__int128>(delta) :
					    static_cast<__int128>(delta);
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>; ++element)
			vector.set<T>(element,
				saturate<T>(value_of(vector.get<T>(element),
						    is_signed) +
						change,
					is_signed));
	});
	context.set_z(dn, vector);
}

/* SQINCP, UQINCP, SQDECP and UQDECP: D in bit 17, U in 16, scalar in 11,
 * and for the scalar forms sf in 10. */
SveResult saturating_by_predicate(Context &context)
{
	const unsigned size = context.field(22, 2);
	const std::uint64_t count = count_active(
		pbits(context, context.field(5, 4)), esize_of(size));
	const bool decrement = context.bit(17);
	const bool is_signed = !context.bit(16);
	if (context.bit(11)) {
		const unsigned dn = context.field(0, 5);
		context.set_x(dn,
			saturating_scalar(context.x(dn), count, decrement,
				is_signed, context.bit(10)));
		return retired();
	}
	saturating_vector(context, size, count, decrement, is_signed);
	return retired();
}

/* CNTB, CNTH, CNTW and CNTD. */
SveResult count_elements(Context &context)
{
	const unsigned esize = esize_of(context.field(22, 2));
	context.set_x(context.field(0, 5),
		std::uint64_t{predicate_count(context.field(5, 5), esize)} *
			(context.field(16, 4) + 1U));
	return retired();
}

/* The element count times the multiplier of a count-by-pattern form. */
[[nodiscard]] std::uint64_t pattern_count(const Context &context) noexcept
{
	return std::uint64_t{predicate_count(
		       context.field(5, 5), esize_of(context.field(22, 2)))} *
		(context.field(16, 4) + 1U);
}

/* INC[BHWD] and DEC[BHWD] (scalar): D in bit 10. */
SveResult increment_scalar(Context &context)
{
	const unsigned dn = context.field(0, 5);
	const std::uint64_t operand = context.x(dn);
	const std::uint64_t count = pattern_count(context);
	context.set_x(dn, context.bit(10) ? operand - count : operand + count);
	return retired();
}

/* INC[HWD] and DEC[HWD] (vector): D in bit 10. */
SveResult increment_vector(Context &context)
{
	const unsigned size = context.field(22, 2);
	const unsigned dn = context.field(0, 5);
	const std::uint64_t count = pattern_count(context);
	const bool decrement = context.bit(10);
	Vector vector = context.z(dn);
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			const T value = vector.get<T>(element);
			vector.set<T>(element,
				static_cast<T>(decrement ? value - count :
							   value + count));
		}
	});
	context.set_z(dn, vector);
	return retired();
}

/* SQINC[BHWD], UQINC[BHWD], SQDEC[BHWD] and UQDEC[BHWD] (scalar): sf in
 * bit 20, D in 11, U in 10. */
SveResult saturating_scalar_by_pattern(Context &context)
{
	const unsigned dn = context.field(0, 5);
	context.set_x(dn,
		saturating_scalar(context.x(dn), pattern_count(context),
			context.bit(11), !context.bit(10), context.bit(20)));
	return retired();
}

/* SQINC[HWD], UQINC[HWD], SQDEC[HWD] and UQDEC[HWD] (vector): D in bit 11,
 * U in 10. */
SveResult saturating_vector_by_pattern(Context &context)
{
	saturating_vector(context, context.field(22, 2), pattern_count(context),
		context.bit(11), !context.bit(10));
	return retired();
}

SveResult read_vector_length(Context &context)
{
	const auto immediate =
		static_cast<std::int64_t>(sign_extend(context.field(5, 6), 6));
	context.set_x(context.field(0, 5),
		static_cast<std::uint64_t>(immediate * vector_bytes));
	return retired();
}

/* ADDVL (op in bit 22 clear) and ADDPL. */
SveResult add_vector_length(Context &context)
{
	const auto immediate =
		static_cast<std::int64_t>(sign_extend(context.field(5, 6), 6));
	const std::int64_t unit =
		context.bit(22) ? vector_bytes / 8 : vector_bytes;
	context.set_x_or_sp(context.field(0, 5),
		context.x_or_sp(context.field(16, 5)) +
			static_cast<std::uint64_t>(immediate * unit));
	return retired();
}

/* WHILELT, WHILELE, WHILELO, WHILELS (lt in bit 10) and WHILEGE, WHILEGT,
 * WHILEHS, WHILEHI, each with U in bit 11: one predicate with sf in bit 12
 * and eq in 4, or a pair with eq in 0. */
SveResult predicate_while(Context &context)
{
	const unsigned esize = esize_of(context.field(22, 2));
	const bool pair = context.row.form == form::whilelo_pp_rr ||
		context.row.form == form::whilels_pp_rr ||
		context.row.form == form::whilelt_pp_rr ||
		context.row.form == form::whilele_pp_rr ||
		context.row.form == form::whilege_pp_rr ||
		context.row.form == form::whilegt_pp_rr ||
		context.row.form == form::whilehs_pp_rr ||
		context.row.form == form::whilehi_pp_rr;
	const bool wide = pair || context.bit(12);
	const bool is_unsigned = context.bit(11);
	const bool increasing = context.bit(10);
	const bool equal = pair ? context.bit(0) : context.bit(4);
	const unsigned elements = 16U / esize * (pair ? 2U : 1U);
	const std::uint64_t width_mask =
		wide ? ~std::uint64_t{0} : 0xffff'ffffU;
	std::uint64_t operand1 = context.x(context.field(5, 5)) & width_mask;
	const std::uint64_t operand2 =
		context.x(context.field(16, 5)) & width_mask;
	const auto value = [&](std::uint64_t operand) -> __int128 {
		if (is_unsigned)
			return operand;
		return wide ? static_cast<__int128>(
				      static_cast<std::int64_t>(operand)) :
			      static_cast<__int128>(static_cast<std::int32_t>(
				      static_cast<std::uint32_t>(operand)));
	};
	const __int128 limit = value(operand2);
	std::uint32_t result = 0;
	bool last = true;
	for (unsigned step = 0; step < elements; ++step) {
		const unsigned element =
			increasing ? step : elements - 1U - step;
		const __int128 current = value(operand1);
		const bool condition = increasing ?
			(equal ? current <= limit : current < limit) :
			(equal ? current > limit : current >= limit);
		last = last && condition;
		if (last)
			result |= 1U << (element * esize);
		operand1 = (increasing ? operand1 + 1U : operand1 - 1U) &
			width_mask;
	}
	if (!pair) {
		context.cpu.nzcv = test(all_bits, result, esize);
		set_pbits(context, context.field(0, 4), result);
		return retired();
	}
	context.cpu.nzcv = [&] {
		bool first = true;
		bool n = false;
		bool z = true;
		bool c = true;
		for (unsigned element = 0; element < elements; ++element) {
			const bool set =
				((result >> (element * esize)) & 1U) != 0U;
			if (first)
				n = set;
			first = false;
			z = z && !set;
			c = !set;
		}
		return (n ? 1U : 0U) << 31U | (z ? 1U : 0U) << 30U |
			(c ? 1U : 0U) << 29U;
	}();
	const unsigned d = context.field(1, 3) << 1U;
	set_pbits(context, d, result & all_bits);
	set_pbits(context, d + 1U, result >> 16U);
	return retired();
}

/* WHILEWR and WHILERW: rw in bit 4. */
SveResult predicate_while_conflict(Context &context)
{
	const unsigned esize = esize_of(context.field(22, 2));
	const auto operand1 =
		static_cast<__int128>(context.x(context.field(5, 5)));
	const auto operand2 =
		static_cast<__int128>(context.x(context.field(16, 5)));
	const bool read_write = context.bit(4);
	__int128 difference = operand2 - operand1;
	if (read_write && difference < 0)
		difference = -difference;
	/* DIVRM rounds toward minus infinity. */
	__int128 quotient = difference / esize;
	if (difference % esize != 0 && difference < 0)
		--quotient;
	std::uint32_t result = 0;
	for (unsigned element = 0; element < 16U / esize; ++element) {
		const bool set = read_write ?
			(quotient == 0 || element < quotient) :
			(quotient <= 0 || element < quotient);
		if (set)
			result |= 1U << (element * esize);
	}
	context.cpu.nzcv = test(all_bits, result, esize);
	set_pbits(context, context.field(0, 4), result);
	return retired();
}

/* PSEL: i1 in bit 23, tszh in 22, tszl in 20:18, Rv in 17:16. */
SveResult predicate_select(Context &context)
{
	const unsigned imm5 = context.field(23, 1) << 4U |
		context.field(22, 1) << 3U | context.field(18, 3);
	unsigned esize;
	unsigned immediate;
	if ((imm5 & 1U) != 0U) {
		esize = 1;
		immediate = imm5 >> 1U;
	} else if ((imm5 & 2U) != 0U) {
		esize = 2;
		immediate = imm5 >> 2U;
	} else if ((imm5 & 4U) != 0U) {
		esize = 4;
		immediate = imm5 >> 3U;
	} else if ((imm5 & 8U) != 0U) {
		esize = 8;
		immediate = imm5 >> 4U;
	} else {
		return fatal();
	}
	const unsigned elements = 16U / esize;
	const auto index = static_cast<std::uint32_t>(
		context.x(12U + context.field(16, 2)));
	const unsigned element = static_cast<unsigned>(
		(std::uint64_t{index} + immediate) % elements);
	const std::uint32_t m = pbits(context, context.field(5, 4));
	set_pbits(context, context.field(0, 4),
		element_set(m, element, esize) ?
			pbits(context, context.field(10, 4)) :
			0U);
	return retired();
}

/* CTERMEQ and CTERMNE: ne in bit 4, sz in 22. */
SveResult compare_terminate(Context &context)
{
	const std::uint64_t mask =
		context.bit(22) ? ~std::uint64_t{0} : 0xffff'ffffU;
	const bool equal = (context.x(context.field(5, 5)) & mask) ==
		(context.x(context.field(16, 5)) & mask);
	const bool terminate = context.bit(4) ? !equal : equal;
	std::uint32_t nzcv = context.cpu.nzcv & 0x6000'0000U;
	if (terminate)
		nzcv |= 0x8000'0000U;
	else if ((context.cpu.nzcv & 0x2000'0000U) == 0U)
		nzcv |= 0x1000'0000U;
	context.cpu.nzcv = nzcv;
	return retired();
}

} // namespace

Handler predicate_handler(std::uint16_t id) noexcept
{
	switch (id) {
	case form::and_p_p_pp_z:
	case form::ands_p_p_pp_z:
	case form::bic_p_p_pp_z:
	case form::bics_p_p_pp_z:
	case form::eor_p_p_pp_z:
	case form::eors_p_p_pp_z:
	case form::nand_p_p_pp_z:
	case form::nands_p_p_pp_z:
	case form::nor_p_p_pp_z:
	case form::nors_p_p_pp_z:
	case form::orn_p_p_pp_z:
	case form::orns_p_p_pp_z:
	case form::orr_p_p_pp_z:
	case form::orrs_p_p_pp_z:
	case form::sel_p_p_pp:
		return predicate_logical;
	case form::brka_p_p_p:
	case form::brkas_p_p_p_z:
	case form::brkb_p_p_p:
	case form::brkbs_p_p_p_z:
		return predicate_break;
	case form::brkn_p_p_pp:
	case form::brkns_p_p_pp:
		return predicate_break_next;
	case form::brkpa_p_p_pp:
	case form::brkpas_p_p_pp:
	case form::brkpb_p_p_pp:
	case form::brkpbs_p_p_pp:
		return predicate_break_propagate;
	case form::pfalse_p:
		return predicate_false;
	case form::ptrue_p_s:
	case form::ptrues_p_s:
		return predicate_true;
	case form::ptest__p_p:
		return predicate_test_form;
	case form::pfirst_p_p_p:
		return predicate_first;
	case form::pnext_p_p_p:
		return predicate_next;
	case form::punpkhi_p_p:
		return predicate_unpack;
	case form::rev_p_p:
		return predicate_reverse;
	case form::trn1_p_pp:
	case form::uzp1_p_pp:
	case form::zip2_p_pp:
		return predicate_permute;
	case form::rdffr_p_p_f:
	case form::rdffrs_p_p_f:
		return read_ffr_predicated;
	case form::rdffr_p_f:
		return read_ffr;
	case form::setffr_f:
		return set_ffr;
	case form::wrffr_f_p:
		return write_ffr;
	case form::cntp_r_p_p:
		return count_predicate;
	case form::incp_r_p_r:
	case form::incp_z_p_z:
	case form::decp_r_p_r:
	case form::decp_z_p_z:
		return increment_by_predicate;
	case form::sqincp_r_p_r_sx:
	case form::sqincp_z_p_z:
	case form::uqincp_r_p_r_uw:
	case form::uqincp_z_p_z:
	case form::sqdecp_r_p_r_sx:
	case form::sqdecp_z_p_z:
	case form::uqdecp_r_p_r_uw:
	case form::uqdecp_z_p_z:
		return saturating_by_predicate;
	case form::cntb_r_s:
		return count_elements;
	case form::incb_r_rs:
	case form::decb_r_rs:
		return increment_scalar;
	case form::incd_z_zs:
	case form::decd_z_zs:
		return increment_vector;
	case form::sqincb_r_rs_sx:
	case form::sqinch_r_rs_sx:
	case form::sqincw_r_rs_sx:
	case form::sqincd_r_rs_sx:
	case form::uqincb_r_rs_uw:
	case form::uqinch_r_rs_uw:
	case form::uqincw_r_rs_uw:
	case form::uqincd_r_rs_uw:
	case form::sqdecb_r_rs_sx:
	case form::sqdech_r_rs_sx:
	case form::sqdecw_r_rs_sx:
	case form::sqdecd_r_rs_sx:
	case form::uqdecb_r_rs_uw:
	case form::uqdech_r_rs_uw:
	case form::uqdecw_r_rs_uw:
	case form::uqdecd_r_rs_uw:
		return saturating_scalar_by_pattern;
	case form::sqinch_z_zs:
	case form::sqincw_z_zs:
	case form::sqincd_z_zs:
	case form::uqinch_z_zs:
	case form::uqincw_z_zs:
	case form::uqincd_z_zs:
	case form::sqdech_z_zs:
	case form::sqdecw_z_zs:
	case form::sqdecd_z_zs:
	case form::uqdech_z_zs:
	case form::uqdecw_z_zs:
	case form::uqdecd_z_zs:
		return saturating_vector_by_pattern;
	case form::rdvl_r_i:
		return read_vector_length;
	case form::addvl_r_ri:
	case form::addpl_r_ri:
		return add_vector_length;
	case form::whilelt_p_p_rr:
	case form::whilele_p_p_rr:
	case form::whilelo_p_p_rr:
	case form::whilels_p_p_rr:
	case form::whilege_p_p_rr:
	case form::whilegt_p_p_rr:
	case form::whilehs_p_p_rr:
	case form::whilehi_p_p_rr:
	case form::whilelt_pp_rr:
	case form::whilele_pp_rr:
	case form::whilelo_pp_rr:
	case form::whilels_pp_rr:
	case form::whilege_pp_rr:
	case form::whilegt_pp_rr:
	case form::whilehs_pp_rr:
	case form::whilehi_pp_rr:
		return predicate_while;
	case form::whilerw_p_rr:
	case form::whilewr_p_rr:
		return predicate_while_conflict;
	case form::psel_p_ppi:
		return predicate_select;
	case form::ctermeq_rr:
		return compare_terminate;
	default:
		return nullptr;
	}
}

} // namespace uml::cpu::arm64::detail::sve_semantics
