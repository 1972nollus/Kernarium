// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The semantic library's integer forms that change their element size: the
 * SVE2 long (bottom and top) and wide arithmetic, multiplies and
 * multiply-adds, the narrowing additions, shifts and extractions, the
 * multi-vector narrowings, and the dot products and complex forms whose
 * elements combine narrower parts.
 */
#include "sve_semantics_internal.hpp"

namespace uml::cpu::arm64::detail::sve_semantics {

namespace {

/* Calls f.template operator()<N, W>() for the narrow element type N and
 * the wide type W twice its size, given the wide element's size field
 * value (1 for halfwords through 3 for doublewords). */
template<typename F>
void with_widening(unsigned wide_size, F &&f)
{
	switch (wide_size & 3U) {
	case 1:
		f.template operator()<std::uint8_t, std::uint16_t>();
		break;
	case 2:
		f.template operator()<std::uint16_t, std::uint32_t>();
		break;
	case 3:
		f.template operator()<std::uint32_t, std::uint64_t>();
		break;
	default:
		break;
	}
}

/* SADDLB, SADDLT, UADDLB, UADDLT, SSUBLB, SSUBLT, USUBLB, USUBLT, SABDLB,
 * SABDLT, UABDLB, UABDLT (bits 13:12 the operation, U in 11, T in 10) and
 * SADDLBT, SSUBLBT, SSUBLTB (subtract in bit 11, TB in 10). */
SveResult long_arithmetic(Context &context)
{
	const std::uint16_t id = context.row.form;
	const bool mixed = id == form::saddlbt_z_zz ||
		id == form::ssublbt_z_zz || id == form::ssubltb_z_zz;
	unsigned first_part;
	unsigned second_part;
	unsigned operation; /* 0 add, 1 subtract, 3 absolute difference */
	bool is_signed;
	if (mixed) {
		first_part = context.field(10, 1);
		second_part = first_part ^ 1U;
		operation = context.field(11, 1);
		is_signed = true;
	} else {
		first_part = second_part = context.field(10, 1);
		operation = context.field(12, 2);
		is_signed = !context.bit(11);
	}
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result;
	with_widening(context.field(22, 2), [&]<typename N, typename W>() {
		for (unsigned element = 0; element < elements_of<W>;
			++element) {
			const __int128 left = value_of(
				n.get<N>(2U * element + first_part), is_signed);
			const __int128 right =
				value_of(m.get<N>(2U * element + second_part),
					is_signed);
			const __int128 value = operation == 0U ? left + right :
				operation == 1U                ? left - right :
				left > right                   ? left - right :
								 right - left;
			result.set<W>(element, static_cast<W>(value));
		}
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* SADDWB, SADDWT, UADDWB, UADDWT, SSUBWB, SSUBWT, USUBWB, USUBWT: subtract
 * in bit 12, U in 11, T in 10. */
SveResult wide_arithmetic(Context &context)
{
	const unsigned part = context.field(10, 1);
	const bool is_signed = !context.bit(11);
	const bool subtract = context.bit(12);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result;
	with_widening(context.field(22, 2), [&]<typename N, typename W>() {
		for (unsigned element = 0; element < elements_of<W>;
			++element) {
			const W wide = n.get<W>(element);
			const auto narrow = static_cast<W>(value_of(
				m.get<N>(2U * element + part), is_signed));
			result.set<W>(element,
				static_cast<W>(subtract ? wide - narrow :
							  wide + narrow));
		}
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* SABALB, SABALT, UABALB, UABALT: U in bit 11, T in 10. */
SveResult absolute_difference_accumulate_long(Context &context)
{
	const unsigned part = context.field(10, 1);
	const bool is_signed = !context.bit(11);
	const unsigned da = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result = context.z(da);
	with_widening(context.field(22, 2), [&]<typename N, typename W>() {
		for (unsigned element = 0; element < elements_of<W>;
			++element) {
			const __int128 left = value_of(
				n.get<N>(2U * element + part), is_signed);
			const __int128 right = value_of(
				m.get<N>(2U * element + part), is_signed);
			const auto difference = static_cast<W>(
				left > right ? left - right : right - left);
			result.set<W>(element,
				static_cast<W>(
					result.get<W>(element) + difference));
		}
	});
	context.set_z(da, result);
	return retired();
}

/* PolynomialMult(): the carry-less product of two narrow elements. */
template<typename W, typename N>
[[nodiscard]] W polynomial_product(N left, N right) noexcept
{
	W result = 0;
	for (unsigned bit = 0; bit < bits_of<N>; ++bit)
		if (((right >> bit) & 1U) != 0U)
			result = static_cast<W>(result ^ (W{left} << bit));
	return result;
}

/* How a long multiply combines its product with the accumulator. */
enum class Accumulate : std::uint8_t {
	none,
	add,
	subtract,
	saturating_none,
	saturating_add,
	saturating_subtract,
};

/* One element of a long multiply: the product of two narrow values, plus
 * or minus the accumulator, wrapped or saturated to W. */
template<typename W>
[[nodiscard]] W multiply_long_element(__int128 left, __int128 right,
	W accumulator, Accumulate accumulate) noexcept
{
	switch (accumulate) {
	case Accumulate::none:
		return static_cast<W>(left * right);
	case Accumulate::add:
		return static_cast<W>(
			accumulator + static_cast<W>(left * right));
	case Accumulate::subtract:
		return static_cast<W>(
			accumulator - static_cast<W>(left * right));
	case Accumulate::saturating_none:
		return saturate_signed<W>(2 * left * right);
	default: {
		const __int128 product =
			to_signed(saturate_signed<W>(2 * left * right));
		const __int128 base = to_signed(accumulator);
		return saturate_signed<W>(
			accumulate == Accumulate::saturating_add ?
				base + product :
				base - product);
	}
	}
}

/* SMULLB, SMULLT, UMULLB, UMULLT, SQDMULLB, SQDMULLT, PMULLB and PMULLT
 * (vectors): bits 12:11 the operation (00 SQDMULL, 01 PMULL, 10 SMULL, 11
 * UMULL), T in 10. PMULL's 128-bit form multiplies doublewords. */
SveResult multiply_long(Context &context)
{
	const unsigned part = context.field(10, 1);
	const unsigned operation = context.field(11, 2);
	const unsigned size = context.field(22, 2);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result;
	if (operation == 1U && size == 0U) {
		/* PMULLB/PMULLT (128-bit): one product per 128-bit segment. */
		const std::uint64_t left = n.get<std::uint64_t>(part);
		const std::uint64_t right = m.get<std::uint64_t>(part);
		unsigned __int128 product = 0;
		for (unsigned bit = 0; bit < 64U; ++bit)
			if (((right >> bit) & 1U) != 0U)
				product ^= static_cast<unsigned __int128>(left)
					<< bit;
		result.set<std::uint64_t>(
			0, static_cast<std::uint64_t>(product));
		result.set<std::uint64_t>(
			1, static_cast<std::uint64_t>(product >> 64U));
		context.set_z(context.field(0, 5), result);
		return retired();
	}
	with_widening(size, [&]<typename N, typename W>() {
		for (unsigned element = 0; element < elements_of<W>;
			++element) {
			const N left = n.get<N>(2U * element + part);
			const N right = m.get<N>(2U * element + part);
			W value;
			if (operation == 1U)
				value = polynomial_product<W>(left, right);
			else
				value = multiply_long_element<W>(
					value_of(left, operation != 3U),
					value_of(right, operation != 3U), W{0},
					operation == 0U ?
						Accumulate::saturating_none :
						Accumulate::none);
			result.set<W>(element, value);
		}
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* SMLALB, SMLALT, UMLALB, UMLALT, SMLSLB, SMLSLT, UMLSLB, UMLSLT (subtract
 * in bit 12, U in 11, T in 10), SQDMLALB, SQDMLALT, SQDMLSLB, SQDMLSLT
 * (subtract in bit 11, T in 10) and SQDMLALBT, SQDMLSLBT (subtract in bit
 * 10), all vectors. */
SveResult multiply_accumulate_long(Context &context)
{
	const std::uint16_t id = context.row.form;
	const bool bottom_top =
		id == form::sqdmlalbt_z_zzz || id == form::sqdmlslbt_z_zzz;
	const bool doubling = bottom_top || id == form::sqdmlalb_z_zzz ||
		id == form::sqdmlalt_z_zzz || id == form::sqdmlslb_z_zzz ||
		id == form::sqdmlslt_z_zzz;
	unsigned first_part;
	unsigned second_part;
	bool subtract;
	bool is_signed = true;
	if (bottom_top) {
		first_part = 0;
		second_part = 1;
		subtract = context.bit(10);
	} else if (doubling) {
		first_part = second_part = context.field(10, 1);
		subtract = context.bit(11);
	} else {
		first_part = second_part = context.field(10, 1);
		subtract = context.bit(12);
		is_signed = !context.bit(11);
	}
	const Accumulate accumulate = doubling ?
		(subtract ? Accumulate::saturating_subtract :
			    Accumulate::saturating_add) :
		(subtract ? Accumulate::subtract : Accumulate::add);
	const unsigned da = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result = context.z(da);
	with_widening(context.field(22, 2), [&]<typename N, typename W>() {
		for (unsigned element = 0; element < elements_of<W>; ++element)
			result.set<W>(element,
				multiply_long_element<W>(
					value_of(n.get<N>(2U * element +
							 first_part),
						is_signed),
					value_of(m.get<N>(2U * element +
							 second_part),
						is_signed),
					result.get<W>(element), accumulate));
	});
	context.set_z(da, result);
	return retired();
}

/*
 * The indexed long multiplies and multiply-adds: SMLALB, SMLALT, UMLALB,
 * UMLALT, SMLSLB, SMLSLT, UMLSLB, UMLSLT, SMULLB, SMULLT, UMULLB, UMULLT,
 * SQDMULLB, SQDMULLT, SQDMLALB, SQDMLALT, SQDMLSLB and SQDMLSLT. Size 10
 * (S from H) has the index in bits 20:19 and 11 and Zm in 18:16; size 11
 * (D from S) in bits 20 and 11 and Zm in 19:16. T is bit 10.
 */
SveResult multiply_long_indexed(Context &context)
{
	const std::uint16_t id = context.row.form;
	const unsigned size = context.field(22, 2);
	const unsigned part = context.field(10, 1);
	const unsigned index = size == 2U ?
		context.field(19, 2) << 1U | context.field(11, 1) :
		context.field(20, 1) << 1U | context.field(11, 1);
	const unsigned m_register =
		size == 2U ? context.field(16, 3) : context.field(16, 4);
	Accumulate accumulate;
	bool is_signed = true;
	if (id == form::smlalb_z_zzzi_s || id == form::smlalt_z_zzzi_s) {
		accumulate = Accumulate::add;
	} else if (id == form::umlalb_z_zzzi_s || id == form::umlalt_z_zzzi_s) {
		accumulate = Accumulate::add;
		is_signed = false;
	} else if (id == form::smlslb_z_zzzi_s || id == form::smlslt_z_zzzi_s) {
		accumulate = Accumulate::subtract;
	} else if (id == form::umlslb_z_zzzi_s || id == form::umlslt_z_zzzi_s) {
		accumulate = Accumulate::subtract;
		is_signed = false;
	} else if (id == form::smullb_z_zzi_s || id == form::smullt_z_zzi_s) {
		accumulate = Accumulate::none;
	} else if (id == form::umullb_z_zzi_s || id == form::umullt_z_zzi_s) {
		accumulate = Accumulate::none;
		is_signed = false;
	} else if (id == form::sqdmullb_z_zzi_s ||
		id == form::sqdmullt_z_zzi_s) {
		accumulate = Accumulate::saturating_none;
	} else if (id == form::sqdmlalb_z_zzzi_s ||
		id == form::sqdmlalt_z_zzzi_s) {
		accumulate = Accumulate::saturating_add;
	} else {
		accumulate = Accumulate::saturating_subtract;
	}
	const unsigned d = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(m_register);
	Vector result = context.z(d);
	with_widening(size, [&]<typename N, typename W>() {
		constexpr unsigned segment = 16U / sizeof(W);
		for (unsigned element = 0; element < elements_of<W>;
			++element) {
			const unsigned base = element - element % segment;
			result.set<W>(element,
				multiply_long_element<W>(
					value_of(n.get<N>(2U * element + part),
						is_signed),
					value_of(m.get<N>(2U * base + index),
						is_signed),
					result.get<W>(element), accumulate));
		}
	});
	context.set_z(d, result);
	return retired();
}

/* ADDHNB, ADDHNT, SUBHNB, SUBHNT, RADDHNB, RADDHNT, RSUBHNB, RSUBHNT: the
 * high half of each wide sum or difference, rounded for R (bit 11),
 * subtract in bit 12, T in 10. The bottom forms zero the odd elements, the
 * top forms keep the even ones. */
SveResult narrowing_high_half(Context &context)
{
	const bool top = context.bit(10);
	const bool rounding = context.bit(11);
	const bool subtract = context.bit(12);
	const unsigned d = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result = top ? context.z(d) : Vector{};
	with_widening(context.field(22, 2), [&]<typename N, typename W>() {
		for (unsigned element = 0; element < elements_of<W>;
			++element) {
			const __int128 left = n.get<W>(element);
			const __int128 right = m.get<W>(element);
			const __int128 value =
				(subtract ? left - right : left + right) +
				(rounding ? __int128{1} << (bits_of<N> - 1U) :
					    0);
			result.set<N>(2U * element + (top ? 1U : 0U),
				static_cast<N>(value >> bits_of<N>));
		}
	});
	context.set_z(d, result);
	return retired();
}

/* SHRNB, SHRNT, RSHRNB, RSHRNT, SQSHRNB, SQSHRNT, SQRSHRNB, SQRSHRNT,
 * UQSHRNB, UQSHRNT, UQRSHRNB, UQRSHRNT, SQSHRUNB, SQSHRUNT, SQRSHRUNB and
 * SQRSHRUNT: tszh (bit 22) : tszl (20:19) : imm3 (18:16) give the narrow
 * size and a right shift counted down from twice its width; bits 13:12 the
 * kind (00 SQSHRUN, 01 SHRN, 10 SQSHRN, 11 UQSHRN), rounding in bit 11, T
 * in 10. */
SveResult narrowing_shift(Context &context)
{
	const unsigned tsize =
		context.field(22, 1) << 2U | context.field(19, 2);
	const unsigned narrow_size =
		static_cast<unsigned>(std::bit_width(tsize)) - 1U;
	const unsigned shift =
		2U * (8U << narrow_size) - (tsize << 3U | context.field(16, 3));
	const unsigned kind = context.field(12, 2);
	const bool rounding = context.bit(11);
	const bool top = context.bit(10);
	const bool source_signed = kind == 0U || kind == 2U;
	const unsigned d = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	Vector result = top ? context.z(d) : Vector{};
	with_widening(narrow_size + 1U, [&]<typename N, typename W>() {
		for (unsigned element = 0; element < elements_of<W>;
			++element) {
			const __int128 value =
				(value_of(n.get<W>(element), source_signed) +
					(rounding ? __int128{1} << (shift -
							    1U) :
						    0)) >>
				shift;
			const N narrow = kind == 1U ?
				static_cast<N>(value) :
				saturate<N>(value, kind == 2U);
			result.set<N>(2U * element + (top ? 1U : 0U), narrow);
		}
	});
	context.set_z(d, result);
	return retired();
}

/* SQXTNB, SQXTNT, UQXTNB, UQXTNT, SQXTUNB and SQXTUNT: the wide element's
 * size from tszh (bit 22) : tszl (20:19); signed to unsigned in bit 12,
 * unsigned in 11, T in 10. */
SveResult saturating_extract_narrow(Context &context)
{
	const unsigned tsize =
		context.field(22, 1) << 2U | context.field(19, 2);
	const unsigned wide_size = static_cast<unsigned>(std::bit_width(tsize));
	const bool to_unsigned = context.bit(12);
	const bool source_signed = !context.bit(11);
	const bool top = context.bit(10);
	const unsigned d = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	Vector result = top ? context.z(d) : Vector{};
	with_widening(wide_size, [&]<typename N, typename W>() {
		for (unsigned element = 0; element < elements_of<W>;
			++element) {
			const __int128 value =
				value_of(n.get<W>(element), source_signed);
			result.set<N>(2U * element + (top ? 1U : 0U),
				saturate<N>(
					value, source_signed && !to_unsigned));
		}
	});
	context.set_z(d, result);
	return retired();
}

/* SSHLLB, SSHLLT, USHLLB and USHLLT: tszh (bit 22) : tszl (20:19) : imm3
 * (18:16) give the narrow size and a left shift counted up from its width;
 * U in bit 11, T in 10. */
SveResult shift_left_long(Context &context)
{
	const unsigned tsize =
		context.field(22, 1) << 2U | context.field(19, 2);
	const unsigned narrow_size =
		static_cast<unsigned>(std::bit_width(tsize)) - 1U;
	const unsigned shift =
		(tsize << 3U | context.field(16, 3)) - (8U << narrow_size);
	const bool is_signed = !context.bit(11);
	const unsigned part = context.field(10, 1);
	const Vector n = context.z(context.field(5, 5));
	Vector result;
	with_widening(narrow_size + 1U, [&]<typename N, typename W>() {
		for (unsigned element = 0; element < elements_of<W>; ++element)
			result.set<W>(element,
				static_cast<W>(
					value_of(n.get<N>(2U * element + part),
						is_signed)
					<< shift));
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* SQCVTN, SQCVTUN, UQCVTN (no shift), SQRSHRN, SQRSHRUN and UQRSHRN (a
 * rounding right shift of 16 - imm4, imm4 in bits 19:16): a pair of word
 * vectors Zn, Zn+1 (Zn even, bits 9:6) saturated to halfwords and
 * interleaved. */
SveResult narrowing_pair(Context &context)
{
	const std::uint16_t id = context.row.form;
	const bool shifting = id == form::sqrshrn_z_mz2 ||
		id == form::sqrshrun_z_mz2 || id == form::uqrshrn_z_mz2;
	const unsigned shift = shifting ? 16U - context.field(16, 4) : 0U;
	const bool source_signed =
		id != form::uqcvtn_z_mz2 && id != form::uqrshrn_z_mz2;
	const bool result_signed =
		id == form::sqcvtn_z_mz2 || id == form::sqrshrn_z_mz2;
	const unsigned n = context.field(6, 4) << 1U;
	const std::array sources{context.z(n), context.z(n + 1U)};
	Vector result;
	for (unsigned element = 0; element < 4U; ++element)
		for (unsigned i = 0; i < 2U; ++i) {
			__int128 value =
				value_of(sources[i].get<std::uint32_t>(element),
					source_signed);
			if (shift != 0U)
				value = (value +
						(__int128{1}
							<< (shift - 1U))) >>
					shift;
			result.set<std::uint16_t>(2U * element + i,
				saturate<std::uint16_t>(value, result_signed));
		}
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* EORBT and EORTB: the even (EORBT) or odd elements of Zd become Zn's
 * element there XOR Zm's neighbouring element; T in bit 10. */
SveResult interleaving_exclusive_or(Context &context)
{
	const unsigned first = context.field(10, 1);
	const unsigned d = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result = context.z(d);
	with_size(context.field(22, 2), [&]<typename T>() {
		for (unsigned pair = 0; pair < elements_of<T> / 2U; ++pair)
			result.set<T>(2U * pair + first,
				static_cast<T>(n.get<T>(2U * pair + first) ^
					m.get<T>(2U * pair + (first ^ 1U))));
	});
	context.set_z(d, result);
	return retired();
}

/* The signedness of a dot product's two operands. */
struct DotSigns final {
	bool first;
	bool second;
};

/* One dot-product element: the accumulator plus the products of the
 * narrow elements of Zn's element `element` and Zm's group `group`. */
template<typename W, typename N>
[[nodiscard]] W dot_element(const Vector &n, const Vector &m, W accumulator,
	unsigned element, unsigned group, DotSigns signs) noexcept
{
	constexpr unsigned parts = sizeof(W) / sizeof(N);
	__int128 sum = to_signed(accumulator);
	for (unsigned i = 0; i < parts; ++i)
		sum += value_of(n.get<N>(parts * element + i), signs.first) *
			value_of(m.get<N>(parts * group + i), signs.second);
	return static_cast<W>(sum);
}

/* An indexed form's Zm and index when words (S results) have the index in
 * bits 20:19 and Zm in 18:16 and doublewords the index in bit 20 and Zm in
 * 19:16. */
struct GroupIndex final {
	unsigned m;
	unsigned index;
};

[[nodiscard]] GroupIndex group_index(
	const Context &context, bool doubleword) noexcept
{
	return doubleword ?
		GroupIndex{context.field(16, 4), context.field(20, 1)} :
		GroupIndex{context.field(16, 3), context.field(19, 2)};
}

/*
 * SDOT, UDOT (four-way, vectors and indexed; bit 22 clear S from B, set D
 * from H; U in bit 10), USDOT and SUDOT (four-way, S from B), and SDOT,
 * UDOT (two-way, S from H; U in bit 10).
 */
SveResult dot_product(Context &context)
{
	const std::uint16_t id = context.row.form;
	const bool indexed = id == form::sdot_z_zzzi_s ||
		id == form::udot_z_zzzi_s || id == form::usdot_z_zzzi_s ||
		id == form::sudot_z_zzzi_s || id == form::sdot_z32_zzzi ||
		id == form::udot_z32_zzzi;
	const bool two_way = id == form::sdot_z32_zzz ||
		id == form::udot_z32_zzz || id == form::sdot_z32_zzzi ||
		id == form::udot_z32_zzzi;
	const bool mixed = id == form::usdot_z_zzz_s ||
		id == form::usdot_z_zzzi_s || id == form::sudot_z_zzzi_s;
	const bool doubleword = !two_way && !mixed && context.bit(22);
	DotSigns signs{!context.bit(10), !context.bit(10)};
	if (id == form::usdot_z_zzz_s || id == form::usdot_z_zzzi_s)
		signs = {false, true};
	else if (id == form::sudot_z_zzzi_s)
		signs = {true, false};
	const GroupIndex group_operands = indexed ?
		group_index(context, doubleword) :
		GroupIndex{context.field(16, 5), 0U};
	const unsigned da = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(group_operands.m);
	Vector result = context.z(da);
	const auto run = [&]<typename W, typename N>() {
		constexpr unsigned segment = 16U / sizeof(W);
		for (unsigned element = 0; element < elements_of<W>;
			++element) {
			const unsigned group = indexed ?
				element - element % segment +
					group_operands.index :
				element;
			result.set<W>(element,
				dot_element<W, N>(n, m, result.get<W>(element),
					element, group, signs));
		}
	};
	if (two_way)
		run.template operator()<std::uint32_t, std::uint16_t>();
	else if (doubleword)
		run.template operator()<std::uint64_t, std::uint16_t>();
	else
		run.template operator()<std::uint32_t, std::uint8_t>();
	context.set_z(da, result);
	return retired();
}

/* CDOT (vectors and indexed): rot in bits 11:10; bit 22 clear S from B,
 * set D from H. */
SveResult complex_dot_product(Context &context)
{
	const bool indexed = context.row.form == form::cdot_z_zzzi_s;
	const bool doubleword = context.bit(22);
	const unsigned rotation = context.field(10, 2);
	const unsigned select_a = rotation & 1U;
	const unsigned select_b = select_a ^ 1U;
	const bool subtract_imaginary = (rotation & 1U) == (rotation >> 1U);
	const GroupIndex group_operands = indexed ?
		group_index(context, doubleword) :
		GroupIndex{context.field(16, 5), 0U};
	const unsigned da = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(group_operands.m);
	Vector result = context.z(da);
	const auto run = [&]<typename W, typename N>() {
		constexpr unsigned segment = 16U / sizeof(W);
		for (unsigned element = 0; element < elements_of<W>;
			++element) {
			const unsigned group = indexed ?
				element - element % segment +
					group_operands.index :
				element;
			__int128 sum = to_signed(result.get<W>(element));
			for (unsigned i = 0; i < 2U; ++i) {
				const __int128 real = to_signed(
					n.get<N>(4U * element + 2U * i));
				const __int128 imaginary = to_signed(
					n.get<N>(4U * element + 2U * i + 1U));
				const __int128 a = to_signed(m.get<N>(
					4U * group + 2U * i + select_a));
				const __int128 b = to_signed(m.get<N>(
					4U * group + 2U * i + select_b));
				sum += real * a;
				sum += subtract_imaginary ? -(imaginary * b) :
							    imaginary * b;
			}
			result.set<W>(element, static_cast<W>(sum));
		}
	};
	if (doubleword)
		run.template operator()<std::uint64_t, std::uint16_t>();
	else
		run.template operator()<std::uint32_t, std::uint8_t>();
	context.set_z(da, result);
	return retired();
}

/*
 * CMLA and SQRDCMLAH (vectors, any size; indexed, bit 22 clear halfwords
 * and set words, with the index and Zm placed as for a word or doubleword
 * dot product): rot in bits 11:10.
 */
SveResult complex_multiply_add(Context &context)
{
	const std::uint16_t id = context.row.form;
	const bool indexed =
		id == form::cmla_z_zzzi_h || id == form::sqrdcmlah_z_zzzi_h;
	const bool saturating =
		id == form::sqrdcmlah_z_zzz || id == form::sqrdcmlah_z_zzzi_h;
	const unsigned rotation = context.field(10, 2);
	const unsigned select_a = rotation & 1U;
	const unsigned select_b = select_a ^ 1U;
	const std::array subtracts{
		(rotation & 1U) != (rotation >> 1U), (rotation >> 1U) != 0U};
	const unsigned size =
		indexed ? (context.bit(22) ? 2U : 1U) : context.field(22, 2);
	const GroupIndex group_operands = indexed ?
		group_index(context, context.bit(22)) :
		GroupIndex{context.field(16, 5), 0U};
	const unsigned da = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(group_operands.m);
	Vector result = context.z(da);
	with_size(size, [&]<typename T>() {
		constexpr unsigned pairs_per_segment = 8U / sizeof(T);
		for (unsigned pair = 0; pair < elements_of<T> / 2U; ++pair) {
			const unsigned group = indexed ?
				pair - pair % pairs_per_segment +
					group_operands.index :
				pair;
			const __int128 a1 =
				to_signed(n.get<T>(2U * pair + select_a));
			const __int128 a2 =
				to_signed(m.get<T>(2U * group + select_a));
			const __int128 b2 =
				to_signed(m.get<T>(2U * group + select_b));
			const std::array products{a1 * a2, a1 * b2};
			for (unsigned part = 0; part < 2U; ++part) {
				const T accumulator =
					result.get<T>(2U * pair + part);
				const __int128 product = products[part];
				T value;
				if (!saturating) {
					const auto low =
						static_cast<T>(product);
					value = static_cast<T>(subtracts[part] ?
							accumulator - low :
							accumulator + low);
				} else {
					value = rounding_doubling_accumulate(
						accumulator, product,
						subtracts[part]);
				}
				result.set<T>(2U * pair + part, value);
			}
		}
	});
	context.set_z(da, result);
	return retired();
}

} // namespace

Handler wide_handler(std::uint16_t id) noexcept
{
	switch (id) {
	case form::saddlb_z_zz:
	case form::saddlt_z_zz:
	case form::uaddlb_z_zz:
	case form::uaddlt_z_zz:
	case form::ssublb_z_zz:
	case form::ssublt_z_zz:
	case form::usublb_z_zz:
	case form::usublt_z_zz:
	case form::sabdlb_z_zz:
	case form::sabdlt_z_zz:
	case form::uabdlb_z_zz:
	case form::uabdlt_z_zz:
	case form::saddlbt_z_zz:
	case form::ssublbt_z_zz:
	case form::ssubltb_z_zz:
		return long_arithmetic;
	case form::saddwb_z_zz:
	case form::saddwt_z_zz:
	case form::uaddwb_z_zz:
	case form::uaddwt_z_zz:
	case form::ssubwb_z_zz:
	case form::ssubwt_z_zz:
	case form::usubwb_z_zz:
	case form::usubwt_z_zz:
		return wide_arithmetic;
	case form::sabalb_z_zzz:
	case form::sabalt_z_zzz:
	case form::uabalb_z_zzz:
	case form::uabalt_z_zzz:
		return absolute_difference_accumulate_long;
	case form::smullb_z_zz:
	case form::smullt_z_zz:
	case form::umullb_z_zz:
	case form::umullt_z_zz:
	case form::sqdmullb_z_zz:
	case form::sqdmullt_z_zz:
	case form::pmullb_z_zz:
	case form::pmullt_z_zz:
		return multiply_long;
	case form::smlalb_z_zzz:
	case form::smlalt_z_zzz:
	case form::umlalb_z_zzz:
	case form::umlalt_z_zzz:
	case form::smlslb_z_zzz:
	case form::smlslt_z_zzz:
	case form::umlslb_z_zzz:
	case form::umlslt_z_zzz:
	case form::sqdmlalb_z_zzz:
	case form::sqdmlalt_z_zzz:
	case form::sqdmlslb_z_zzz:
	case form::sqdmlslt_z_zzz:
	case form::sqdmlalbt_z_zzz:
	case form::sqdmlslbt_z_zzz:
		return multiply_accumulate_long;
	case form::smlalb_z_zzzi_s:
	case form::smlalt_z_zzzi_s:
	case form::umlalb_z_zzzi_s:
	case form::umlalt_z_zzzi_s:
	case form::smlslb_z_zzzi_s:
	case form::smlslt_z_zzzi_s:
	case form::umlslb_z_zzzi_s:
	case form::umlslt_z_zzzi_s:
	case form::smullb_z_zzi_s:
	case form::smullt_z_zzi_s:
	case form::umullb_z_zzi_s:
	case form::umullt_z_zzi_s:
	case form::sqdmullb_z_zzi_s:
	case form::sqdmullt_z_zzi_s:
	case form::sqdmlalb_z_zzzi_s:
	case form::sqdmlalt_z_zzzi_s:
	case form::sqdmlslb_z_zzzi_s:
	case form::sqdmlslt_z_zzzi_s:
		return multiply_long_indexed;
	case form::addhnb_z_zz:
	case form::addhnt_z_zz:
	case form::subhnb_z_zz:
	case form::subhnt_z_zz:
	case form::raddhnb_z_zz:
	case form::raddhnt_z_zz:
	case form::rsubhnb_z_zz:
	case form::rsubhnt_z_zz:
		return narrowing_high_half;
	case form::shrnb_z_zi:
	case form::shrnt_z_zi:
	case form::rshrnb_z_zi:
	case form::rshrnt_z_zi:
	case form::sqshrnb_z_zi:
	case form::sqshrnt_z_zi:
	case form::sqrshrnb_z_zi:
	case form::sqrshrnt_z_zi:
	case form::uqshrnb_z_zi:
	case form::uqshrnt_z_zi:
	case form::uqrshrnb_z_zi:
	case form::uqrshrnt_z_zi:
	case form::sqshrunb_z_zi:
	case form::sqshrunt_z_zi:
	case form::sqrshrunb_z_zi:
	case form::sqrshrunt_z_zi:
		return narrowing_shift;
	case form::sqxtnb_z_zz:
	case form::sqxtnt_z_zz:
	case form::uqxtnb_z_zz:
	case form::uqxtnt_z_zz:
	case form::sqxtunb_z_zz:
	case form::sqxtunt_z_zz:
		return saturating_extract_narrow;
	case form::sshllb_z_zi:
	case form::sshllt_z_zi:
	case form::ushllb_z_zi:
	case form::ushllt_z_zi:
		return shift_left_long;
	case form::sqcvtn_z_mz2:
	case form::sqcvtun_z_mz2:
	case form::uqcvtn_z_mz2:
	case form::sqrshrn_z_mz2:
	case form::sqrshrun_z_mz2:
	case form::uqrshrn_z_mz2:
		return narrowing_pair;
	case form::eorbt_z_zz:
	case form::eortb_z_zz:
		return interleaving_exclusive_or;
	case form::sdot_z_zzz:
	case form::udot_z_zzz:
	case form::sdot_z_zzzi_s:
	case form::udot_z_zzzi_s:
	case form::usdot_z_zzz_s:
	case form::usdot_z_zzzi_s:
	case form::sudot_z_zzzi_s:
	case form::sdot_z32_zzz:
	case form::udot_z32_zzz:
	case form::sdot_z32_zzzi:
	case form::udot_z32_zzzi:
		return dot_product;
	case form::cdot_z_zzz:
	case form::cdot_z_zzzi_s:
		return complex_dot_product;
	case form::cmla_z_zzz:
	case form::cmla_z_zzzi_h:
	case form::sqrdcmlah_z_zzz:
	case form::sqrdcmlah_z_zzzi_h:
		return complex_multiply_add;
	default:
		return nullptr;
	}
}

} // namespace uml::cpu::arm64::detail::sve_semantics
