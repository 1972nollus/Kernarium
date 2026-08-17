// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gadget_selection_api.hpp"
#include "program.hpp"
#include "simd_operand_histogram.hpp"

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/id_registers.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

/* Translating a conditional branch's fall-through inline behind it, rather than
 * ending the block there. A block otherwise ends at its first branch, which on
 * a browser guest is a mean of 4.93 instructions; conditional branches end
 * 36.8% of blocks and are the only large category whose continuation keeps the
 * guest addresses contiguous, so nothing about validation or fault accounting
 * has to change. See G162. */
#ifndef UML_GADGET_INLINE_CONDITIONAL_FALLTHROUGH
#define UML_GADGET_INLINE_CONDITIONAL_FALLTHROUGH 1
#endif

namespace uml::cpu::arm64::gadgets::builder_detail {

inline constexpr bool inline_conditional_fallthrough =
	UML_GADGET_INLINE_CONDITIONAL_FALLTHROUGH != 0;

/* The byte offset of a guest vector register inside CpuState, which every
 * SIMD and floating-point operand word in the decode fragments is built from.
 *
 * This was written out by hand in twenty-seven separate places, each one
 * redeclaring the same lambda. Beyond the duplication, that left the engine
 * with no single point where a guest vector operand is named, so G-4 could not
 * measure which registers translated code actually uses without editing every
 * copy - and G129 reverted an attempt that hooked the two helpers which do have
 * a shared form and captured almost nothing. One helper gives the measurement a
 * choke point and the bank a place to consult a mapping.
 */
[[nodiscard]] inline std::size_t vector_operand_offset(unsigned index) noexcept
{
	diagnostics::record_simd_operand(index);
	return offsetof(CpuState, vectors) + index * sizeof(VectorRegister);
}

static_assert(offsetof(CpuState, vectors) + 31U * sizeof(VectorRegister) <=
	0xffffU, "a vector operand offset must fit the 16-bit operand field");

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
	std::pmr::vector<std::uintptr_t> &words;
	const Builder::LinkResolver &resolve_link;
	const Builder::IndirectLinkResolver &resolve_indirect_link;
	const BuildOptions &options;
	/* Where the link cells this decode emits are recorded, or null when the
	 * caller will not offer the stream to another executor. */
	LinkRelocations *relocations;
	/* Where the suffix operands of inlined conditional branches are
	 * recorded, so `build` can fill them in once it knows how long the block
	 * turned out to be. */
	InlineSuffixes *suffixes;
	/* Instructions retired by the runs already closed, so a decoder can name
	 * a position in the whole block rather than in the run it is inside. */
	std::size_t retired_before_run;
	/* Where the build should pick up when a decoder asks to continue
	 * somewhere else, and, while a callee is being translated inline, the
	 * address its return continues at. Zero when not inside one. */
	std::uint64_t *continue_at;
	/* The address an inlined callee's return continues at, set together
	 * with `continue_at` when a call is being entered. */
	std::uint64_t *continue_return;
	std::uint64_t inlined_return;
	/* Whether this callee may be translated inline here at all. */
	const std::function<bool(std::uint64_t)> *may_inline_call;
};

/* Emit the cell a direct control-flow gadget reads its successor through.
 *
 * The pointer is this executor's record for `target`; the guest PC is what
 * another executor would resolve against its own. Recording the cell here is
 * what keeps the two descriptions from drifting apart -- every site that names
 * link state goes through one of these two helpers.
 */
inline void emit_link(DecodeContext &context, std::uint64_t target)
{
	if (context.relocations != nullptr)
		context.relocations->add(context.words.size(), false, target);
	context.words.push_back(reinterpret_cast<std::uintptr_t>(
		context.resolve_link(target)));
}

/* Emit the cell an indirect branch reads its trained targets through. This one
 * names the branch site rather than a target, because the site is what owns the
 * trained cells. */
inline void emit_indirect_link(DecodeContext &context, std::uint64_t source)
{
	if (context.relocations != nullptr)
		context.relocations->add(context.words.size(), true, source);
	context.words.push_back(reinterpret_cast<std::uintptr_t>(
		context.resolve_indirect_link(source)));
}

/* Continue translating the fall-through of a conditional branch rather than
 * ending the block at it.
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
	if constexpr (!inline_conditional_fallthrough)
		return false;
	else if (gadget == nullptr || context.suffixes == nullptr ||
		 context.suffixes->full())
		return false;
	else {
	selection::emit(context.words, gadget);
	emit_link(context, target);
	context.suffixes->add(context.words.size(),
		context.retired_before_run + context.compiled + retires);
	context.words.push_back(0);
	return true;
	}
}

/* As above, for the branches that read a register operand first. */
[[nodiscard]] inline bool emit_inlined_compare_branch(
	DecodeContext &context, selection::Gadget gadget,
	std::uintptr_t operand, std::uint64_t target, std::size_t retires = 1)
{
	if constexpr (!inline_conditional_fallthrough)
		return false;
	else if (gadget == nullptr || context.suffixes == nullptr ||
		 context.suffixes->full())
		return false;
	else {
	selection::emit(context.words, gadget);
	selection::emit(context.words, operand);
	emit_link(context, target);
	context.suffixes->add(context.words.size(),
		context.retired_before_run + context.compiled + retires);
	context.words.push_back(0);
	return true;
	}
}

[[nodiscard]] inline bool emit_inlined_test_branch(
	DecodeContext &context, selection::Gadget gadget,
	std::uintptr_t operand, std::uintptr_t mask, std::uint64_t target,
	std::size_t retires = 1)
{
	if constexpr (!inline_conditional_fallthrough)
		return false;
	else if (gadget == nullptr || context.suffixes == nullptr ||
		 context.suffixes->full())
		return false;
	else {
	selection::emit(context.words, gadget);
	selection::emit(context.words, operand);
	selection::emit(context.words, mask);
	emit_link(context, target);
	context.suffixes->add(context.words.size(),
		context.retired_before_run + context.compiled + retires);
	context.words.push_back(0);
	return true;
	}
}


enum class DecodeResult : std::uint8_t {
	decoded,
	terminal,
	implementation_gap,
	/* Translated so far, but the next guest instruction is at the address
	 * left in `continue_at` rather than the one after this. */
	continue_elsewhere,
};

[[nodiscard]] DecodeResult implementation_gap(DecodeContext &context);
[[nodiscard]] DecodeResult decode_branch_system(DecodeContext &context);
[[nodiscard]] DecodeResult decode_data_processing(DecodeContext &context);
[[nodiscard]] DecodeResult decode_floating_simd(DecodeContext &context);
[[nodiscard]] DecodeResult decode_load_store(DecodeContext &context);
[[nodiscard]] DecodeResult decode_exception(DecodeContext &context);

} // namespace uml::cpu::arm64::gadgets::builder_detail
