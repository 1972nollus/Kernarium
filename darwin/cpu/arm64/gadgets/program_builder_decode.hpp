// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gadget_selection_api.hpp"
#include "program.hpp"

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/id_registers.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

namespace uml::cpu::arm64::detail {
struct SmeEncoding;
} // namespace uml::cpu::arm64::detail

namespace uml::cpu::arm64::gadgets::builder_detail {

struct DecodeContext final {
	std::uint64_t guest_address;
	std::uint64_t address;
	std::uint32_t instruction;
	/* The instructions after this one, for the decoders that fuse a pair into
	 * a single entry. Empty at the end of the window, so a decoder must check
	 * before reading it, and a fusing decoder owns advancing `compiled` past
	 * everything it consumed. */
	std::span<const std::uint32_t> following;
	std::size_t &compiled;
	bool &program_accesses_fp_state;
	ProgramWords &words;
	const Builder::LinkResolver &resolve_link;
	const Builder::IndirectLinkResolver &resolve_indirect_link;
	const BuildOptions &options;
	/* Where the suffix operands of inlined conditional branches are
	 * recorded, so `build` can fill them in once it knows how long the block
	 * turned out to be. */
	InlineSuffixes &suffixes;
};

/* The top 16 bits of the operand of a transfer that carries no PC word: the
 * instruction's position in the block, which a fault reports and the C++
 * boundary turns back into the PC (see resolve_fault_position). `build`
 * bounds a block to fit. */
[[nodiscard]] inline std::uintptr_t fault_position(
	const DecodeContext &context) noexcept
{
	return static_cast<std::uintptr_t>(context.compiled) << 48;
}

/* Emit the cell a direct control-flow gadget reads its successor through: this
 * executor's record for `target`. */
inline void emit_link(DecodeContext &context, std::uint64_t target)
{
	context.words.push_back(reinterpret_cast<std::uintptr_t>(
		context.resolve_link(target)));
}

/* Emit the cell an indirect branch reads its trained targets through. This one
 * names the branch site rather than a target, because the site is what owns the
 * trained cells. */
inline void emit_indirect_link(DecodeContext &context, std::uint64_t source,
			       bool is_return = false)
{
	auto *site = context.resolve_indirect_link(source);
#if UML_GADGET_EXECUTION_HISTOGRAM
	if (site != nullptr)
		site->kind = is_return ? 1 : 0;
#else
	(void)is_return;
#endif
	context.words.push_back(reinterpret_cast<std::uintptr_t>(site));
}

/* Continue translating the fall-through of a conditional branch rather than
 * ending the block at it. A block otherwise ends at its first branch, and
 * conditional branches are the only large category of block ends whose
 * continuation keeps the guest addresses contiguous; see G162.
 *
 * The taken edge keeps its link. The word where the fall-through link used to
 * sit becomes the count of instructions translated after this one, which the
 * taken path gives back to the precharged total before it leaves. `build` fills
 * that in; here it is only reserved and recorded.
 *
 * Returns false when the block must end at this branch after all, in which case
 * the caller emits its ordinary terminal form and nothing has been written.
 */
[[nodiscard]] inline bool emit_inlined_conditional(
	DecodeContext &context, selection::Gadget gadget, std::uint64_t target,
	std::size_t retires = 1)
{
	if (gadget == nullptr || context.suffixes.full())
		return false;
	selection::emit(context.words, gadget);
	emit_link(context, target);
	context.suffixes.add(context.words.size(), context.compiled + retires);
	context.words.push_back(0);
	return true;
}

/* Pack an inline suffix into a zero high-bit range of an operand. build()
 * caps a block at 2^16 instructions, so a suffix needs at most sixteen bits,
 * which a shift of 48 or less leaves free above the operand. */
[[nodiscard]] inline bool emit_inlined_packed_compare_branch(
	DecodeContext &context, selection::Gadget gadget,
	std::uintptr_t operand, std::uint64_t target, std::uint8_t suffix_shift,
	std::size_t retires = 1)
{
	if (gadget == nullptr || suffix_shift == 0 || suffix_shift > 48 ||
	    (operand >> suffix_shift) != 0 ||
	    context.suffixes.full())
		return false;
	selection::emit(context.words, gadget);
	context.suffixes.add(context.words.size(),
		context.compiled + retires, suffix_shift);
	selection::emit(context.words, operand);
	emit_link(context, target);
	return true;
}

[[nodiscard]] inline bool emit_inlined_compare_zero_branch(
	DecodeContext &context, selection::Gadget gadget,
	std::uintptr_t operand, std::uint64_t target, std::size_t retires = 1)
{
	return emit_inlined_packed_compare_branch(
		context, gadget, operand, target, 16, retires);
}

[[nodiscard]] inline bool emit_inlined_test_branch(
	DecodeContext &context, selection::Gadget gadget,
	std::uintptr_t operand, std::uintptr_t mask, std::uint64_t target,
	std::size_t retires = 1)
{
	if (gadget == nullptr || operand > 0xffffU ||
	    context.suffixes.full())
		return false;
	selection::emit(context.words, gadget);
	context.suffixes.add(context.words.size(),
		context.compiled + retires, 16);
	selection::emit(context.words, operand);
	selection::emit(context.words, mask);
	emit_link(context, target);
	return true;
}


enum class DecodeResult : std::uint8_t {
	decoded,
	terminal,
	implementation_gap,
};

/* End the block at an encoding the virtual CPU does not implement: the
 * illegal terminal reports it at this instruction, which retires. */
[[nodiscard]] inline DecodeResult illegal_instruction(DecodeContext &context)
{
	selection::emit(context.words, selection::uml_arm64_gadget_illegal);
	selection::emit(context.words, context.address);
	++context.compiled;
	return DecodeResult::terminal;
}

[[nodiscard]] DecodeResult implementation_gap(DecodeContext &context);
[[nodiscard]] DecodeResult decode_branch_system(DecodeContext &context);
[[nodiscard]] DecodeResult decode_data_processing(DecodeContext &context);
[[nodiscard]] DecodeResult decode_floating_simd(DecodeContext &context);
[[nodiscard]] DecodeResult decode_load_store(DecodeContext &context);
[[nodiscard]] DecodeResult decode_exception(DecodeContext &context);

/* The SME instructions (program_builder_sme.cpp), which decode_branch_system
 * reaches only where it would otherwise end the block: an MSR (immediate) no
 * base PSTATE field claims, a system register it does not know, and a word
 * no other decoder matched. Without SME on the virtual CPU each ends the
 * block as that path always has. */
[[nodiscard]] DecodeResult decode_sme_pstate(DecodeContext &context);
[[nodiscard]] DecodeResult decode_sme_system_register(DecodeContext &context);
[[nodiscard]] DecodeResult decode_sme_space(DecodeContext &context);

/* The generated register-only streaming forms
 * (program_builder_streaming.cpp). decode_streaming_sve() translates a word
 * of an SVE, SVE2 or SME row for SVCR.SM = 1, or returns nothing for a row
 * without a generated form; decode_streaming_fallback() translates a word
 * whose base entries streaming mode cannot select: an FP&SIMD class legal in
 * streaming mode, or else the illegal exit. */
[[nodiscard]] std::optional<DecodeResult> decode_streaming_sve(
	DecodeContext &context, const detail::SmeEncoding &row);
/* The vector memory gadgets: decode_streaming_memory() translates a word of
 * an SVE or SME load, store or prefetch row for SVCR.SM = 1, or returns
 * nothing for a row that is not one or has no gadget; the fallback also
 * takes the SIMD&FP loads and stores streaming mode keeps. */
[[nodiscard]] std::optional<DecodeResult> decode_streaming_memory(
	DecodeContext &context, const detail::SmeEncoding &row);
[[nodiscard]] DecodeResult decode_streaming_fallback(DecodeContext &context);

/* Whether a program translated for SVCR.SM = 1 may select `entry`: an entry
 * of the SME part's streaming half or of a base row engine/sme.inc lists. */
[[nodiscard]] bool streaming_entry(std::uintptr_t entry) noexcept;

/* One row of the streaming twin table engine/sme.inc emits, laid out as the
 * FEAT_AFP twin table is: entries [base_begin, base_end) run as the entries
 * at the same offsets from twin_begin in a program translated for
 * SVCR.SM = 1. */
struct StreamingTwin final {
	std::uintptr_t base_begin;
	std::uintptr_t base_end;
	std::uintptr_t twin_begin;
	std::uintptr_t twin_end;
};
static_assert(sizeof(StreamingTwin) == 32);

[[nodiscard]] std::span<const StreamingTwin> streaming_twins() noexcept;

/* The entry a program translated for SVCR.SM = 1 selects in place of base
 * entry `entry`: its streaming twin, or `entry` itself when it has none. */
[[nodiscard]] std::uintptr_t streaming_twin(std::uintptr_t entry);

/* Selects the streaming forms of the entries one decode emitted for a
 * program translated for SVCR.SM = 1: a base entry that calls helpers becomes
 * its twin, and so does the entry an SP-alignment prefix dispatches after its
 * check. Returns whether every entry may then be selected there. */
[[nodiscard]] bool select_streaming_entries(std::span<std::uintptr_t> emitted);

} // namespace uml::cpu::arm64::gadgets::builder_detail
