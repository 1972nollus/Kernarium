// SPDX-License-Identifier: GPL-2.0-or-later
/* Translation of the non-streaming SVE forms for a program translated for
 * SVCR.SM = 0. decode_sme_space reaches decode_sve_space only from its cold
 * path, and only where the host the engine translates for has the SVE
 * profile, which the runtime presents only from an engine at
 * sve_support complete (cpu::engine_host_capabilities).
 *
 * A word takes the SVE path when its encoding-table row executes outside
 * streaming mode only with FEAT_SVE: the row's non-streaming requirement
 * decides whether the virtual CPU implements it, and the generated header
 * (engine/generate_sve.py) gives the form the engine runs for it. A row
 * with no generated form runs through the shared semantic library
 * (sve_library, sve_helpers.cpp), which implements every row decoded here,
 * so the engine is at sve_support complete. The SME rows that execute in
 * either mode, such as RDSVL, stay with the SME decoder. A program holding
 * such a word accesses the FP state before it runs, since P and FFR travel
 * with it, and enters through the SVE link entry, which marks the task as
 * having executed SVE. */
#include "program_builder_decode.hpp"
#include "gadget_selection_api.hpp"
#include "sve_forms.hpp"
#include "sve_helpers.hpp"
#include "sve_layout.h"

#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/layout/assembly_offsets.h"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/arm64/sve_profile.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

extern "C" {
/* engine/sve.inc: the generated entries, in the order of the header, the
 * entry of a variant the generator leaves to the semantic library, and the
 * gadget that runs a word through that library. */
extern const std::uintptr_t uml_arm64_gadget_sve_generated_entries[];
extern const std::uintptr_t uml_arm64_gadget_sve_generated_entries_end[];
void uml_arm64_gadget_sve_generated_absent();
void uml_arm64_gadget_sve_library();
void uml_arm64_gadget_sve_sp_check();
}

namespace uml::cpu::arm64::gadgets::builder_detail {
namespace {

using detail::SmeOperandKind;
using selection::emit;
using sve::Form;
using sve::Lowering;

static_assert(sve::row_forms.size() == detail::sme_encodings.size(),
	"the SVE forms are generated from this encoding table");

/* One WHILE* table entry (sve_layout.h). */
struct alignas(1U << UML_GADGET_SVE_WHILE_ENTRY_SHIFT) WhileEntry final {
	SvePredicate mask;
	std::uint32_t nzcv;
};
static_assert(sizeof(WhileEntry) == 1U << UML_GADGET_SVE_WHILE_ENTRY_SHIFT);
static_assert(offsetof(WhileEntry, mask) == UML_GADGET_SVE_WHILE_MASK &&
	offsetof(WhileEntry, nzcv) == UML_GADGET_SVE_WHILE_NZCV);

using WhileTable = std::array<WhileEntry, UML_GADGET_SVE_WHILE_COUNTS>;
static_assert(UML_GADGET_SVE_WHILE_COUNTS == sve_vector_length + 1U);

/* Entry `count` holds the predicate whose first `count` elements of
 * `esize` bytes are active, or with `trailing` its last `count`, and the
 * NZCV that PredTest() leaves for it under an all-true mask. A count past
 * the element count is the whole vector. */
[[nodiscard]] constexpr WhileTable while_table(unsigned esize, bool trailing)
{
	WhileTable table{};
	const unsigned elements = sve_vector_length / esize;
	for (unsigned count = 0; count < table.size(); ++count) {
		const unsigned active = std::min(count, elements);
		auto &entry = table[count];
		if (!trailing) {
			const auto leading = sve_leading_predicate(esize, active);
			entry.mask = leading.predicate;
			entry.nzcv = leading.nzcv;
			continue;
		}
		for (unsigned element = elements - active; element < elements;
		     ++element)
			entry.mask[element * esize] = 0xffU;
		/* N: element 0 is active; Z: none is; C: the last is not. */
		entry.nzcv = (active == elements ? 0x8000'0000U : 0U) |
			(active == 0U ? 0x6000'0000U : 0U);
	}
	return table;
}

/* [trailing][size field] */
alignas(64) constexpr std::array<std::array<WhileTable, 4>, 2> while_tables{{
	{{while_table(1, false), while_table(2, false), while_table(4, false),
		while_table(8, false)}},
	{{while_table(1, true), while_table(2, true), while_table(4, true),
		while_table(8, true)}},
}};
static_assert(while_tables[1][2][4].nzcv == 0x8000'0000U &&
	while_tables[1][2][3].nzcv == 0U &&
	while_tables[1][2][3].mask[4] == 0xffU &&
	while_tables[1][2][3].mask[0] == 0U);

/* Constants the gadgets read through a pointer in their stream
 * (engine/generate_sve.py, TABLE_*): the bytes 0-15; the TBL indexes that
 * pack the active elements of a vector of words, by the mask of which of
 * its four elements are active; and the same for doublewords and two. A
 * byte past the packed elements is out of range for TBL, which gives zero. */
struct SveTables final {
	std::array<std::uint8_t, 16> iota{};
	std::array<std::array<std::uint8_t, 16>, 16> compact_word{};
	std::array<std::array<std::uint8_t, 16>, 4> compact_doubleword{};
};

[[nodiscard]] constexpr SveTables make_sve_tables() noexcept
{
	SveTables tables;
	for (unsigned byte = 0; byte < 16U; ++byte)
		tables.iota[byte] = static_cast<std::uint8_t>(byte);
	for (unsigned mask = 0; mask < 16U; ++mask) {
		unsigned out = 0;
		for (auto &index : tables.compact_word[mask])
			index = 0xffU;
		for (unsigned element = 0; element < 4U; ++element) {
			if ((mask >> element & 1U) == 0U)
				continue;
			for (unsigned byte = 0; byte < 4U; ++byte)
				tables.compact_word[mask][out * 4U + byte] =
					static_cast<std::uint8_t>(element * 4U + byte);
			++out;
		}
	}
	for (unsigned mask = 0; mask < 4U; ++mask) {
		unsigned out = 0;
		for (auto &index : tables.compact_doubleword[mask])
			index = 0xffU;
		for (unsigned element = 0; element < 2U; ++element) {
			if ((mask >> element & 1U) == 0U)
				continue;
			for (unsigned byte = 0; byte < 8U; ++byte)
				tables.compact_doubleword[mask][out * 8U + byte] =
					static_cast<std::uint8_t>(element * 8U + byte);
			++out;
		}
	}
	return tables;
}

alignas(16) constexpr SveTables sve_tables = make_sve_tables();
static_assert(offsetof(SveTables, compact_word) == 16 &&
	offsetof(SveTables, compact_doubleword) == 16 + 16 * 16);

/* The word's bits under `mask`, packed from the least significant up. */
[[nodiscard]] constexpr std::uint32_t gather(std::uint32_t word,
	std::uint32_t mask) noexcept
{
	std::uint32_t result = 0;
	for (unsigned position = 0; mask != 0U; mask &= mask - 1U, ++position)
		result |= ((word >> std::countr_zero(mask)) & 1U) << position;
	return result;
}

[[nodiscard]] constexpr std::int64_t sign_extend(std::uint32_t value,
	unsigned width) noexcept
{
	const auto shift = 64U - width;
	return static_cast<std::int64_t>(std::uint64_t{value} << shift) >> shift;
}

/* The element of `esize` bytes repeated across 64 bits. */
[[nodiscard]] constexpr std::uint64_t replicate(std::uint64_t value,
	unsigned esize) noexcept
{
	if (esize == 8U)
		return value;
	value &= (std::uint64_t{1} << (esize * 8U)) - 1U;
	for (unsigned width = esize * 8U; width < 64U; width *= 2U)
		value |= value << width;
	return value;
}

/* The CpuState offset of a slot's register: Z and V in `vectors`, P in the
 * mask images, a general register in `registers` with XZR the zero slot,
 * or SP, which follows the 31 general registers. */
[[nodiscard]] std::uint64_t slot_offset(SmeOperandKind kind,
	unsigned number) noexcept
{
	switch (kind) {
	case SmeOperandKind::z:
	case SmeOperandKind::v:
		return UML_GADGET_CPU_V0 + std::uint64_t{number} * 16U;
	case SmeOperandKind::p:
		return UML_ARM64_CPU_SVE_P + std::uint64_t{number} * 16U;
	case SmeOperandKind::gpr:
		if (number == 31U)
			return UML_GADGET_CPU_ZERO;
		break;
	case SmeOperandKind::gpr_or_sp:
	case SmeOperandKind::za_tile:
		break;
	}
	return std::uint64_t{number} * sizeof(std::uint64_t);
}

[[nodiscard]] unsigned slot_number(const Form &form, unsigned slot,
	std::uint32_t word) noexcept
{
	return detail::sme_operand_number(sve::slots[form.slot_begin + slot],
		word);
}

[[nodiscard]] std::uint64_t packed_slots(const Form &form,
	std::uint32_t word) noexcept
{
	std::uint64_t packed = 0;
	for (unsigned slot = 0; slot < form.slot_count; ++slot)
		packed |= slot_offset(sve::slots[form.slot_begin + slot].kind,
			slot_number(form, slot, word)) << (16U * slot);
	return packed;
}

[[nodiscard]] std::span<const std::uintptr_t> generated_entries() noexcept
{
	return {uml_arm64_gadget_sve_generated_entries,
		uml_arm64_gadget_sve_generated_entries_end};
}

[[nodiscard]] unsigned element_bytes(std::uint32_t word) noexcept
{
	return 1U << ((word >> 22) & 3U);
}

/* CNT*, INC*, DEC* and the vector INC* and DEC*: the elements the pattern
 * names, times the multiplier. */
[[nodiscard]] std::uint64_t element_count(std::uint32_t word) noexcept
{
	return std::uint64_t{predicate_pattern_count((word >> 5) & 31U,
		sve_vector_length / element_bytes(word))} *
		(((word >> 16) & 15U) + 1U);
}

void emit_words(DecodeContext &context, std::uint64_t value)
{
	emit(context.words, static_cast<std::uintptr_t>(value));
}

void emit_predicate(DecodeContext &context, const SvePredicate &predicate)
{
	std::uint64_t low = 0;
	std::uint64_t high = 0;
	std::memcpy(&low, predicate.data(), sizeof(low));
	std::memcpy(&high, predicate.data() + sizeof(low), sizeof(high));
	emit_words(context, low);
	emit_words(context, high);
}

/* MOV Xd, #value, as the base part runs it; XZR discards it. */
DecodeResult emit_move(DecodeContext &context, unsigned rd,
	std::uint64_t value)
{
	if (rd == 31U) {
		emit(context.words, selection::uml_arm64_gadget_nop);
		return DecodeResult::decoded;
	}
	emit(context.words, selection::uml_arm64_gadget_move_wide_constant);
	emit_words(context, rd * sizeof(std::uint64_t));
	emit_words(context, value);
	return DecodeResult::decoded;
}

/* ADD or SUB (immediate) of |value| < 4096, 64-bit, flags unchanged, as the
 * base part runs it: register 31 is SP. */
DecodeResult emit_add(DecodeContext &context, unsigned rd, unsigned rn,
	std::int64_t value)
{
	const bool subtract = value < 0;
	const auto magnitude = static_cast<std::uintptr_t>(
		subtract ? -value : value);
	emit(context.words, rd == rn && rd != 31U ?
		selection::add_sub_immediate_inplace_gadget(true, subtract, false,
			rd) :
		selection::add_sub_immediate_gadget(true, subtract, false,
			rn == 31U, rd == 31U));
	emit(context.words, rd | (static_cast<std::uintptr_t>(rn) << 8) |
		(magnitude << 16));
	return DecodeResult::decoded;
}

/* The forms that select base entries. */
DecodeResult emit_base(DecodeContext &context, const Form &form)
{
	const auto word = context.instruction;
	const unsigned rd = slot_number(form, 0, word);
	switch (form.lowering) {
	case Lowering::count_move:
		/* CNT[BHWD] Xd, or RDVL Xd, #imm. */
		return emit_move(context, rd, form.argument != 0U ?
			static_cast<std::uint64_t>(sign_extend((word >> 5) & 63U, 6) *
				sve_vector_length) :
			element_count(word));
	case Lowering::count_add: {
		/* INC[BHWD] or DEC[BHWD] Xdn; XZR discards it. */
		if (rd == 31U)
			return emit_move(context, rd, 0);
		const auto count = static_cast<std::int64_t>(element_count(word));
		return emit_add(context, rd, rd,
			form.argument != 0U ? -count : count);
	}
	case Lowering::vector_length_add: {
		/* ADDVL or ADDPL Xd|SP, Xn|SP, #imm: VL or PL bytes each. */
		const auto step = form.argument != 0U ?
			sve_vector_length / 8U : sve_vector_length;
		return emit_add(context, rd, slot_number(form, 1, word),
			sign_extend((word >> 5) & 63U, 6) * step);
	}
	case Lowering::prefetch:
		/* PRF[BHWD]: a hint, which never faults. */
		emit(context.words, selection::uml_arm64_gadget_nop);
		return DecodeResult::decoded;
	default:
		return implementation_gap(context);
	}
}

/* A contiguous memory form's immediate, its bits concatenated in order and
 * sign-extended where the form says. */
[[nodiscard]] std::int64_t memory_immediate(const detail::SmeMemory &memory,
	std::uint32_t word) noexcept
{
	std::uint32_t value = 0;
	unsigned width = 0;
	for (unsigned index = 0; index < memory.immediate_segment_count;
	     ++index) {
		const auto &segment = memory.immediate[index];
		value = (value << segment.width) |
			detail::sme_segment_value(segment, word);
		width += segment.width;
	}
	if (width == 0U)
		return 0;
	return memory.immediate_signed ? sign_extend(value, width) :
		static_cast<std::int64_t>(value);
}

/* The bytes a contiguous form's immediate adds to its base: the immediate
 * times the access's footprint, its memory element, or 16. A register
 * index is scaled in the gadget. */
[[nodiscard]] std::uint64_t memory_offset(const detail::SmeMemory &memory,
	std::uint32_t word) noexcept
{
	const unsigned esize = detail::sme_memory_esize(memory, word) / 8U;
	const unsigned msize = memory.msize / 8U;
	std::uint64_t unit = 0;
	switch (memory.offset) {
	case detail::SmeAddressOffset::immediate_footprint:
		if (memory.shape == detail::SmeMemoryShape::fill_z)
			unit = sve_vector_length;
		else if (memory.shape == detail::SmeMemoryShape::fill_p)
			unit = sve_vector_length / 8U;
		else
			unit = std::uint64_t{memory.registers} *
				(sve_vector_length / esize) * msize;
		break;
	case detail::SmeAddressOffset::immediate_element:
		unit = msize;
		break;
	case detail::SmeAddressOffset::immediate_16:
		unit = 16U;
		break;
	default:
		return 0;
	}
	return static_cast<std::uint64_t>(memory_immediate(memory, word)) *
		unit;
}

/* The TBL indexes of DUP Zd.T, Zn.T[imm]: each byte of the element the
 * index names, or out of range (zero) past the vector. */
[[nodiscard]] SvePredicate broadcast_indexes(std::uint32_t word) noexcept
{
	const unsigned field = (((word >> 22) & 3U) << 5) | ((word >> 16) & 31U);
	const auto lowest = static_cast<unsigned>(std::countr_zero(field & 31U));
	const unsigned esize = 1U << lowest;
	const unsigned index = field >> (lowest + 1U);
	SvePredicate indexes{};
	for (unsigned byte = 0; byte < sve_vector_length; ++byte)
		indexes[byte] = static_cast<std::uint8_t>(
			(index + 1U) * esize <= sve_vector_length ?
				index * esize + byte % esize : 0xffU);
	return indexes;
}

/* The TBL indexes of EXT: the bytes of the pair (first, second) from the
 * immediate's position, which is 0 past the vector. */
[[nodiscard]] SvePredicate extract_indexes(std::uint32_t word) noexcept
{
	unsigned position = (((word >> 16) & 31U) << 3) | ((word >> 10) & 7U);
	if (position >= sve_vector_length)
		position = 0;
	SvePredicate indexes{};
	for (unsigned byte = 0; byte < sve_vector_length; ++byte)
		indexes[byte] = static_cast<std::uint8_t>(position + byte);
	return indexes;
}

/* The immediates after the slot offsets, or nothing for a reserved one. */
[[nodiscard]] bool emit_immediates(DecodeContext &context, const Form &form)
{
	const auto word = context.instruction;
	const unsigned esize = element_bytes(word);
	switch (form.lowering) {
	case Lowering::predicate_constant:
	case Lowering::predicate_constant_flags: {
		const auto count = predicate_pattern_count((word >> 5) & 31U,
			sve_vector_length / esize);
		const auto constant = sve_leading_predicate(esize, count);
		emit_predicate(context, constant.predicate);
		/* PredTest(result, result, esize). */
		if (form.lowering == Lowering::predicate_constant_flags)
			emit_words(context, count != 0U ? 0x8000'0000U :
				0x6000'0000U);
		return true;
	}
	case Lowering::while_compare:
	case Lowering::while_conflict:
		emit(context.words, reinterpret_cast<std::uintptr_t>(
			while_tables[form.argument & 1U][(word >> 22) & 3U].data()));
		return true;
	case Lowering::vector_count:
		emit_words(context, element_count(word));
		return true;
	case Lowering::integer_immediate:
		emit_words(context, replicate(static_cast<std::uint64_t>(
			sign_extend((word >> 5) & 0xffU, 8) *
			(((word >> 13) & 1U) != 0U ? 256 : 1)), esize));
		return true;
	case Lowering::unsigned_immediate:
		/* imm8, shifted left by 8 when bit 13 (sh) is set. */
		emit_words(context, replicate(std::uint64_t{(word >> 5) & 0xffU} <<
			(((word >> 13) & 1U) != 0U ? 8U : 0U), esize));
		return true;
	case Lowering::bitmask_immediate: {
		const auto bits = detail::decode_logical_immediate(true,
			(word >> 17) & 1U, (word >> 11) & 63U, (word >> 5) & 63U);
		if (!bits)
			return false;
		emit_words(context, *bits);
		return true;
	}
	case Lowering::compare_signed_immediate:
		/* imm5, sign-extended to the element size. */
		emit_words(context, replicate(static_cast<std::uint64_t>(
			sign_extend((word >> 16) & 31U, 5)), esize));
		return true;
	case Lowering::compare_unsigned_immediate:
		/* imm7. */
		emit_words(context, replicate((word >> 14) & 127U, esize));
		return true;
	case Lowering::index_immediate: {
		/* INDEX with an immediate start (argument 0) or step (1), which
		 * the gadget adds to its register operand, or both (2), when the
		 * whole vector is a constant. */
		const auto start = static_cast<std::uint64_t>(
			sign_extend((word >> 5) & 31U, 5));
		const auto step = static_cast<std::uint64_t>(
			sign_extend((word >> 16) & 31U, 5));
		if (form.argument == 0U) {
			emit_words(context, start);
		} else if (form.argument == 1U) {
			emit_words(context, step);
		} else if (form.argument == 3U) {
			/* Both are registers. */
		} else {
			std::array<std::uint64_t, 2> lanes{};
			const unsigned bits = esize * 8U;
			const std::uint64_t mask = bits == 64U ?
				~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1U;
			for (unsigned element = 0;
			     element < sve_vector_length / esize; ++element) {
				const auto value =
					(start + std::uint64_t{element} * step) & mask;
				const unsigned bit = element * bits;
				lanes[bit / 64U] |= value << (bit % 64U);
			}
			emit_words(context, lanes[0]);
			emit_words(context, lanes[1]);
			return true;
		}
		/* A byte or halfword series adds the step times the byte
		 * numbers. */
		if (esize < 4U)
			emit(context.words, reinterpret_cast<std::uintptr_t>(
				sve_tables.iota.data()));
		return true;
	}
	case Lowering::tables:
		emit(context.words, reinterpret_cast<std::uintptr_t>(
			&sve_tables));
		return true;
	case Lowering::float_immediate:
		emit_words(context, replicate(selection::expand_vfp_immediate(
			static_cast<std::uint8_t>((word >> 5) & 0xffU), esize * 8U),
			esize));
		return true;
	case Lowering::index_table:
		emit_predicate(context, broadcast_indexes(word));
		return true;
	case Lowering::extract_table:
		emit_predicate(context, extract_indexes(word));
		return true;
	case Lowering::plain:
	case Lowering::count_move:
	case Lowering::count_add:
	case Lowering::vector_length_add:
	case Lowering::memory:
	case Lowering::prefetch:
		return true;
	}
	return true;
}

/* A WHILE* followed by B.cond on one NZCV bit runs as one entry whose
 * branch falls through to the rest of the block, translated inline. */
[[nodiscard]] std::optional<std::uint32_t> fused_condition(
	const DecodeContext &context, const Form &form) noexcept
{
	if (form.lowering != Lowering::while_compare || !context.resolve_link ||
	    context.following.empty() || context.suffixes.full() ||
	    (context.following.front() & 0xff00'0010U) != 0x5400'0000U)
		return std::nullopt;
	const auto condition = context.following.front() & 15U;
	const auto found = std::ranges::find(sve::fused_conditions, condition);
	if (found == sve::fused_conditions.end())
		return std::nullopt;
	return static_cast<std::uint32_t>(found - sve::fused_conditions.begin());
}

/* The word, whose row is detail::sme_encodings[index], through the
 * semantic library. The word goes in the operand's high half: an SVE word
 * is at least 0x0400'0000, so the operand is above every user address and
 * never reads as a gadget entry. */
DecodeResult emit_library(DecodeContext &context, std::size_t index)
{
	if (!detail::sve_semantics_covers(detail::sme_encodings[index]))
		return implementation_gap(context);
	emit(context.words, uml_arm64_gadget_sve_library);
	emit(context.words, (static_cast<std::uint64_t>(context.instruction)
		<< 32) | static_cast<std::uint64_t>(index));
	emit(context.words, context.address);
	return DecodeResult::decoded;
}

DecodeResult emit_form(DecodeContext &context, const Form &form,
	std::size_t index)
{
	const auto word = context.instruction;
	if (form.variants == 0U)
		return emit_base(context, form);
	if ((form.flags & sve::discard_on_zero) != 0U &&
	    slot_number(form, 0, word) == 31U) {
		emit(context.words, selection::uml_arm64_gadget_nop);
		return DecodeResult::decoded;
	}
	const auto fused = fused_condition(context, form);
	auto variant = gather(word, form.variant_mask);
	if (fused)
		variant += 2U + 2U * *fused;
	const auto entries = generated_entries();
	if (variant >= form.variants ||
	    form.first_entry + variant >= entries.size())
		return implementation_gap(context);
	const auto entry = entries[form.first_entry + variant];
	if (entry == reinterpret_cast<std::uintptr_t>(
		    uml_arm64_gadget_sve_generated_absent))
		return emit_library(context, index);
	const auto *const memory = form.lowering == Lowering::memory ?
		detail::sme_encoding_memory_of(detail::sme_encodings[index]) :
		nullptr;
	const auto start = context.words.size();
	if (memory != nullptr) {
		/* An SP base is checked first (sve_sp_check). */
		const auto *const operands = detail::sme_encoding_operands_of(
			detail::sme_encodings[index]);
		const auto &base = operands[memory->base_operand];
		if (base.kind == SmeOperandKind::gpr_or_sp &&
		    detail::sme_operand_number(base, word) == 31U)
			emit(context.words, uml_arm64_gadget_sve_sp_check);
	}
	emit(context.words, entry);
	const auto operand = context.words.size();
	if (form.slot_count != 0U)
		emit_words(context, packed_slots(form, word));
	if (!emit_immediates(context, form)) {
		/* A reserved immediate: the word is UNDEFINED. */
		context.words.truncate(start);
		return illegal_instruction(context);
	}
	if (memory != nullptr) {
		/* The byte offset, then the stream of sve_library, which the
		 * gadget's slow path enters. */
		emit_words(context, memory_offset(*memory, word));
		emit(context.words, (static_cast<std::uint64_t>(word) << 32) |
			static_cast<std::uint64_t>(index));
		emit(context.words, context.address);
		/* Five words with the entry, as sve_sp_check relies on. */
	}
	if (!fused)
		return DecodeResult::decoded;
	const auto branch = context.following.front();
	const auto branch_address = context.address + sizeof(word);
	const auto target = static_cast<std::uint64_t>(
		static_cast<std::int64_t>(branch_address) +
		sign_extend(((branch >> 5) & 0x7ffffU) << 2, 21));
	emit_link(context, target);
	/* The instructions after the pair go in the operand's top 16 bits;
	 * the loop retires the branch as it advances past it. */
	context.suffixes.add(operand, context.compiled + 2U, 48);
	++context.compiled;
	return DecodeResult::decoded;
}

} // namespace

std::optional<DecodeResult> decode_sve_space(DecodeContext &context)
{
	const auto word = context.instruction;
	const auto &host = context.options.host_capabilities;
	const auto decoded_word =
		decode_sve_word(word, host, context.options.svcr);
	switch (decoded_word.kind) {
	case SveWord::other:
		/* The SME decoder's, or UNDEFINED on a virtual CPU without
		 * SME. */
		if (implements_sme(host))
			return std::nullopt;
		return illegal_instruction(context);
	case SveWord::undefined:
		return illegal_instruction(context);
	case SveWord::implemented:
		break;
	}
	const auto *row = decoded_word.row;
	const auto index =
		static_cast<std::size_t>(row - detail::sme_encodings.data());
	const auto number = sve::row_forms[index];
	DecodeResult decoded = DecodeResult::decoded;
	/* Every other form, and a generated form's absent variant, runs
	 * through the semantic library, which implements every row the
	 * virtual CPU can decode here. */
	if (number != sve::no_form)
		decoded = emit_form(context, sve::forms[number], index);
	else
		decoded = emit_library(context, index);
	if (decoded == DecodeResult::decoded) {
		context.program_accesses_fp_state = true;
		context.program_holds_sve = true;
	}
	return decoded;
}

} // namespace uml::cpu::arm64::gadgets::builder_detail
