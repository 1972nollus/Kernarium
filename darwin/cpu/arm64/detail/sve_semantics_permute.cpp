// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The semantic library's data-movement forms: broadcasts and copies of
 * immediates, scalars and elements, index vectors, vector addresses, the
 * element permutes (zip, unzip, transpose, reverse, unpack, table lookup,
 * extract, splice, compact, insert), selects, prefixes, and the conditional
 * and unconditional extractions of the last active element.
 */
#include "sve_semantics_internal.hpp"

namespace uml::cpu::arm64::detail::sve_semantics {

namespace {

/* VFPExpandImm(): an 8-bit floating-point immediate as an element of
 * `bits` bits. */
[[nodiscard]] std::uint64_t expand_fp_immediate(
	unsigned imm8, unsigned bits) noexcept
{
	const unsigned exponent_bits = bits == 16U ? 5U :
		bits == 32U                        ? 8U :
						     11U;
	const unsigned fraction_bits = bits - exponent_bits - 1U;
	const std::uint64_t sign = (imm8 >> 7U) & 1U;
	const std::uint64_t b6 = (imm8 >> 6U) & 1U;
	std::uint64_t exponent = (b6 ^ 1U) << (exponent_bits - 1U);
	for (unsigned bit = 2; bit < exponent_bits - 1U; ++bit)
		exponent |= b6 << bit;
	exponent |= (imm8 >> 4U) & 3U;
	const std::uint64_t fraction = std::uint64_t{imm8 & 0xfU}
		<< (fraction_bits - 4U);
	return sign << (bits - 1U) | exponent << fraction_bits | fraction;
}

/* Every element of a vector set to `value`. */
[[nodiscard]] Vector broadcast(unsigned size, std::uint64_t value) noexcept
{
	Vector result;
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>; ++element)
			result.set<T>(element, static_cast<T>(value));
	});
	return result;
}

/* The active elements of Zd set to `value`, the inactive ones kept
 * (merging) or zeroed. */
[[nodiscard]] Vector copy_active(const Vector &destination,
	const Predicate &mask, unsigned size, std::uint64_t value,
	bool merging) noexcept
{
	Vector result = merging ? destination : Vector{};
	with_size(size, [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>; ++element)
			if (active(mask, element, sizeof(T)))
				result.set<T>(element, static_cast<T>(value));
	});
	return result;
}

/* The signed imm8 (bits 12:5) of DUP and CPY (immediate), shifted left 8
 * when sh (bit 13) is set. */
[[nodiscard]] std::uint64_t shifted_immediate(const Context &context) noexcept
{
	return static_cast<std::uint64_t>(sign_extend(context.field(5, 8), 8)
		<< (context.bit(13) ? 8U : 0U));
}

/* DUP (immediate). */
SveResult duplicate_immediate(Context &context)
{
	context.set_z(context.field(0, 5),
		broadcast(context.field(22, 2), shifted_immediate(context)));
	return retired();
}

/* FDUP: the floating-point imm8 in bits 12:5. */
SveResult duplicate_fp_immediate(Context &context)
{
	const unsigned size = context.field(22, 2);
	context.set_z(context.field(0, 5),
		broadcast(size,
			expand_fp_immediate(context.field(5, 8), 8U << size)));
	return retired();
}

/* DUP (scalar): Rn, or SP for 31. */
SveResult duplicate_scalar(Context &context)
{
	context.set_z(context.field(0, 5),
		broadcast(context.field(22, 2),
			context.x_or_sp(context.field(5, 5))));
	return retired();
}

/* DUP (indexed): imm2 (bits 23:22) : tsz (20:16); the lowest set bit of
 * tsz selects the element size, quadwords included, and the bits above it
 * the index. An index past the vector gives zero. */
SveResult duplicate_element(Context &context)
{
	const unsigned tsz = context.field(16, 5);
	const unsigned imm = context.field(22, 2) << 5U | tsz;
	const auto lowest = static_cast<unsigned>(std::countr_zero(tsz));
	const unsigned index = imm >> (lowest + 1U);
	const unsigned bytes = 1U << lowest;
	const Vector n = context.z(context.field(5, 5));
	Vector result;
	if (index < vector_bytes / bytes)
		for (unsigned byte = 0; byte < vector_bytes; ++byte)
			result.bytes[byte] =
				n.bytes[index * bytes + byte % bytes];
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* CPY (immediate, merging and zeroing; Pg in bits 19:16, M in bit 14). */
SveResult copy_immediate(Context &context)
{
	const unsigned d = context.field(0, 5);
	const bool merging = context.row.form == form::cpy_z_p_i;
	context.set_z(d,
		copy_active(context.z(d), context.p(context.field(16, 4)),
			context.field(22, 2), shifted_immediate(context),
			merging));
	return retired();
}

/* FCPY: the floating-point imm8 in bits 12:5, merging, Pg in 19:16. */
SveResult copy_fp_immediate(Context &context)
{
	const unsigned d = context.field(0, 5);
	const unsigned size = context.field(22, 2);
	context.set_z(d,
		copy_active(context.z(d), context.p(context.field(16, 4)), size,
			expand_fp_immediate(context.field(5, 8), 8U << size),
			true));
	return retired();
}

/* CPY (scalar): Rn, or SP for 31, merging, Pg in bits 12:10. */
SveResult copy_scalar(Context &context)
{
	const unsigned d = context.field(0, 5);
	context.set_z(d,
		copy_active(context.z(d), context.p(context.field(10, 3)),
			context.field(22, 2),
			context.x_or_sp(context.field(5, 5)), true));
	return retired();
}

/* CPY (SIMD&FP scalar): the low element of Vn, merging. */
SveResult copy_vector_scalar(Context &context)
{
	const unsigned d = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	context.set_z(d,
		copy_active(context.z(d), context.p(context.field(10, 3)),
			context.field(22, 2), n.get<std::uint64_t>(0), true));
	return retired();
}

/* INDEX: a start and a step, each an imm5 or a general register (XZR for
 * 31): the start in bits 9:5, the step in 20:16. */
SveResult index_vector(Context &context)
{
	const std::uint16_t id = context.row.form;
	const bool start_register =
		id == form::index_z_ri || id == form::index_z_rr;
	const bool step_register =
		id == form::index_z_ir || id == form::index_z_rr;
	const std::uint64_t start = start_register ?
		context.x(context.field(5, 5)) :
		static_cast<std::uint64_t>(sign_extend(context.field(5, 5), 5));
	const std::uint64_t step = step_register ?
		context.x(context.field(16, 5)) :
		static_cast<std::uint64_t>(
			sign_extend(context.field(16, 5), 5));
	Vector result;
	with_size(context.field(22, 2), [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>; ++element)
			result.set<T>(element,
				static_cast<T>(start + element * step));
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* ADR: Zn plus Zm's offsets scaled by 1 << msz (bits 11:10): same-size
 * words or doublewords (sz in bit 22), or doublewords with sign- or
 * zero-extended word offsets (bits 23:22 00 and 01). */
SveResult vector_address(Context &context)
{
	const unsigned kind = context.field(22, 2);
	const unsigned scale = context.field(10, 2);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result;
	if (kind == 2U) {
		for (unsigned element = 0; element < 4U; ++element)
			result.set<std::uint32_t>(element,
				static_cast<std::uint32_t>(
					n.get<std::uint32_t>(element) +
					(m.get<std::uint32_t>(element)
						<< scale)));
	} else {
		for (unsigned element = 0; element < 2U; ++element) {
			const std::uint64_t offset =
				m.get<std::uint64_t>(element);
			const std::uint64_t extended = kind == 3U ?
				offset :
				kind == 0U ?
				static_cast<std::uint64_t>(
					static_cast<std::int64_t>(
						static_cast<std::int32_t>(
							offset))) :
				offset & 0xffff'ffffU;
			result.set<std::uint64_t>(element,
				n.get<std::uint64_t>(element) +
					(extended << scale));
		}
	}
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* INSR (scalar, XZR for 31, and SIMD&FP scalar): Zdn shifted up one
 * element with the scalar in element 0. */
SveResult insert(Context &context)
{
	const unsigned size = context.field(22, 2);
	const unsigned bytes = esize_of(size);
	const unsigned dn = context.field(0, 5);
	const std::uint64_t value = context.row.form == form::insr_z_r ?
		context.x(context.field(5, 5)) :
		context.z(context.field(5, 5)).get<std::uint64_t>(0);
	const Vector source = context.z(dn);
	Vector result;
	std::memcpy(result.bytes.data() + bytes, source.bytes.data(),
		vector_bytes - bytes);
	std::memcpy(result.bytes.data(), &value, bytes);
	context.set_z(dn, result);
	return retired();
}

/* MOVPRFX (predicated, M in bit 16, and unpredicated). */
SveResult move_prefix(Context &context)
{
	const unsigned d = context.field(0, 5);
	const Vector n = context.z(context.field(5, 5));
	if (context.row.form == form::movprfx_z_z) {
		context.set_z(d, n);
		return retired();
	}
	const Predicate &mask = context.p(context.field(10, 3));
	Vector result = context.bit(16) ? context.z(d) : Vector{};
	with_size(context.field(22, 2), [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>; ++element)
			if (active(mask, element, sizeof(T)))
				result.set<T>(element, n.get<T>(element));
	});
	context.set_z(d, result);
	return retired();
}

/* SEL: Pg in bits 13:10. */
SveResult select(Context &context)
{
	const Predicate &mask = context.p(context.field(10, 4));
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result;
	with_size(context.field(22, 2), [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>; ++element)
			result.set<T>(element,
				active(mask, element, sizeof(T)) ?
					n.get<T>(element) :
					m.get<T>(element));
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* REV (vector). */
SveResult reverse_elements(Context &context)
{
	const Vector n = context.z(context.field(5, 5));
	Vector result;
	with_size(context.field(22, 2), [&]<typename T>() {
		for (unsigned element = 0; element < elements_of<T>; ++element)
			result.set<T>(element,
				n.get<T>(elements_of<T> - 1U - element));
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* SUNPKLO, SUNPKHI, UUNPKLO and UUNPKHI: half of Zn's elements widened to
 * the size in bits 23:22; high in bit 16, unsigned in 17. */
SveResult unpack(Context &context)
{
	const unsigned size = context.field(22, 2);
	const bool high = context.bit(16);
	const bool is_signed = !context.bit(17);
	const Vector n = context.z(context.field(5, 5));
	Vector result;
	with_size(size, [&]<typename T>() {
		if constexpr (sizeof(T) > 1) {
			using H = narrower_of<T>;
			const unsigned base = high ? elements_of<T> : 0U;
			for (unsigned element = 0; element < elements_of<T>;
				++element)
				result.set<T>(element,
					static_cast<T>(value_of(
						n.get<H>(base + element),
						is_signed)));
		}
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* ZIP1, ZIP2, UZP1, UZP2, TRN1 and TRN2 (vectors): bits 12:10 the
 * permute. */
SveResult permute_vectors(Context &context)
{
	const unsigned op = context.field(10, 3);
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result;
	with_size(context.field(22, 2), [&]<typename T>() {
		constexpr unsigned count = elements_of<T>;
		const unsigned part = op & 1U;
		for (unsigned element = 0; element < count; ++element) {
			T value;
			switch (op >> 1U) {
			case 0: { /* ZIP */
				const unsigned source =
					part * count / 2U + element / 2U;
				value = element % 2U == 0U ? n.get<T>(source) :
							     m.get<T>(source);
				break;
			}
			case 1: { /* UZP */
				const unsigned source = 2U * element + part;
				value = source < count ?
					n.get<T>(source) :
					m.get<T>(source - count);
				break;
			}
			default: { /* TRN */
				const unsigned base = element & ~1U;
				value = element % 2U == 0U ?
					n.get<T>(base + part) :
					m.get<T>(base + part);
				break;
			}
			}
			result.set<T>(element, value);
		}
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* TBL (one and two registers) and TBX (one register): an out-of-range
 * index gives zero, or for TBX keeps Zd's element. */
SveResult table_lookup(Context &context)
{
	const std::uint16_t id = context.row.form;
	const bool extension = id == form::tbx_z_zz;
	const bool double_table =
		id == form::tbl_z_zz_1 && context.field(10, 3) == 2U;
	const unsigned d = context.field(0, 5);
	const unsigned n = context.field(5, 5);
	std::array<std::uint8_t, 2U * vector_bytes> table{};
	std::memcpy(table.data(), context.z(n).bytes.data(), vector_bytes);
	if (double_table)
		std::memcpy(table.data() + vector_bytes,
			context.z((n + 1U) % 32U).bytes.data(), vector_bytes);
	const Vector indexes = context.z(context.field(16, 5));
	Vector result = context.z(d);
	with_size(context.field(22, 2), [&]<typename T>() {
		const unsigned entries =
			(double_table ? 2U : 1U) * elements_of<T>;
		for (unsigned element = 0; element < elements_of<T>;
			++element) {
			const T index = indexes.get<T>(element);
			if (index < entries) {
				T value;
				std::memcpy(&value,
					table.data() + index * sizeof(T),
					sizeof(T));
				result.set<T>(element, value);
			} else if (!extension) {
				result.set<T>(element, T{0});
			}
		}
	});
	context.set_z(d, result);
	return retired();
}

/* EXT (constructive, bit 22 set: Zd from Zn, Zn+1; destructive: Zdn from
 * Zdn, Zm): imm8h (bits 20:16) : imm8l (12:10) bytes in, or none past the
 * vector. */
SveResult extract(Context &context)
{
	const bool constructive = context.bit(22);
	const unsigned d = context.field(0, 5);
	const unsigned first = constructive ? context.field(5, 5) : d;
	const unsigned second =
		constructive ? (first + 1U) % 32U : context.field(5, 5);
	unsigned position = context.field(16, 5) << 3U | context.field(10, 3);
	if (position >= vector_bytes)
		position = 0;
	std::array<std::uint8_t, 2U * vector_bytes> joined{};
	std::memcpy(joined.data(), context.z(first).bytes.data(), vector_bytes);
	std::memcpy(joined.data() + vector_bytes,
		context.z(second).bytes.data(), vector_bytes);
	Vector result;
	std::memcpy(
		result.bytes.data(), joined.data() + position, vector_bytes);
	context.set_z(d, result);
	return retired();
}

/* The last element `mask` makes active, or -1 (LastActiveElement()). */
[[nodiscard]] int last_active(const Predicate &mask, unsigned esize) noexcept
{
	for (int element = static_cast<int>(vector_bytes / esize) - 1;
		element >= 0; --element)
		if (active(mask, static_cast<unsigned>(element), esize))
			return element;
	return -1;
}

/* SPLICE (constructive, bit 16 set: Zd from Zn, Zn+1; destructive: Zdn
 * from Zdn, Zm): the segment of the first operand from its first to its
 * last active element (Pv in bits 12:10), then the second operand's
 * leading elements. */
SveResult splice(Context &context)
{
	const unsigned size = context.field(22, 2);
	const unsigned esize = esize_of(size);
	const bool constructive = context.bit(16);
	const unsigned d = context.field(0, 5);
	const unsigned first = constructive ? context.field(5, 5) : d;
	const unsigned second =
		constructive ? (first + 1U) % 32U : context.field(5, 5);
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector n = context.z(first);
	const Vector m = context.z(second);
	const int last = last_active(mask, esize);
	Vector result;
	with_size(size, [&]<typename T>() {
		unsigned x = 0;
		bool started = false;
		for (int element = 0; element <= last; ++element) {
			started = started ||
				active(mask, static_cast<unsigned>(element),
					sizeof(T));
			if (started)
				result.set<T>(x++,
					n.get<T>(static_cast<unsigned>(
						element)));
		}
		for (unsigned element = 0; x < elements_of<T>; ++element)
			result.set<T>(x++, m.get<T>(element));
	});
	context.set_z(d, result);
	return retired();
}

/* COMPACT: the active words or doublewords (32 << sz, sz in bit 22) packed
 * into the low elements, the rest zero. */
SveResult compact(Context &context)
{
	const unsigned size = context.bit(22) ? 3U : 2U;
	const Predicate &mask = context.p(context.field(10, 3));
	const Vector n = context.z(context.field(5, 5));
	Vector result;
	with_size(size, [&]<typename T>() {
		unsigned x = 0;
		for (unsigned element = 0; element < elements_of<T>; ++element)
			if (active(mask, element, sizeof(T)))
				result.set<T>(x++, n.get<T>(element));
	});
	context.set_z(context.field(0, 5), result);
	return retired();
}

/* The element CLASTx and LASTx extract, or -1 when CLASTx has no active
 * element: B (bit 16) takes the last active element, A the one after it,
 * wrapping; LASTB with none active takes the last element. */
[[nodiscard]] int extracted_element(
	const Context &context, unsigned esize, bool conditional) noexcept
{
	const int elements = static_cast<int>(vector_bytes / esize);
	int last = last_active(context.p(context.field(10, 3)), esize);
	if (conditional && last < 0)
		return -1;
	if (context.bit(16))
		return last < 0 ? elements - 1 : last;
	++last;
	return last >= elements ? 0 : last;
}

/* The zero-extended element `element` of `vector` for the size field
 * `size`. */
[[nodiscard]] std::uint64_t element_of(
	const Vector &vector, unsigned size, unsigned element) noexcept
{
	return with_size(size, [&]<typename T>() -> std::uint64_t {
		return vector.get<T>(element);
	});
}

/* CLASTA, CLASTB (general register: Rdn with XZR for 31, written as Wdn
 * below doublewords; SIMD&FP scalar; vector) and LASTA, LASTB (general
 * register and SIMD&FP scalar). */
SveResult extract_last(Context &context)
{
	const std::uint16_t id = context.row.form;
	const unsigned size = context.field(22, 2);
	const unsigned esize = esize_of(size);
	const bool conditional = id == form::clasta_r_p_z ||
		id == form::clastb_r_p_z || id == form::clasta_v_p_z ||
		id == form::clastb_v_p_z || id == form::clasta_z_p_zz ||
		id == form::clastb_z_p_zz;
	const bool general = id == form::clasta_r_p_z ||
		id == form::clastb_r_p_z || id == form::lasta_r_p_z ||
		id == form::lastb_r_p_z;
	const bool vector_form =
		id == form::clasta_z_p_zz || id == form::clastb_z_p_zz;
	const unsigned d = context.field(0, 5);
	const Vector source = context.z(context.field(5, 5));
	const int element = extracted_element(context, esize, conditional);
	std::uint64_t value;
	if (element >= 0) {
		value = element_of(
			source, size, static_cast<unsigned>(element));
	} else if (general) {
		value = context.x(d) &
			(esize == 8U ? ~std::uint64_t{0} :
				       (std::uint64_t{1} << (8U * esize)) - 1U);
	} else if (vector_form) {
		return retired();
	} else {
		value = element_of(context.z(d), size, 0);
	}
	if (vector_form)
		context.set_z(d, broadcast(size, value));
	else if (general)
		context.set_x(d, value);
	else
		context.set_v_scalar(d, value, esize);
	return retired();
}

} // namespace

Handler permute_handler(std::uint16_t id) noexcept
{
	switch (id) {
	case form::dup_z_i:
		return duplicate_immediate;
	case form::fdup_z_i:
		return duplicate_fp_immediate;
	case form::dup_z_r:
		return duplicate_scalar;
	case form::dup_z_zi:
		return duplicate_element;
	case form::cpy_z_p_i:
	case form::cpy_z_o_i:
		return copy_immediate;
	case form::fcpy_z_p_i:
		return copy_fp_immediate;
	case form::cpy_z_p_r:
		return copy_scalar;
	case form::cpy_z_p_v:
		return copy_vector_scalar;
	case form::index_z_ii:
	case form::index_z_ir:
	case form::index_z_ri:
	case form::index_z_rr:
		return index_vector;
	case form::adr_z_az_sd_same_scaled:
		return vector_address;
	case form::insr_z_r:
	case form::insr_z_v:
		return insert;
	case form::movprfx_z_p_z:
	case form::movprfx_z_z:
		return move_prefix;
	case form::sel_z_p_zz:
		return select;
	case form::rev_z_z:
		return reverse_elements;
	case form::sunpkhi_z_z:
	case form::uunpkhi_z_z:
		return unpack;
	case form::zip2_z_zz:
	case form::uzp1_z_zz:
	case form::trn1_z_zz:
		return permute_vectors;
	case form::tbl_z_zz_1:
	case form::tbx_z_zz:
		return table_lookup;
	case form::ext_z_zi_con:
		return extract;
	case form::splice_z_p_zz_con:
		return splice;
	case form::compact_z_p_z:
		return compact;
	case form::clasta_r_p_z:
	case form::clastb_r_p_z:
	case form::clasta_v_p_z:
	case form::clastb_v_p_z:
	case form::clasta_z_p_zz:
	case form::clastb_z_p_zz:
	case form::lasta_r_p_z:
	case form::lastb_r_p_z:
	case form::lasta_v_p_z:
	case form::lastb_v_p_z:
		return extract_last;
	default:
		return nullptr;
	}
}

} // namespace uml::cpu::arm64::detail::sve_semantics
