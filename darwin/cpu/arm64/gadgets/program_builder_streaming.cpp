// SPDX-License-Identifier: GPL-2.0-or-later
/* Selection of the generated register-only streaming gadgets
 * (engine/generate_streaming.py) for a program translated for SVCR.SM = 1:
 * the SVE, SVE2 and SME forms the encoding table's rows name, and the FP&SIMD
 * classes that execute in streaming mode. The generated header gives each
 * form its entries, the instruction bits that select among them and the
 * operands whose offsets the stream carries; a lowered form's immediate, or
 * a count derived from the run-time SVL, follows them. */
#include "program_builder_decode.hpp"
#include "gadget_selection_api.hpp"
#include "streaming_forms.hpp"

#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/layout/assembly_offsets.h"
#include "cpu/arm64/sme_profile.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>

extern "C" {
/* engine/sme.inc: the generated entries, in the order of the header. */
extern const std::uintptr_t uml_arm64_gadget_sm_generated_entries[];
extern const std::uintptr_t uml_arm64_gadget_sm_generated_entries_end[];
}

namespace uml::cpu::arm64::gadgets::builder_detail {
namespace {

using detail::SmeOperand;
using detail::SmeOperandKind;
using selection::emit;
using streaming::Form;
using streaming::Lowering;

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

/* DecodePredCount: the elements of `esize` bits a predicate pattern names
 * at this SVL. */
[[nodiscard]] constexpr std::uint64_t pattern_count(unsigned pattern,
	unsigned esize, unsigned svl) noexcept
{
	const unsigned elements = svl * 8U / esize;
	switch (pattern) {
	case 0: /* POW2 */
		return std::bit_floor(elements);
	case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
		return elements >= pattern ? pattern : 0U;
	case 9: case 10: case 11: case 12: case 13: {
		const unsigned count = 16U << (pattern - 9U);
		return elements >= count ? count : 0U;
	}
	case 29: /* MUL4 */
		return elements - elements % 4U;
	case 30: /* MUL3 */
		return elements - elements % 3U;
	case 31: /* ALL */
		return elements;
	default:
		return 0;
	}
}

/* The element size, in bits, that an immediate shift's tsz field selects,
 * and the field joined to imm3, for the three field layouts. */
struct ShiftImmediate final {
	unsigned esize;
	unsigned value;
};

[[nodiscard]] constexpr ShiftImmediate shift_immediate(std::uint32_t word,
	unsigned layout) noexcept
{
	unsigned tsz;
	unsigned imm3;
	if (layout == 1) {	/* predicated: tszh 23:22, tszl 9:8, imm3 7:5 */
		tsz = ((word >> 20) & 0xcU) | ((word >> 8) & 3U);
		imm3 = (word >> 5) & 7U;
	} else if (layout == 2) {	/* narrow: tszh 22, tszl 20:19 */
		tsz = ((word >> 20) & 4U) | ((word >> 19) & 3U);
		imm3 = (word >> 16) & 7U;
	} else {	/* tszh 23:22, tszl 20:19, imm3 18:16 */
		tsz = ((word >> 20) & 0xcU) | ((word >> 19) & 3U);
		imm3 = (word >> 16) & 7U;
	}
	const unsigned esize = 8U << (std::bit_width(tsz) - 1U);
	return {esize, (tsz << 3) | imm3};
}

struct Lowered final {
	std::array<std::uint64_t, 3> immediates{};
	/* The variant an immediate shift's element size selects. */
	std::uint32_t variant = 0;
};

/* A lowered form's immediates, or nothing for a word whose immediate is
 * reserved. */
[[nodiscard]] std::optional<Lowered> lower(const Form &form,
	std::uint32_t word, unsigned svl) noexcept
{
	Lowered result;
	auto &immediate = result.immediates;
	const unsigned size = (word >> 22) & 3U;
	const unsigned esize = 8U << size;
	switch (form.lowering) {
	case Lowering::none:
	case Lowering::plain:
		break;
	case Lowering::logical_immediate: {
		const auto bits = detail::decode_logical_immediate(true,
			(word >> 17) & 1U, (word >> 11) & 63U, (word >> 5) & 63U);
		if (!bits)
			return std::nullopt;
		immediate[0] = *bits;
		break;
	}
	case Lowering::index_immediates:
		immediate[0] = static_cast<std::uint64_t>(
			sign_extend((word >> 5) & 31U, 5));
		immediate[1] = static_cast<std::uint64_t>(
			sign_extend((word >> 16) & 31U, 5));
		break;
	case Lowering::index_start:
		immediate[0] = static_cast<std::uint64_t>(
			sign_extend((word >> 5) & 31U, 5));
		break;
	case Lowering::index_step:
		immediate[0] = static_cast<std::uint64_t>(
			sign_extend((word >> 16) & 31U, 5));
		break;
	case Lowering::arith_unsigned:
		immediate[0] = std::uint64_t{(word >> 5) & 0xffU} <<
			(((word >> 13) & 1U) != 0U ? 8U : 0U);
		break;
	case Lowering::arith_signed:
		immediate[0] = static_cast<std::uint64_t>(
			sign_extend((word >> 5) & 0xffU, 8));
		break;
	case Lowering::arith_byte:
		immediate[0] = (word >> 5) & 0xffU;
		break;
	case Lowering::arith_saturating: {
		/* The unsigned immediate as three parts that each fit a
		 * positive element of this size. */
		auto remaining = std::uint64_t{(word >> 5) & 0xffU} <<
			(((word >> 13) & 1U) != 0U ? 8U : 0U);
		const auto largest = (std::uint64_t{1} << (esize - 1U)) - 1U;
		for (auto &part : immediate) {
			part = std::min(remaining, largest);
			remaining -= part;
		}
		if (remaining != 0U)
			return std::nullopt;
		break;
	}
	case Lowering::dup_immediate:
		immediate[0] = static_cast<std::uint64_t>(
			sign_extend((word >> 5) & 0xffU, 8) *
			(((word >> 13) & 1U) != 0U ? 256 : 1));
		break;
	case Lowering::fp_immediate:
		immediate[0] = selection::expand_vfp_immediate(
			static_cast<std::uint8_t>((word >> 5) & 0xffU), esize);
		break;
	case Lowering::compare_signed:
		immediate[0] = static_cast<std::uint64_t>(
			sign_extend((word >> 16) & 31U, 5));
		break;
	case Lowering::compare_unsigned:
		immediate[0] = (word >> 14) & 0x7fU;
		break;
	case Lowering::element_count:
		immediate[0] = pattern_count((word >> 5) & 31U, esize, svl) *
			(((word >> 16) & 15U) + 1U);
		break;
	case Lowering::predicate_count:
		immediate[0] = pattern_count((word >> 5) & 31U, esize, svl);
		break;
	case Lowering::extract: {
		/* EXT's byte offset; one past the vector is offset 0. */
		const unsigned offset = (((word >> 16) & 31U) << 3) |
			((word >> 10) & 7U);
		immediate[0] = offset < svl ? offset : 0U;
		break;
	}
	case Lowering::broadcast_index: {
		const unsigned field = (((word >> 22) & 3U) << 5) |
			((word >> 16) & 31U);
		const unsigned lowest =
			static_cast<unsigned>(std::countr_zero(field & 31U));
		const unsigned index = field >> (lowest + 1U);
		/* A quadword selects its doubleword pair. */
		immediate[0] = lowest == 4U ? 2U * index : index;
		break;
	}
	case Lowering::shift_right:
	case Lowering::shift_right_negated:
	case Lowering::shift_left:
	case Lowering::rotate_right: {
		const auto shift = shift_immediate(word, form.argument);
		const bool left = form.lowering == Lowering::shift_left;
		const auto amount = left ? shift.value - shift.esize :
			2U * shift.esize - shift.value;
		immediate[0] = form.lowering == Lowering::shift_right_negated ?
			static_cast<std::uint64_t>(-std::int64_t{amount}) : amount;
		if (form.lowering == Lowering::rotate_right)
			immediate[1] = shift.esize - amount;
		result.variant = static_cast<std::uint32_t>(
			std::countr_zero(shift.esize)) - 3U;
		break;
	}
	case Lowering::fp_conditional_compare:
		immediate[0] = std::uint64_t{word & 15U} << 28;
		break;
	case Lowering::fp_fixed_to_float: {
		/* 2^-fbits in the destination's precision. */
		const unsigned fbits = 64U - ((word >> 10) & 63U);
		immediate[0] = form.argument != 0U ?
			std::uint64_t{1023U - fbits} << 52 :
			std::uint64_t{127U - fbits} << 23;
		break;
	}
	}
	return result;
}

[[nodiscard]] unsigned immediate_count(Lowering lowering) noexcept
{
	switch (lowering) {
	case Lowering::none:
	case Lowering::plain:
		return 0;
	case Lowering::index_immediates:
	case Lowering::rotate_right:
		return 2;
	case Lowering::arith_saturating:
		return 3;
	default:
		return 1;
	}
}

/* The offset a slot's register has: in the register file for Z, P and V,
 * in CpuState for a general register, where XZR reads the zero slot. */
[[nodiscard]] std::uint64_t slot_offset(SmeOperandKind kind, unsigned number,
	unsigned svl) noexcept
{
	switch (kind) {
	case SmeOperandKind::z:
	case SmeOperandKind::v:
		return sme_z_offset(svl, number);
	case SmeOperandKind::p:
		return sme_p_offset(svl, number);
	case SmeOperandKind::gpr:
		return number == 31U ? UML_GADGET_CPU_ZERO :
			std::uint64_t{number} * sizeof(std::uint64_t);
	case SmeOperandKind::gpr_or_sp:
	case SmeOperandKind::za_tile:
		break;
	}
	return std::uint64_t{number} * sizeof(std::uint64_t);
}

/* The 16-bit offsets of every register the form's operands name, in
 * operand order with a group's registers in turn; ZA tiles are part of
 * the selected entry. */
[[nodiscard]] std::uint64_t slot_offsets(const SmeOperand *operands,
	unsigned count, std::uint32_t word, unsigned svl, bool &any) noexcept
{
	std::uint64_t packed = 0;
	unsigned slot = 0;
	for (unsigned index = 0; index < count; ++index) {
		const auto &operand = operands[index];
		if (operand.kind == SmeOperandKind::za_tile)
			continue;
		const auto number = detail::sme_operand_number(operand, word);
		const unsigned registers =
			operand.kind == SmeOperandKind::p ? 16U : 32U;
		for (unsigned member = 0; member < operand.count; ++member) {
			const auto offset = slot_offset(operand.kind,
				(number + member * operand.stride) % registers, svl);
			packed |= offset << (16U * slot++);
		}
	}
	any = slot != 0U;
	return packed;
}

[[nodiscard]] std::span<const std::uintptr_t> generated_entries() noexcept
{
	return {uml_arm64_gadget_sm_generated_entries,
		uml_arm64_gadget_sm_generated_entries_end};
}

/* Emits one generated form for the context's word, or reports a word the
 * form cannot take. */
[[nodiscard]] bool emit_form(DecodeContext &context, const Form &form,
	const SmeOperand *operands)
{
	const auto word = context.instruction;
	const unsigned svl = context.options.host_capabilities.sme_svl_bytes;
	const auto lowered = lower(form, word, svl);
	if (!lowered)
		return false;
	const auto variant = gather(word, form.variant_mask) + lowered->variant;
	if (variant >= form.variants)
		return false;
	const auto entries = generated_entries();
	if (form.first_entry + variant >= entries.size())
		return false;
	bool any = false;
	const auto offsets = slot_offsets(operands, form.operand_count, word,
		svl, any);
	emit(context.words, entries[form.first_entry + variant]);
	if (any)
		emit(context.words, static_cast<std::uintptr_t>(offsets));
	for (unsigned index = 0; index < immediate_count(form.lowering); ++index)
		emit(context.words,
			static_cast<std::uintptr_t>(lowered->immediates[index]));
	return true;
}

/* The FP&SIMD class a word belongs to, as classify_fp_simd_streaming()
 * finds it, or the class count for none. */
[[nodiscard]] std::size_t fp_simd_class(std::uint32_t word) noexcept
{
	const std::uint32_t bucket = word >> detail::sme_encoding_bucket_shift;
	for (std::uint32_t slot = detail::fp_simd_streaming_buckets[bucket];
	     slot < detail::fp_simd_streaming_buckets[bucket + 1U]; ++slot) {
		const auto index = detail::fp_simd_streaming_bucket_rows[slot];
		const auto &row = detail::fp_simd_streaming_classes[index];
		if ((word & row.mask) == row.value)
			return index;
	}
	return detail::fp_simd_streaming_classes.size();
}

static_assert(streaming::sve_row_forms.size() ==
	detail::sme_encodings.size());
static_assert(streaming::fp_class_forms.size() ==
	detail::fp_simd_streaming_classes.size());

} // namespace

std::optional<DecodeResult> decode_streaming_sve(DecodeContext &context,
	const detail::SmeEncoding &row)
{
	const auto index = static_cast<std::size_t>(
		&row - detail::sme_encodings.data());
	const auto form_index = streaming::sve_row_forms[index];
	if (form_index == streaming::no_form)
		return std::nullopt;
	const auto &form = streaming::forms[form_index];
	if (!emit_form(context, form, detail::sme_encoding_operands_of(row)))
		return illegal_instruction(context);
	return DecodeResult::decoded;
}

DecodeResult decode_streaming_fallback(DecodeContext &context)
{
	const auto index = fp_simd_class(context.instruction);
	if (index < streaming::fp_class_forms.size() &&
	    !detail::fp_simd_streaming_classes[index].advsimd) {
		const auto form_index = streaming::fp_class_forms[index];
		if (form_index != streaming::no_form) {
			const auto &form = streaming::forms[form_index];
			if (emit_form(context, form,
				    streaming::fp_operands.data() +
					    form.operand_begin))
				return DecodeResult::decoded;
		}
	}
	return illegal_instruction(context);
}

} // namespace uml::cpu::arm64::gadgets::builder_detail
