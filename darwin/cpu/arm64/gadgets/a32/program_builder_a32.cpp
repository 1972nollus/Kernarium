// SPDX-License-Identifier: GPL-2.0-or-later
#include "translator.hpp"

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/decode_support.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/gadgets/a32_layout.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

/* The A64 engine's gadgets the AArch32 part shares: control flow, which
 * carries a key where an A64 stream carries a PC; the barriers; and the
 * exclusive-access family, whose host monitor and forward-progress rules are
 * the engine's (system_atomic.inc). */
extern "C" {
void uml_arm64_gadget_branch_linked();
void uml_arm64_gadget_branch();
void uml_arm64_gadget_interrupt();
void uml_arm64_gadget_illegal();
void uml_arm64_gadget_yield();
void uml_arm64_gadget_isb();
void uml_arm64_gadget_clear_exclusive();
#define UML_A32_DECLARE_CONDITIONAL(cc) \
	void uml_arm64_gadget_branch_##cc(); \
	void uml_arm64_gadget_branch_##cc##_linked(); \
	void uml_arm64_gadget_branch_##cc##_inline();
UML_A32_DECLARE_CONDITIONAL(eq)
UML_A32_DECLARE_CONDITIONAL(ne)
UML_A32_DECLARE_CONDITIONAL(hs)
UML_A32_DECLARE_CONDITIONAL(lo)
UML_A32_DECLARE_CONDITIONAL(mi)
UML_A32_DECLARE_CONDITIONAL(pl)
UML_A32_DECLARE_CONDITIONAL(vs)
UML_A32_DECLARE_CONDITIONAL(vc)
UML_A32_DECLARE_CONDITIONAL(hi)
UML_A32_DECLARE_CONDITIONAL(ls)
UML_A32_DECLARE_CONDITIONAL(ge)
UML_A32_DECLARE_CONDITIONAL(lt)
UML_A32_DECLARE_CONDITIONAL(gt)
UML_A32_DECLARE_CONDITIONAL(le)
#undef UML_A32_DECLARE_CONDITIONAL
#define UML_A32_DECLARE_BARRIER(n) \
	void uml_arm64_gadget_dmb_##n(); \
	void uml_arm64_gadget_dsb_##n();
UML_A32_DECLARE_BARRIER(0) UML_A32_DECLARE_BARRIER(1)
UML_A32_DECLARE_BARRIER(2) UML_A32_DECLARE_BARRIER(3)
UML_A32_DECLARE_BARRIER(4) UML_A32_DECLARE_BARRIER(5)
UML_A32_DECLARE_BARRIER(6) UML_A32_DECLARE_BARRIER(7)
UML_A32_DECLARE_BARRIER(8) UML_A32_DECLARE_BARRIER(9)
UML_A32_DECLARE_BARRIER(10) UML_A32_DECLARE_BARRIER(11)
UML_A32_DECLARE_BARRIER(12) UML_A32_DECLARE_BARRIER(13)
UML_A32_DECLARE_BARRIER(14) UML_A32_DECLARE_BARRIER(15)
#undef UML_A32_DECLARE_BARRIER
void uml_arm64_gadget_exclusive_load_relaxed_b_r();
void uml_arm64_gadget_exclusive_load_relaxed_h_r();
void uml_arm64_gadget_exclusive_load_relaxed_w_r();
void uml_arm64_gadget_exclusive_store_relaxed_b_rr();
void uml_arm64_gadget_exclusive_store_relaxed_h_rr();
void uml_arm64_gadget_exclusive_store_relaxed_w_rr();
void uml_arm64_gadget_exclusive_pair_load_relaxed_w_rr();
void uml_arm64_gadget_exclusive_pair_store_relaxed_w_rrr();
void uml_arm64_gadget_exclusive_load_acquire_b_r();
void uml_arm64_gadget_exclusive_load_acquire_h_r();
void uml_arm64_gadget_exclusive_load_acquire_w_r();
void uml_arm64_gadget_exclusive_store_release_b_rr();
void uml_arm64_gadget_exclusive_store_release_h_rr();
void uml_arm64_gadget_exclusive_store_release_w_rr();
void uml_arm64_gadget_exclusive_pair_load_acquire_w_rr();
void uml_arm64_gadget_exclusive_pair_store_release_w_rrr();
void uml_arm64_gadget_ordered_load_acquire_b_r();
void uml_arm64_gadget_ordered_load_acquire_h_r();
void uml_arm64_gadget_ordered_load_acquire_w_r();
void uml_arm64_gadget_ordered_store_release_b_r();
void uml_arm64_gadget_ordered_store_release_h_r();
void uml_arm64_gadget_ordered_store_release_w_r();
}

namespace uml::cpu::arm64::gadgets::a32 {
namespace {

using namespace aarch32;

constexpr std::array<void (*)(), 14> conditional_inline{
	uml_arm64_gadget_branch_eq_inline, uml_arm64_gadget_branch_ne_inline,
	uml_arm64_gadget_branch_hs_inline, uml_arm64_gadget_branch_lo_inline,
	uml_arm64_gadget_branch_mi_inline, uml_arm64_gadget_branch_pl_inline,
	uml_arm64_gadget_branch_vs_inline, uml_arm64_gadget_branch_vc_inline,
	uml_arm64_gadget_branch_hi_inline, uml_arm64_gadget_branch_ls_inline,
	uml_arm64_gadget_branch_ge_inline, uml_arm64_gadget_branch_lt_inline,
	uml_arm64_gadget_branch_gt_inline, uml_arm64_gadget_branch_le_inline};
constexpr std::array<void (*)(), 14> conditional_plain{
	uml_arm64_gadget_branch_eq, uml_arm64_gadget_branch_ne,
	uml_arm64_gadget_branch_hs, uml_arm64_gadget_branch_lo,
	uml_arm64_gadget_branch_mi, uml_arm64_gadget_branch_pl,
	uml_arm64_gadget_branch_vs, uml_arm64_gadget_branch_vc,
	uml_arm64_gadget_branch_hi, uml_arm64_gadget_branch_ls,
	uml_arm64_gadget_branch_ge, uml_arm64_gadget_branch_lt,
	uml_arm64_gadget_branch_gt, uml_arm64_gadget_branch_le};
constexpr std::array<void (*)(), 16> data_barrier{
	uml_arm64_gadget_dmb_0, uml_arm64_gadget_dmb_1, uml_arm64_gadget_dmb_2,
	uml_arm64_gadget_dmb_3, uml_arm64_gadget_dmb_4, uml_arm64_gadget_dmb_5,
	uml_arm64_gadget_dmb_6, uml_arm64_gadget_dmb_7, uml_arm64_gadget_dmb_8,
	uml_arm64_gadget_dmb_9, uml_arm64_gadget_dmb_10,
	uml_arm64_gadget_dmb_11, uml_arm64_gadget_dmb_12,
	uml_arm64_gadget_dmb_13, uml_arm64_gadget_dmb_14,
	uml_arm64_gadget_dmb_15};
constexpr std::array<void (*)(), 16> synchronization_barrier{
	uml_arm64_gadget_dsb_0, uml_arm64_gadget_dsb_1, uml_arm64_gadget_dsb_2,
	uml_arm64_gadget_dsb_3, uml_arm64_gadget_dsb_4, uml_arm64_gadget_dsb_5,
	uml_arm64_gadget_dsb_6, uml_arm64_gadget_dsb_7, uml_arm64_gadget_dsb_8,
	uml_arm64_gadget_dsb_9, uml_arm64_gadget_dsb_10,
	uml_arm64_gadget_dsb_11, uml_arm64_gadget_dsb_12,
	uml_arm64_gadget_dsb_13, uml_arm64_gadget_dsb_14,
	uml_arm64_gadget_dsb_15};

/* The data-processing operations, as a32_forms.hpp numbers them. */
enum Op : unsigned {
	op_and, op_eor, op_sub, op_rsb, op_add, op_adc, op_sbc, op_rsc, op_tst,
	op_teq, op_cmp, op_cmn, op_orr, op_mov, op_bic, op_mvn, op_orn,
};

enum Shape : unsigned {
	shape_imm, shape_imc, shape_reg, shape_lsl, shape_lsr, shape_asr,
	shape_ror, shape_rrx, shape_rlsl, shape_rlsr, shape_rasr, shape_rror,
};

[[nodiscard]] constexpr bool is_logical(unsigned op) noexcept
{
	return op == op_and || op == op_eor || op == op_tst || op == op_teq ||
	       op == op_orr || op == op_mov || op == op_bic || op == op_mvn ||
	       op == op_orn;
}

[[nodiscard]] constexpr bool is_compare(unsigned op) noexcept
{
	return op == op_tst || op == op_teq || op == op_cmp || op == op_cmn;
}

/* The forms whose immediate the decoder leaves as the raw 12-bit field. */
[[nodiscard]] constexpr bool raw_immediate(unsigned op) noexcept
{
	return op == op_and || op == op_eor || op == op_bic || op == op_orr ||
	       op == op_orn || op == op_tst || op == op_teq || op == op_mvn;
}

[[nodiscard]] std::uint32_t mask_of_width(unsigned width) noexcept
{
	return width >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << width) - 1U;
}

/* An indirect site that never matches, for the table branches of an
 * uncached single instruction: its miss leaves with the target as the PC. */
IndirectLink &dummy_site() noexcept
{
	static IndirectLink site = [] {
		IndirectLink empty;
		for (auto &entry : empty.entries)
			entry = {~std::uint64_t{}, nullptr};
		return empty;
	}();
	return site;
}

} // namespace

namespace detail {

void Translator::close_run()
{
	if (!run_active_)
		return;
	words_[run_operand_] =
		(words_.size() - (run_operand_ + 1)) * sizeof(std::uintptr_t);
	run_active_ = false;
}

void Translator::begin_instruction(unsigned cond)
{
	if (cond >= 14) {
		if (run_active_)
			close_run();
		return;
	}
	if (run_active_ && run_cond_ == cond && !run_dirty_)
		return;
	close_run();
	static constexpr std::array<G, 14> skips{
		G::skip_eq, G::skip_ne, G::skip_cs, G::skip_cc, G::skip_mi,
		G::skip_pl, G::skip_vs, G::skip_vc, G::skip_hi, G::skip_ls,
		G::skip_ge, G::skip_lt, G::skip_gt, G::skip_le};
	push(skips[cond]);
	run_operand_ = words_.size();
	push(0);
	run_active_ = true;
	run_cond_ = cond;
	run_dirty_ = false;
}

void Translator::credit()
{
	if (!run_active_)
		return;
	push(G::credit);
	patches_.emplace_back(words_.size(), compiled_ + 1);
	push(0);
}

std::uintptr_t Translator::source(int number, std::uintptr_t slot)
{
	if (number != 15)
		return static_cast<std::uintptr_t>(number) * 8U;
	set_scratch(slot, pc_value());
	return slot;
}

void Translator::set_scratch(std::uintptr_t slot, std::uint32_t value)
{
	push(G::set_scratch);
	push(slot);
	push(value);
}

/* The branch to the PC an instruction wrote to a register slot. */
void Translator::emit_indirect(std::uintptr_t register_offset,
			       bool interwork, bool returns)
{
	credit();
	const bool cached = !!indirects_;
	if (interwork) {
		push(cached ? (returns ? G::bx_return_cached : G::bx_cached)
			    : G::bx);
	} else {
		push(cached ? (returns ? G::bw_return_cached : G::bw_cached)
			    : G::bw);
	}
	push(register_offset);
	if (cached)
		push(reinterpret_cast<std::uintptr_t>(indirects_(key_)));
}

void Translator::emit_direct_exit(std::uint64_t target)
{
	credit();
	if (linked()) {
		push_external(uml_arm64_gadget_branch_linked);
		push_link(target);
	} else {
		push_external(uml_arm64_gadget_branch);
		push(target);
	}
}

void Translator::emit_fallthrough()
{
	close_run();
	const auto next = input_.key + 4U * unit_;
	if (it_.active() && !input_.single) {
		/* Ended inside an IT block: record where, and leave through the
		 * executor, whose next entry runs the rest unlinked. */
		push(G::set_itstate);
		push(it_.bits());
		push_external(uml_arm64_gadget_interrupt);
		push(next);
		return;
	}
	if (linked()) {
		push_external(uml_arm64_gadget_branch_linked);
		push_link(next);
	} else {
		push_external(uml_arm64_gadget_interrupt);
		push(next);
	}
}

void Translator::emit_illegal()
{
	push_external(uml_arm64_gadget_illegal);
	push(key_);
}

/* An instruction the profile cannot run ends the block at the illegal
 * exit. */
void Translator::finish_stop(bool, std::uint32_t word)
{
	close_run();
	push_external(uml_arm64_gadget_illegal);
	push(key_);
	complete_ = false;
	stop_address_ = key_;
	stop_instruction_ = word;
}

/* ------------------------------------------------------------------ DP */

Translator::Result Translator::emit_dp_values(const Instruction &in,
	unsigned op, bool s, unsigned shape_class, std::uintptr_t rd,
	std::uintptr_t rn, std::uintptr_t rm, std::uintptr_t rs,
	std::uint32_t immediate, std::optional<unsigned> carry)
{
	const auto &o = in.operands;
	const bool logical = is_logical(op);
	const bool compare = is_compare(op);
	const bool sets = s || compare;
	unsigned shape = shape_imm;
	std::uint64_t word = rd | rn << 8;
	if (shape_class == 0) {
		if (logical && sets && carry) {
			shape = shape_imc;
			word |= std::uint64_t{*carry} << 16;
		}
		word |= std::uint64_t{immediate} << 32;
	} else {
		word |= rm << 16;
		if (shape_class == 1) {
			const auto amount = static_cast<unsigned>(o.shift_n);
			switch (o.shift_t) {
			case SRType::LSL:
				if (amount == 0) {
					shape = shape_reg;
				} else {
					shape = shape_lsl;
					word |= std::uint64_t{amount |
						(32U - amount) << 6} << 32;
				}
				break;
			case SRType::LSR:
				shape = shape_lsr;
				word |= std::uint64_t{amount |
					(amount - 1U) << 6} << 32;
				break;
			case SRType::ASR:
				shape = shape_asr;
				word |= std::uint64_t{amount |
					(amount - 1U) << 6} << 32;
				break;
			case SRType::ROR:
				shape = shape_ror;
				word |= std::uint64_t{amount |
					(amount - 1U) << 6} << 32;
				break;
			case SRType::RRX:
				shape = shape_rrx;
				break;
			}
		} else {
			word |= rs << 24;
			switch (o.shift_t) {
			case SRType::LSL: shape = shape_rlsl; break;
			case SRType::LSR: shape = shape_rlsr; break;
			case SRType::ASR: shape = shape_rasr; break;
			default: shape = shape_rror; break;
			}
		}
	}
	const G which = kDp[op][sets ? 1 : 0][shape];
	if (which == G::count)
		return {};
	push(which);
	push(word);
	return {false, sets};
}

Translator::Result Translator::emit_mov_constant(const Instruction &in, int d,
	std::uint32_t value)
{
	const bool to_pc = d == 15;
	const std::uintptr_t rd =
		to_pc ? scratch0 : static_cast<std::uintptr_t>(d) * 8U;
	(void)emit_dp_values(in, op_mov, false, 0, rd, 0, 0, 0, value,
			     std::nullopt);
	if (to_pc)
		emit_indirect(scratch0, !thumb_, false);
	return {to_pc, false};
}

Translator::Result Translator::emit_dp(const Instruction &in,
				       const FormPlan &plan)
{
	const auto &o = in.operands;
	const unsigned op = plan.sub;
	const unsigned shape_class = plan.aux & 3U;
	const bool s = o.setflags || is_compare(op);
	std::uintptr_t rn = 0;
	std::uintptr_t rm = 0;
	std::uintptr_t rs = 0;
	/* The SP forms name no Rn: the first operand is R13. */
	if (op != op_mov && op != op_mvn)
		rn = source((plan.aux & 4U) != 0 ? 13 : o.n, scratch0);
	if (shape_class != 0)
		rm = source(o.m, scratch1);
	if (shape_class == 2)
		rs = static_cast<std::uintptr_t>(o.s) * 8U;
	std::uint32_t immediate = 0;
	std::optional<unsigned> carry;
	if (shape_class == 0) {
		if (raw_immediate(op)) {
			const auto expanded = o.a32 ?
				aarch32::detail::a32_expand_imm_c(o.imm) :
				aarch32::detail::t32_expand_imm_c(o.imm);
			immediate = static_cast<std::uint32_t>(expanded.imm32);
			if (!expanded.carry_in)
				carry = static_cast<unsigned>(expanded.carry);
		} else {
			immediate = static_cast<std::uint32_t>(o.imm32);
			if (is_logical(op) && !o.carry_flags)
				carry = o.carry;
		}
	}
	const bool to_pc = !is_compare(op) && o.d == 15;
	const std::uintptr_t rd = is_compare(op) ? 0 :
		to_pc ? scratch0 : static_cast<std::uintptr_t>(o.d) * 8U;
	const auto result = emit_dp_values(in, op, s, shape_class, rd, rn, rm,
					   rs, immediate, carry);
	if (to_pc)
		emit_indirect(scratch0, !thumb_, false);
	return {to_pc, result.flags};
}

/* ------------------------------------------------------------ multiply */

Translator::Result Translator::emit_multiply(const Instruction &in,
					     const FormPlan &plan)
{
	const auto &o = in.operands;
	const auto d = static_cast<std::uint64_t>(o.d) * 8U;
	const auto n = static_cast<std::uint64_t>(o.n) * 8U;
	const auto m = static_cast<std::uint64_t>(o.m) * 8U;
	const auto a = static_cast<std::uint64_t>(o.a) * 8U;
	const auto lo = static_cast<std::uint64_t>(o.dLo) * 8U;
	const auto hi = static_cast<std::uint64_t>(o.dHi) * 8U;
	switch (plan.family) {
	case Family::mul: {
		const bool s = o.setflags;
		G which;
		if (plan.sub == 0)
			which = s ? G::muls : G::mul;
		else if (plan.sub == 1)
			which = s ? G::mlas : G::mla;
		else
			which = G::mls;
		push(which);
		push(d | n << 8 | m << 16 | a << 24);
		return {false, s};
	}
	case Family::mull: {
		static constexpr std::array<G, 10> table{
			G::umull, G::umulls, G::smull, G::smulls, G::umlal,
			G::umlals, G::smlal, G::smlals, G::umaal, G::umaal};
		const bool s = o.setflags && plan.sub != 4;
		push(table[plan.sub * 2U + (s ? 1U : 0U)]);
		push(lo | hi << 8 | n << 16 | m << 24);
		return {false, s};
	}
	case Family::mulh: {
		static constexpr std::array<G, 6> table{
			G::smmul, G::smmulr, G::smmla, G::smmlar, G::smmls,
			G::smmlsr};
		push(table[plan.sub]);
		push(d | n << 8 | m << 16 | a << 24);
		return {};
	}
	case Family::mulxy: {
		static constexpr std::array<G, 16> table{
			G::smulbb, G::smulbt, G::smultb, G::smultt, G::smlabb,
			G::smlabt, G::smlatb, G::smlatt, G::smulwb, G::smulwt,
			G::smlawb, G::smlawt, G::smlalbb, G::smlalbt,
			G::smlaltb, G::smlaltt};
		push(table[plan.sub]);
		if (plan.sub >= 12)
			push(lo | hi << 8 | n << 16 | m << 24);
		else
			push(d | n << 8 | m << 16 | a << 24);
		return {};
	}
	case Family::div:
		push(plan.sub == 0 ? G::sdiv : G::udiv);
		push(d | n << 8 | m << 16);
		return {};
	default:
		return {};
	}
}

Translator::Result Translator::emit_integer(const Instruction &in,
					    const FormPlan &plan)
{
	const auto &o = in.operands;
	const auto d = static_cast<std::uint64_t>(o.d) * 8U;
	const auto n = static_cast<std::uint64_t>(o.n) * 8U;
	const auto m = static_cast<std::uint64_t>(o.m) * 8U;
	switch (plan.family) {
	case Family::movw:
		return emit_mov_constant(in, o.d,
			static_cast<std::uint32_t>(o.imm32));
	case Family::movt:
		push(G::movt);
		push(d | std::uint64_t{o.imm16} << 48);
		return {};
	case Family::adr: {
		const auto offset = static_cast<std::uint32_t>(o.imm32);
		const auto value = o.add ? pc_aligned() + offset :
					   pc_aligned() - offset;
		return emit_mov_constant(in, o.d, value);
	}
	case Family::extend: {
		static constexpr std::array<G, 8> table{
			G::sxtb, G::sxth, G::uxtb, G::uxth, G::sxtba, G::sxtha,
			G::uxtba, G::uxtha};
		push(table[plan.sub]);
		push(d | n << 8 | m << 16 |
		     std::uint64_t{static_cast<unsigned>(o.rotation)} << 24);
		return {};
	}
	case Family::bits: {
		static constexpr std::array<G, 5> table{
			G::rev, G::rev16, G::revsh, G::rbit, G::clz};
		push(table[plan.sub]);
		push(d | m << 16);
		return {};
	}
	case Family::field: {
		const auto lsb = static_cast<unsigned>(o.lsbit);
		if (plan.sub == 0) {
			const unsigned width = o.widthminus1 + 1U;
			push(G::ubfx);
			push(d | n << 8 | std::uint64_t{lsb} << 16 |
			     std::uint64_t{mask_of_width(width)} << 32);
		} else if (plan.sub == 1) {
			const unsigned width = o.widthminus1 + 1U;
			push(G::sbfx);
			push(d | n << 8 |
			     std::uint64_t{32U - lsb - width} << 16 |
			     std::uint64_t{32U - width} << 24);
		} else {
			const unsigned width = o.msbit - o.lsbit + 1U;
			const std::uint64_t mask =
				std::uint64_t{mask_of_width(width)} << lsb;
			if (plan.sub == 2) {
				push(G::bfi);
				push(d | n << 8 | std::uint64_t{lsb} << 16 |
				     (mask & 0xffff'ffffU) << 32);
			} else {
				push(G::bfc);
				push(d | (mask & 0xffff'ffffU) << 32);
			}
		}
		return {};
	}
	case Family::sat: {
		const auto bits = static_cast<unsigned>(o.saturate_to);
		const bool asr = o.shift_t == SRType::ASR;
		std::int64_t maximum;
		std::int64_t minimum;
		if (plan.sub == 0) {
			maximum = (std::int64_t{1} << (bits - 1U)) - 1;
			minimum = -(std::int64_t{1} << (bits - 1U));
			push(asr ? G::ssat_asr : G::ssat_lsl);
		} else {
			maximum = (std::int64_t{1} << bits) - 1;
			minimum = 0;
			push(asr ? G::usat_asr : G::usat_lsl);
		}
		push(d | n << 8 | std::uint64_t{static_cast<unsigned>(
			o.shift_n)} << 16);
		push((static_cast<std::uint64_t>(maximum) & 0xffff'ffffU) |
		     (static_cast<std::uint64_t>(minimum) & 0xffff'ffffU) << 32);
		return {};
	}
	case Family::pkh:
		push(plan.sub == 0 ? G::pkhbt : G::pkhtb);
		push(d | n << 8 | m << 16 |
		     std::uint64_t{static_cast<unsigned>(o.shift_n)} << 24);
		return {};
	default:
		return {};
	}
}

/* The operand word the library gadget and the library slow paths carry
 * (a32_layout.h). */
std::uint64_t Translator::library_operand(const Instruction &in,
					  bool pc_value) const noexcept
{
	return std::uint64_t{in.word} |
	       std::uint64_t{it_.bits()} << UML_GADGET_A32_LIBRARY_IT_SHIFT |
	       std::uint64_t{static_cast<std::uint8_t>(profile_.level)}
		       << UML_GADGET_A32_LIBRARY_LEVEL_SHIFT |
	       std::uint64_t{pc_value} << UML_GADGET_A32_LIBRARY_PC_VALUE_SHIFT |
	       std::uint64_t{static_cast<std::uint16_t>(in.form)}
		       << UML_GADGET_A32_LIBRARY_FORM_SHIFT;
}

/* Whether a form's semantics read or write the vector registers or the
 * FPSCR: VFP and Advanced SIMD (V...), the FLDMX and FSTMX block forms and
 * the crypto extensions. The library works on the registers the guest
 * state holds, so a block that reaches it for one of these must have the
 * deferred floating-point state loaded (and marked dirty) first. */
static bool touches_fp_state(const char *mnemonic) noexcept
{
	return mnemonic[0] == 'V' || std::strncmp(mnemonic, "FLDM", 4) == 0 ||
	       std::strncmp(mnemonic, "FSTM", 4) == 0 ||
	       std::strncmp(mnemonic, "AES", 3) == 0 ||
	       std::strncmp(mnemonic, "SHA1", 4) == 0 ||
	       std::strncmp(mnemonic, "SHA256", 6) == 0;
}

/* An instruction the engine leaves to the semantic library. */
Translator::Result Translator::emit_library(const Instruction &in)
{
	if (touches_fp_state(in.info().mnemonic))
		uses_fp_ = true;
	push(G::library);
	push(key_);
	push(library_operand(in));
	return {};
}

/* ------------------------------------------------------- loads, stores */

Translator::Result Translator::emit_single(const Instruction &in,
					   const FormPlan &plan)
{
	const auto &o = in.operands;
	const bool load = plan.family == Family::load;
	const unsigned size = plan.sub;
	const bool literal = (plan.aux & 1U) != 0;
	const bool register_form = (plan.aux & 2U) != 0;
	const bool unprivileged = (plan.aux & 4U) != 0;
	const bool to_pc = load && o.t == 15;
	std::uintptr_t rt = static_cast<std::uintptr_t>(o.t) * 8U;
	if (to_pc)
		rt = scratch0;
	else if (!load && o.t == 15)
		rt = source(15, scratch0);

	unsigned mode;
	std::uint64_t word = rt;
	if (literal) {
		const auto offset = static_cast<std::uint32_t>(o.imm32);
		const auto address = o.add ? pc_aligned() + offset :
					     pc_aligned() - offset;
		mode = 9;
		word |= std::uint64_t{address} << 32;
	} else {
		const std::uintptr_t rn = source(o.n, scratch1);
		/* The unprivileged forms post-index in A32 and use the offset
		 * form, with no writeback, in T32. */
		const bool writeback = unprivileged ? o.postindex != 0 : o.wback;
		const bool post = unprivileged ? o.postindex != 0 :
						 (!o.index && o.wback);
		word |= rn << 8;
		if (!register_form) {
			std::uint32_t offset = static_cast<std::uint32_t>(o.imm32);
			if (!o.add)
				offset = 0U - offset;
			mode = !writeback ? 0U : post ? 2U : 1U;
			word |= std::uint64_t{offset} << 32;
		} else {
			std::uintptr_t rm = static_cast<std::uintptr_t>(o.m) * 8U;
			unsigned amount = 0;
			if (o.shift_t == SRType::LSL) {
				amount = static_cast<unsigned>(o.shift_n);
			} else {
				/* A shift the transfer cannot apply is a move
				 * first. */
				(void)emit_dp_values(in, op_mov, false, 1,
						     scratch0 == rt ? scratch1 :
						     scratch0, 0, rm, 0, 0,
						     std::nullopt);
				rm = scratch0 == rt ? scratch1 : scratch0;
			}
			const unsigned base = !writeback ? 3U : post ? 7U : 5U;
			mode = base + (o.add ? 0U : 1U);
			word |= rm << 16 | std::uint64_t{amount} << 24;
		}
	}
	push(load ? kLoad[size][mode] : kStore[size][mode]);
	push(key_);
	push(word);
	if (to_pc)
		emit_indirect(scratch0, true, o.n == 13);
	return {to_pc, false};
}

Translator::Result Translator::emit_double(const Instruction &in,
					   const FormPlan &plan)
{
	const auto &o = in.operands;
	const bool load = plan.family == Family::loadd;
	const bool literal = (plan.aux & 1U) != 0;
	const auto rt = static_cast<std::uint64_t>(o.t) * 8U;
	const auto rt2 = static_cast<std::uint64_t>(o.t2) * 8U;
	std::uint64_t word = rt | rt2 << 8;
	unsigned mode;
	if (literal) {
		const auto offset = static_cast<std::uint32_t>(o.imm32);
		const auto address = o.add ? pc_aligned() + offset :
					     pc_aligned() - offset;
		mode = 9;
		word |= std::uint64_t{address} << 32;
	} else {
		const std::uintptr_t rn = source(o.n, scratch0);
		word |= rn << 16;
		/* The register forms are the A32 encodings with bit 22 clear. */
		const bool by_register = !thumb_ && (in.word & (1U << 22)) == 0;
		if (!by_register) {
			std::uint32_t offset = static_cast<std::uint32_t>(o.imm32);
			if (!o.add)
				offset = 0U - offset;
			mode = !o.wback ? 0U : o.index ? 1U : 2U;
			word |= std::uint64_t{offset} << 32;
		} else {
			const std::uintptr_t rm = source(o.m, scratch1);
			const unsigned base = !o.wback ? 3U : o.index ? 5U : 7U;
			mode = base + (o.add ? 0U : 1U);
			word |= rm << 24;
		}
	}
	push(load ? kLdrd[mode] : kStrd[mode]);
	push(key_);
	push(word);
	return {};
}

Translator::Result Translator::emit_block_transfer(const Instruction &in,
						   const FormPlan &plan)
{
	const auto &o = in.operands;
	const bool load = plan.family == Family::ldm;
	const int base_register = plan.aux != 0 ? 13 : o.n;
	const bool writeback = plan.aux != 0 || o.wback;
	const unsigned registers = o.registers;
	const int count = std::popcount(registers);
	int start;
	int delta;
	switch (plan.sub) {
	case 0: start = 0; delta = 4 * count; break;
	case 1: start = -4 * count + 4; delta = -4 * count; break;
	case 2: start = -4 * count; delta = -4 * count; break;
	default: start = 4; delta = 4 * count; break;
	}
	const bool has_pc = (registers & 0x8000U) != 0;
	if (!load && has_pc)
		set_scratch(scratch0, pc_value());
	push(load ? kLdm[count] : kStm[count]);
	push(key_);
	push(static_cast<std::uint64_t>(base_register) * 8U |
	     std::uint64_t{static_cast<std::uint8_t>(start)} << 8 |
	     std::uint64_t{static_cast<std::uint8_t>(delta)} << 16 |
	     std::uint64_t{writeback} << 24);
	std::uint64_t packed = 0;
	unsigned index = 0;
	for (unsigned number = 0; number < 16; ++number) {
		if ((registers >> number & 1U) == 0)
			continue;
		const std::uint64_t offset =
			number == 15 ? scratch0 : std::uint64_t{number} * 8U;
		packed |= offset << (8U * (index % 8U));
		if (++index % 8U == 0) {
			push(packed);
			packed = 0;
		}
	}
	if (index % 8U != 0)
		push(packed);
	/* A transfer that crosses a page is the library's; a load that writes
	 * PC leaves the value in the scratch slot, as the gadget does. */
	push(library_operand(in, load && has_pc));
	if (load && has_pc) {
		emit_indirect(scratch0, true, base_register == 13);
		return {true, false};
	}
	return {};
}

Translator::Result Translator::emit_exclusive(const Instruction &in,
					      const FormPlan &plan)
{
	const auto &o = in.operands;
	const bool store = (plan.aux & 1U) != 0;
	/* LDAEX and STLEX are the same operations with the host's acquire and
	 * release forms. */
	const bool ordered = (plan.aux & 2U) != 0;
	const unsigned size = plan.sub;
	const auto t = static_cast<std::uint64_t>(o.t) * 8U;
	const auto t2 = static_cast<std::uint64_t>(o.t2) * 8U;
	auto n = static_cast<std::uint64_t>(o.n) * 8U;
	const auto d = static_cast<std::uint64_t>(o.d) * 8U;
	/* The T32 forms add a word offset to the base; the gadgets take the
	 * address in a register. */
	if (o.imm32 != 0) {
		(void)emit_dp_values(in, op_add, false, 0, scratch0, n, 0, 0,
				     static_cast<std::uint32_t>(o.imm32),
				     std::nullopt);
		n = scratch0;
	}
	if (!store) {
		if (size == 3) {
			push_external(ordered ?
				uml_arm64_gadget_exclusive_pair_load_acquire_w_rr :
				uml_arm64_gadget_exclusive_pair_load_relaxed_w_rr);
			push(t | t2 << 16 | n << 32);
		} else {
			push_external(ordered ?
				(size == 0 ?
				 uml_arm64_gadget_exclusive_load_acquire_b_r :
				 size == 1 ?
				 uml_arm64_gadget_exclusive_load_acquire_h_r :
				 uml_arm64_gadget_exclusive_load_acquire_w_r) :
				(size == 0 ?
				 uml_arm64_gadget_exclusive_load_relaxed_b_r :
				 size == 1 ?
				 uml_arm64_gadget_exclusive_load_relaxed_h_r :
				 uml_arm64_gadget_exclusive_load_relaxed_w_r));
			push(t | n << 16);
		}
	} else if (size == 3) {
		push_external(ordered ?
			uml_arm64_gadget_exclusive_pair_store_release_w_rrr :
			uml_arm64_gadget_exclusive_pair_store_relaxed_w_rrr);
		push(t | t2 << 16 | d << 32 | n << 48);
	} else {
		push_external(ordered ?
			(size == 0 ?
			 uml_arm64_gadget_exclusive_store_release_b_rr :
			 size == 1 ?
			 uml_arm64_gadget_exclusive_store_release_h_rr :
			 uml_arm64_gadget_exclusive_store_release_w_rr) :
			(size == 0 ?
			 uml_arm64_gadget_exclusive_store_relaxed_b_rr :
			 size == 1 ?
			 uml_arm64_gadget_exclusive_store_relaxed_h_rr :
			 uml_arm64_gadget_exclusive_store_relaxed_w_rr));
		push(t | d << 16 | n << 32);
	}
	push(key_);
	return {};
}

/* LDA, LDAB, LDAH, STL, STLB and STLH: the host's load-acquire and
 * store-release, whose alignment rule is the exclusives'. The operands are
 * the register and the register holding the address. */
Translator::Result Translator::emit_ordered(const Instruction &in,
					    const FormPlan &plan)
{
	const auto &o = in.operands;
	const auto t = static_cast<std::uint64_t>(o.t) * 8U;
	const auto n = static_cast<std::uint64_t>(o.n) * 8U;
	if (plan.aux == 0)
		push_external(plan.sub == 0 ?
			uml_arm64_gadget_ordered_load_acquire_b_r :
			plan.sub == 1 ?
			uml_arm64_gadget_ordered_load_acquire_h_r :
			uml_arm64_gadget_ordered_load_acquire_w_r);
	else
		push_external(plan.sub == 0 ?
			uml_arm64_gadget_ordered_store_release_b_r :
			plan.sub == 1 ?
			uml_arm64_gadget_ordered_store_release_h_r :
			uml_arm64_gadget_ordered_store_release_w_r);
	push(t | n << 16);
	push(key_);
	return {};
}

/* ------------------------------------------------------------ branches */

Translator::Result Translator::emit_branch(const Instruction &in,
					   const FormPlan &plan)
{
	const auto &o = in.operands;
	switch (plan.family) {
	case Family::branch: {
		const auto target = target_key(
			pc_value() + static_cast<std::uint32_t>(o.imm32), thumb_);
		if (in.cond >= 14) {
			emit_direct_exit(target);
			return {true, false};
		}
		if (linked() && !input_.single) {
			push_external(conditional_inline[in.cond]);
			push_link(target);
			patches_.emplace_back(words_.size(), compiled_ + 1);
			push(0);
			return {};
		}
		push_external(conditional_plain[in.cond]);
		push(target);
		push(next_key_);
		return {true, false};
	}
	case Family::bl: {
		const bool to_thumb = o.targetInstrSet == InstrSet::T32;
		const bool exchange = to_thumb != thumb_;
		const auto base = exchange ? pc_aligned() : pc_value();
		const auto target = target_key(
			base + static_cast<std::uint32_t>(o.imm32), to_thumb);
		credit();
		if (linked()) {
			push(G::bl_linked);
			push_link(target);
			push_link(next_key_);
		} else {
			push(G::bl);
			push(target);
		}
		push(return_value());
		return {true, false};
	}
	case Family::bx: {
		emit_indirect(source(o.m, scratch0), true, o.m == 14);
		return {true, false};
	}
	case Family::blx: {
		const auto src = source(o.m, scratch0);
		credit();
		if (indirects_ && linked()) {
			push(G::blx_cached);
			push(src);
			push(reinterpret_cast<std::uintptr_t>(indirects_(key_)));
			push_link(next_key_);
		} else {
			push(G::blx);
			push(src);
		}
		push(return_value());
		return {true, false};
	}
	case Family::cbz: {
		const auto target = target_key(
			pc_value() + static_cast<std::uint32_t>(o.imm32), true);
		const auto rn = static_cast<std::uintptr_t>(o.n) * 8U;
		const bool nonzero = plan.sub != 0;
		if (linked() && !input_.single) {
			push(nonzero ? G::cbnz_inline : G::cbz_inline);
			push(rn);
			push_link(target);
			patches_.emplace_back(words_.size(), compiled_ + 1);
			push(0);
			return {};
		}
		push(nonzero ? G::cbnz_plain : G::cbz_plain);
		push(rn);
		push(target);
		push(next_key_);
		return {true, false};
	}
	case Family::tb: {
		const auto rn = source(o.n, scratch0);
		const auto rm = static_cast<std::uintptr_t>(o.m) * 8U;
		credit();
		push(plan.sub == 0 ? G::tbb : G::tbh);
		push(key_);
		push(rn | rm << 8 | std::uint64_t{pc_value()} << 32);
		push(indirects_ ? reinterpret_cast<std::uintptr_t>(indirects_(key_))
				: reinterpret_cast<std::uintptr_t>(&dummy_site()));
		return {true, false};
	}
	default:
		return {};
	}
}

/* --------------------------------------------------------------- system */

Translator::Result Translator::emit_system(const Instruction &in,
					   const FormPlan &plan)
{
	const auto &o = in.operands;
	switch (plan.family) {
	case Family::svc:
		credit();
		push(G::svc);
		push(next_key_);
		return {true, false};
	case Family::bkpt:
		credit();
		push(G::bkpt);
		push(key_);
		return {true, false};
	case Family::hint:
		if (plan.aux != 0)
			push_external(uml_arm64_gadget_yield);
		return {};
	case Family::barrier:
		if (plan.sub == 2)
			push_external(uml_arm64_gadget_isb);
		else if (plan.sub == 0)
			push_external(data_barrier[in.word & 15U]);
		else
			push_external(synchronization_barrier[in.word & 15U]);
		return {};
	case Family::clrex:
		push_external(uml_arm64_gadget_clear_exclusive);
		return {};
	case Family::mrs:
		push(G::mrs);
		push(static_cast<std::uint64_t>(o.d) * 8U);
		return {};
	case Family::msr: {
		const unsigned mask = thumb_ ? in.word >> 10 & 3U :
					       in.word >> 18 & 3U;
		std::uintptr_t rn;
		if (in.form == Form::MSR_i_A1_AS) {
			set_scratch(scratch0, static_cast<std::uint32_t>(o.imm32));
			rn = scratch0;
		} else {
			rn = static_cast<std::uintptr_t>(o.n) * 8U;
		}
		if (mask == 0)
			return {};
		push(G::msr);
		push(rn | std::uint64_t{(mask >> 1) & 1U} << 8 |
		     std::uint64_t{mask & 1U} << 9);
		return {false, (mask & 2U) != 0};
	}
	case Family::cp15: {
		const auto t = static_cast<std::uint64_t>(o.t) * 8U;
		switch (in.sysreg) {
		case SysReg::tpidruro:
			push(G::read_tpidruro);
			push(t);
			return {};
		case SysReg::tpidrurw_read:
			push(G::read_tpidrurw);
			push(t);
			return {};
		case SysReg::tpidrurw_write:
			push(G::write_tpidrurw);
			push(t);
			return {};
		case SysReg::cntfrq:
			push(G::read_cntfrq);
			push(t);
			return {};
		case SysReg::cntvct:
			push(G::read_cntvct);
			push(t | static_cast<std::uint64_t>(o.t2) * 8U << 8);
			return {};
		case SysReg::cp15_dsb:
			push(G::cp15_dsb);
			push(key_);
			return {};
		case SysReg::cp15_dmb:
			push(G::cp15_dmb);
			push(key_);
			return {};
		case SysReg::cp15_isb:
			push(G::cp15_isb);
			push(key_);
			return {};
		case SysReg::none:
			break;
		}
		return {};
	}
	default:
		return {};
	}
}

Translator::Result Translator::emit(const Instruction &in,
				    const FormPlan &plan)
{
	switch (plan.family) {
	case Family::dp:
		return emit_dp(in, plan);
	case Family::mul:
	case Family::mull:
	case Family::mulh:
	case Family::mulxy:
	case Family::div:
		return emit_multiply(in, plan);
	case Family::movw:
	case Family::movt:
	case Family::adr:
	case Family::extend:
	case Family::bits:
	case Family::field:
	case Family::sat:
	case Family::pkh:
		return emit_integer(in, plan);
	case Family::library:
		return emit_library(in);
	case Family::load:
	case Family::store:
		return emit_single(in, plan);
	case Family::loadd:
	case Family::stored:
		return emit_double(in, plan);
	case Family::ldm:
	case Family::stm:
		return emit_block_transfer(in, plan);
	case Family::exclusive:
		return emit_exclusive(in, plan);
	case Family::ordered:
		return emit_ordered(in, plan);
	case Family::branch:
	case Family::bl:
	case Family::bx:
	case Family::blx:
	case Family::cbz:
	case Family::tb:
		return emit_branch(in, plan);
	case Family::it:
		return {};
	case Family::svc:
	case Family::bkpt:
	case Family::hint:
	case Family::barrier:
	case Family::clrex:
	case Family::mrs:
	case Family::msr:
	case Family::cp15:
		return emit_system(in, plan);
	case Family::vfp3:
	case Family::vfpmac:
	case Family::vfp2:
	case Family::vfpcmp:
	case Family::vfpimm:
	case Family::vfpcvt:
	case Family::vfpcore:
	case Family::vfpsys:
	case Family::vfpmem:
	case Family::vfpblock:
		return emit_vfp(in, plan);
	case Family::neon:
		return emit_neon(in, plan);
	case Family::neonmem:
		return emit_structure(in, plan);
	case Family::udf:
	case Family::none:
		break;
	}
	return {};
}

/* ----------------------------------------------------------------- run */

BuildOutput Translator::run()
{
	const auto *bytes = input_.code.data();
	const auto half = [&](std::size_t unit) {
		std::uint16_t value;
		std::memcpy(&value, bytes + unit * 2U, sizeof(value));
		return value;
	};
	const auto word_at = [&](std::size_t unit) {
		std::uint32_t value;
		std::memcpy(&value, bytes + unit * 4U, sizeof(value));
		return value;
	};

	while (!ended_ && compiled_ < limit_) {
		if (!input_.single && compiled_ >= soft_limit_ && !it_.active())
			break;
		key_ = input_.key + 4U * unit_;
		if (unit_ >= available_)
			break;
		std::size_t length = 1;
		std::uint32_t raw;
		Instruction in;
		if (thumb_) {
			const std::uint16_t hw1 = half(unit_);
			if (thumb_length(hw1) == 4) {
				if (unit_ + 1 >= available_)
					break;
				length = 2;
				raw = std::uint32_t{hw1} << 16 | half(unit_ + 1);
			} else {
				raw = std::uint32_t{hw1} << 16;
			}
			in = decode_thumb(raw, it_, profile_);
		} else {
			raw = word_at(unit_);
			in = decode_a32(raw, profile_);
		}
		next_key_ = key_ + 4U * length;
		const FormPlan plan = kFormPlans[static_cast<std::size_t>(in.form)];
		/* A coprocessor access the profile does not accept, and a
		 * coprocessor instruction of a level above the engine's, are
		 * undefined only when their condition passes (the pseudocode
		 * tests ConditionPassed first). Every other undefined word is
		 * undefined whatever the condition: the profile leaves it out at
		 * decode time, it is unallocated, or it is UNPREDICTABLE, and
		 * so is a coprocessor word the full profile finds UNPREDICTABLE
		 * (an encoding that is only valid under AL). The decoder
		 * reports an undefined word's condition as 14, so a conditional
		 * one takes the word's own field. */
		if (in.status == Status::undefined) {
			unsigned cond = 14;
			const auto full = Profile::at(Level::complete);
			if (thumb_) {
				if (in.in_it && (in.reason == Reason::profile ||
						 in.reason == Reason::feature)) {
					const auto whole = decode_thumb(raw, it_, full);
					if (whole.status != Status::undefined ||
					    whole.reason != Reason::unpredictable)
						cond = in.cond;
				}
			} else {
				const bool coprocessor = (raw >> 25 & 7U) == 6U ||
							 (raw >> 24 & 15U) == 14U;
				if (coprocessor && (raw >> 28) < 14U &&
				    (in.reason == Reason::profile ||
				     in.reason == Reason::feature ||
				     in.reason == Reason::unallocated)) {
					const auto whole = decode_a32(raw, full);
					if (whole.status != Status::undefined ||
					    whole.reason != Reason::unpredictable)
						cond = raw >> 28;
				}
			}
			if (cond < 14) {
				begin_instruction(cond);
				push_external(uml_arm64_gadget_illegal);
				push(key_);
				it_ = it_.advance();
				unit_ += length;
				units_ = unit_;
				++compiled_;
				if (input_.single)
					break;
				continue;
			}
		}
		if (in.status == Status::undefined ||
		    plan.family == Family::none || plan.family == Family::udf) {
			units_ = unit_ + length;
			finish_stop(true, raw);
			break;
		}
		if (in.status == Status::ok) {
			/* A conditional branch tests its own condition; every
			 * other conditional instruction is behind a skip. */
			if (plan.family == Family::branch ||
			    plan.family == Family::cbz || plan.family == Family::it)
				close_run();
			else
				begin_instruction(in.cond);
			const Result result = emit(in, plan);
			if (result.flags)
				flags_written();
			if (result.terminal && !run_active_)
				ended_ = true;
		}
		it_ = in.next_it;
		unit_ += length;
		units_ = unit_;
		++compiled_;
		if (input_.single)
			break;
	}

	const bool stopped = !complete_;
	if (!stopped && !ended_) {
		units_ = unit_;
		emit_fallthrough();
	} else {
		close_run();
	}
	for (const auto &[index, through] : patches_)
		words_[index] = compiled_ > through ? compiled_ - through : 0;

	BuildOutput output{
		BuildResult{Program{input_.key, compiled_, std::move(words_),
				    uses_fp_, false},
			    complete_, stop_address_, stop_instruction_},
		units_, it_};
	return output;
}

} // namespace detail

BuildOutput build(const BuildInput &input,
		  const Builder::LinkResolver &resolve_link,
		  std::pmr::memory_resource *storage,
		  const Builder::IndirectLinkResolver &resolve_indirect_link,
		  const BuildOptions &, const Profile &profile,
		  std::size_t block_instructions)
{
	return detail::Translator{input, resolve_link, storage, resolve_indirect_link,
			  profile, block_instructions}.run();
}

std::optional<std::size_t> thumb_position(
	std::span<const std::byte> halfwords, std::size_t unit) noexcept
{
	std::size_t position = 0;
	std::size_t at = 0;
	const auto count = halfwords.size() / 2U;
	while (at < count) {
		if (at == unit)
			return position;
		if (at > unit)
			return std::nullopt;
		std::uint16_t hw1;
		std::memcpy(&hw1, halfwords.data() + at * 2U, sizeof(hw1));
		at += thumb_length(hw1) / 2U;
		++position;
	}
	return at == unit ? std::optional{position} : std::nullopt;
}

bool routes_to_library(aarch32::Form form) noexcept
{
	return kFormPlans[static_cast<std::size_t>(form)].family ==
	       Family::library;
}

bool may_reach_library(aarch32::Form form) noexcept
{
	const auto family =
		kFormPlans[static_cast<std::size_t>(form)].family;
	return family == Family::library || family == Family::ldm ||
	       family == Family::stm || family == Family::vfpblock ||
	       family == Family::neonmem;
}

std::uint8_t thumb_itstate(std::span<const std::byte> halfwords,
			   std::size_t unit, const Profile &profile) noexcept
{
	ItState state;
	std::size_t at = 0;
	const auto count = halfwords.size() / 2U;
	while (at < count && at < unit) {
		std::uint16_t hw1;
		std::uint16_t hw2 = 0;
		std::memcpy(&hw1, halfwords.data() + at * 2U, sizeof(hw1));
		const auto length = thumb_length(hw1) / 2U;
		if (length == 2) {
			if (at + 1 >= count)
				break;
			std::memcpy(&hw2, halfwords.data() + (at + 1) * 2U,
				    sizeof(hw2));
		}
		const auto decoded = decode_thumb(
			std::uint32_t{hw1} << 16 | hw2, state, profile);
		state = decoded.next_it;
		at += length;
	}
	return state.bits();
}

} // namespace uml::cpu::arm64::gadgets::a32
