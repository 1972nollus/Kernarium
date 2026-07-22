// SPDX-License-Identifier: GPL-2.0-or-later
#include "program.hpp"

#include "../detail/instruction_helpers.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>

namespace uml::backend::arm64::gadgets {
namespace {

extern "C" {
int uml_arm64_gadget_enter(const std::uintptr_t *program, CpuState *cpu,
			    Tlb *tlb);
void uml_arm64_gadget_movz();
void uml_arm64_gadget_movk();
void uml_arm64_gadget_movn();
void uml_arm64_gadget_add_sub_immediate();
void uml_arm64_gadget_add_sub_register();
void uml_arm64_gadget_add_sub_carry();
void uml_arm64_gadget_multiply_add();
void uml_arm64_gadget_variable_shift();
void uml_arm64_gadget_divide();
void uml_arm64_gadget_crc32();
void uml_arm64_gadget_extract_register();
void uml_arm64_gadget_conditional_select();
void uml_arm64_gadget_conditional_compare_eq();
void uml_arm64_gadget_conditional_compare_ne();
void uml_arm64_gadget_conditional_compare_hs();
void uml_arm64_gadget_conditional_compare_lo();
void uml_arm64_gadget_conditional_compare_mi();
void uml_arm64_gadget_conditional_compare_pl();
void uml_arm64_gadget_conditional_compare_vs();
void uml_arm64_gadget_conditional_compare_vc();
void uml_arm64_gadget_conditional_compare_hi();
void uml_arm64_gadget_conditional_compare_ls();
void uml_arm64_gadget_conditional_compare_ge();
void uml_arm64_gadget_conditional_compare_lt();
void uml_arm64_gadget_conditional_compare_gt();
void uml_arm64_gadget_conditional_compare_le();
void uml_arm64_gadget_conditional_compare_always();
void uml_arm64_gadget_logical_immediate();
void uml_arm64_gadget_logical_shifted();
void uml_arm64_gadget_bitfield();
void uml_arm64_gadget_bit_reverse_count();
void uml_arm64_gadget_floating_arithmetic();
void uml_arm64_gadget_floating_multiply_add();
void uml_arm64_gadget_floating_move_immediate();
void uml_arm64_gadget_floating_move_single();
void uml_arm64_gadget_floating_move_double();
void uml_arm64_gadget_floating_absolute_single();
void uml_arm64_gadget_floating_absolute_double();
void uml_arm64_gadget_floating_negate_single();
void uml_arm64_gadget_floating_negate_double();
void uml_arm64_gadget_floating_sqrt_single();
void uml_arm64_gadget_floating_sqrt_double();
void uml_arm64_gadget_floating_multiply_single();
void uml_arm64_gadget_floating_multiply_double();
void uml_arm64_gadget_floating_divide_single();
void uml_arm64_gadget_floating_divide_double();
void uml_arm64_gadget_floating_add_single();
void uml_arm64_gadget_floating_add_double();
void uml_arm64_gadget_floating_subtract_single();
void uml_arm64_gadget_floating_subtract_double();
void uml_arm64_gadget_floating_negative_multiply_single();
void uml_arm64_gadget_floating_negative_multiply_double();
void uml_arm64_gadget_floating_madd_single();
void uml_arm64_gadget_floating_madd_double();
void uml_arm64_gadget_floating_msub_single();
void uml_arm64_gadget_floating_msub_double();
void uml_arm64_gadget_floating_nmadd_single();
void uml_arm64_gadget_floating_nmadd_double();
void uml_arm64_gadget_floating_nmsub_single();
void uml_arm64_gadget_floating_nmsub_double();
void uml_arm64_gadget_floating_compare();
void uml_arm64_gadget_floating_convert_precision();
void uml_arm64_gadget_floating_integer_convert();
void uml_arm64_gadget_floating_to_integer_fixed_s32_9();
void uml_arm64_gadget_floating_round_integral();
void uml_arm64_gadget_floating_absolute_difference();
#define UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(name) \
	void uml_arm64_gadget_floating_conditional_compare_##name();
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(eq)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(ne)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(hs)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(lo)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(mi)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(pl)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(vs)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(vc)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(hi)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(ls)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(ge)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(lt)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(gt)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(le)
UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(always)
#undef UML_DECLARE_FLOATING_CONDITIONAL_COMPARE
void uml_arm64_gadget_simd_and();
void uml_arm64_gadget_simd_bic();
void uml_arm64_gadget_simd_orr();
void uml_arm64_gadget_simd_orn();
void uml_arm64_gadget_simd_eor();
void uml_arm64_gadget_simd_bsl();
void uml_arm64_gadget_simd_bit();
void uml_arm64_gadget_simd_bif();
void uml_arm64_gadget_simd_modified_immediate();
void uml_arm64_gadget_simd_compare_zero();
void uml_arm64_gadget_simd_compare_register();
void uml_arm64_gadget_simd_min_max();
void uml_arm64_gadget_simd_count_bits();
void uml_arm64_gadget_simd_not();
void uml_arm64_gadget_simd_addv();
#define UML_DECLARE_SIMD_EXTRACT_Q(index) void uml_arm64_gadget_simd_extract_q##index();
UML_DECLARE_SIMD_EXTRACT_Q(0) UML_DECLARE_SIMD_EXTRACT_Q(1)
UML_DECLARE_SIMD_EXTRACT_Q(2) UML_DECLARE_SIMD_EXTRACT_Q(3)
UML_DECLARE_SIMD_EXTRACT_Q(4) UML_DECLARE_SIMD_EXTRACT_Q(5)
UML_DECLARE_SIMD_EXTRACT_Q(6) UML_DECLARE_SIMD_EXTRACT_Q(7)
UML_DECLARE_SIMD_EXTRACT_Q(8) UML_DECLARE_SIMD_EXTRACT_Q(9)
UML_DECLARE_SIMD_EXTRACT_Q(10) UML_DECLARE_SIMD_EXTRACT_Q(11)
UML_DECLARE_SIMD_EXTRACT_Q(12) UML_DECLARE_SIMD_EXTRACT_Q(13)
UML_DECLARE_SIMD_EXTRACT_Q(14) UML_DECLARE_SIMD_EXTRACT_Q(15)
#undef UML_DECLARE_SIMD_EXTRACT_Q
#define UML_DECLARE_SIMD_EXTRACT_D(index) void uml_arm64_gadget_simd_extract_d##index();
UML_DECLARE_SIMD_EXTRACT_D(0) UML_DECLARE_SIMD_EXTRACT_D(1)
UML_DECLARE_SIMD_EXTRACT_D(2) UML_DECLARE_SIMD_EXTRACT_D(3)
UML_DECLARE_SIMD_EXTRACT_D(4) UML_DECLARE_SIMD_EXTRACT_D(5)
UML_DECLARE_SIMD_EXTRACT_D(6) UML_DECLARE_SIMD_EXTRACT_D(7)
#undef UML_DECLARE_SIMD_EXTRACT_D
void uml_arm64_gadget_simd_shift_right_narrow();
void uml_arm64_gadget_simd_narrow_move();
void uml_arm64_gadget_simd_arithmetic_narrow();
void uml_arm64_gadget_simd_add_sub_pairwise();
void uml_arm64_gadget_simd_multiply();
void uml_arm64_gadget_simd_shift_right();
void uml_arm64_gadget_simd_shift_left();
void uml_arm64_gadget_simd_shift_left_long();
void uml_arm64_gadget_simd_shift_register();
void uml_arm64_gadget_simd_table_lookup();
void uml_arm64_gadget_simd_reverse_64();
void uml_arm64_gadget_simd_reverse_32();
void uml_arm64_gadget_simd_reverse_16();
void uml_arm64_gadget_simd_permute();
void uml_arm64_gadget_simd_duplicate_element();
void uml_arm64_gadget_simd_vector_element_to_gpr();
void uml_arm64_gadget_simd_gpr_to_vector_element();
void uml_arm64_gadget_simd_insert_element();
void uml_arm64_gadget_simd_integer_float_convert();
void uml_arm64_gadget_simd_arithmetic_wide();
void uml_arm64_gadget_simd_floating_unary();
void uml_arm64_gadget_vector_to_gpr_64_low();
void uml_arm64_gadget_gpr_to_vector_64_low();
void uml_arm64_gadget_vector_to_gpr_32_low();
void uml_arm64_gadget_gpr_to_vector_32_low();
void uml_arm64_gadget_vector_to_gpr_64_high();
void uml_arm64_gadget_gpr_to_vector_64_high();
void uml_arm64_gadget_floating_select_eq();
void uml_arm64_gadget_floating_select_ne();
void uml_arm64_gadget_floating_select_hs();
void uml_arm64_gadget_floating_select_lo();
void uml_arm64_gadget_floating_select_mi();
void uml_arm64_gadget_floating_select_pl();
void uml_arm64_gadget_floating_select_vs();
void uml_arm64_gadget_floating_select_vc();
void uml_arm64_gadget_floating_select_hi();
void uml_arm64_gadget_floating_select_ls();
void uml_arm64_gadget_floating_select_ge();
void uml_arm64_gadget_floating_select_lt();
void uml_arm64_gadget_floating_select_gt();
void uml_arm64_gadget_floating_select_le();
void uml_arm64_gadget_floating_select_always();
void uml_arm64_gadget_pc_relative();
void uml_arm64_gadget_tls_read();
void uml_arm64_gadget_tls_write();
void uml_arm64_gadget_system_register();
void uml_arm64_gadget_memory_barrier();
void uml_arm64_gadget_dup_gpr_to_vector();
void uml_arm64_gadget_nop();
void uml_arm64_gadget_load32_unsigned();
void uml_arm64_gadget_load64_unsigned();
void uml_arm64_gadget_store32_unsigned();
void uml_arm64_gadget_store64_unsigned();
void uml_arm64_gadget_load_store_integer_unsigned();
void uml_arm64_gadget_load_store_vector_unsigned();
void uml_arm64_gadget_load_store_integer_other();
void uml_arm64_gadget_load_store_vector_other();
void uml_arm64_gadget_load_store_pair();
void uml_arm64_gadget_load_store_vector_pair();
void uml_arm64_gadget_load_store_simd_structure();
void uml_arm64_gadget_load_simd_replicate();
void uml_arm64_gadget_load_store_simd_lane();
void uml_arm64_gadget_load_store_ordered();
void uml_arm64_gadget_load_store_exclusive();
void uml_arm64_gadget_svc();
void uml_arm64_gadget_interrupt();
void uml_arm64_gadget_fatal();
void uml_arm64_gadget_illegal();
void uml_arm64_gadget_branch();
void uml_arm64_gadget_branch_link();
void uml_arm64_gadget_branch_register();
void uml_arm64_gadget_branch_link_register();
void uml_arm64_gadget_return();
void uml_arm64_gadget_branch_eq();
void uml_arm64_gadget_branch_ne();
void uml_arm64_gadget_branch_hs();
void uml_arm64_gadget_branch_lo();
void uml_arm64_gadget_branch_mi();
void uml_arm64_gadget_branch_pl();
void uml_arm64_gadget_branch_vs();
void uml_arm64_gadget_branch_vc();
void uml_arm64_gadget_branch_hi();
void uml_arm64_gadget_branch_ls();
void uml_arm64_gadget_branch_ge();
void uml_arm64_gadget_branch_lt();
void uml_arm64_gadget_branch_gt();
void uml_arm64_gadget_branch_le();
void uml_arm64_gadget_branch_always();
void uml_arm64_gadget_compare_branch();
void uml_arm64_gadget_test_branch();
#if UML_ENABLE_GADGET_INTERPRETER_RUNPATH
void uml_arm64_gadget_decoded();
#endif
}

extern "C" std::uint64_t uml_arm64_gadget_counter_read()
{
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

using Gadget = void (*)();

static_assert(offsetof(CpuState, registers) == 0);
static_assert(offsetof(CpuState, sp) == 248);
static_assert(offsetof(CpuState, pc) == 256);
static_assert(offsetof(CpuState, nzcv) == 264);
static_assert(offsetof(CpuState, vectors) == 272);
static_assert(offsetof(CpuState, fpcr) == 784);
static_assert(offsetof(CpuState, fpsr) == 788);
static_assert(offsetof(CpuState, tls) == 792);

void emit(std::vector<std::uintptr_t> &words, Gadget gadget)
{
	words.push_back(reinterpret_cast<std::uintptr_t>(gadget));
}

void emit(std::vector<std::uintptr_t> &words, std::uintptr_t operand)
{
	words.push_back(operand);
}

void emit_interpreter_or_fatal(std::vector<std::uintptr_t> &words,
			       std::uint32_t instruction,
			       std::uint64_t address)
{
#if UML_ENABLE_GADGET_INTERPRETER_RUNPATH
	emit(words, uml_arm64_gadget_decoded);
	emit(words, instruction);
#else
	(void)instruction;
	emit(words, uml_arm64_gadget_fatal);
#endif
	emit(words, address);
}

[[nodiscard]] Gadget conditional_branch_gadget(unsigned condition) noexcept
{
	switch (condition) {
	case 0: return uml_arm64_gadget_branch_eq;
	case 1: return uml_arm64_gadget_branch_ne;
	case 2: return uml_arm64_gadget_branch_hs;
	case 3: return uml_arm64_gadget_branch_lo;
	case 4: return uml_arm64_gadget_branch_mi;
	case 5: return uml_arm64_gadget_branch_pl;
	case 6: return uml_arm64_gadget_branch_vs;
	case 7: return uml_arm64_gadget_branch_vc;
	case 8: return uml_arm64_gadget_branch_hi;
	case 9: return uml_arm64_gadget_branch_ls;
	case 10: return uml_arm64_gadget_branch_ge;
	case 11: return uml_arm64_gadget_branch_lt;
	case 12: return uml_arm64_gadget_branch_gt;
	case 13: return uml_arm64_gadget_branch_le;
	default: return uml_arm64_gadget_branch_always;
	}
}

[[nodiscard]] Gadget conditional_compare_gadget(unsigned condition) noexcept
{
	switch (condition) {
	case 0: return uml_arm64_gadget_conditional_compare_eq;
	case 1: return uml_arm64_gadget_conditional_compare_ne;
	case 2: return uml_arm64_gadget_conditional_compare_hs;
	case 3: return uml_arm64_gadget_conditional_compare_lo;
	case 4: return uml_arm64_gadget_conditional_compare_mi;
	case 5: return uml_arm64_gadget_conditional_compare_pl;
	case 6: return uml_arm64_gadget_conditional_compare_vs;
	case 7: return uml_arm64_gadget_conditional_compare_vc;
	case 8: return uml_arm64_gadget_conditional_compare_hi;
	case 9: return uml_arm64_gadget_conditional_compare_ls;
	case 10: return uml_arm64_gadget_conditional_compare_ge;
	case 11: return uml_arm64_gadget_conditional_compare_lt;
	case 12: return uml_arm64_gadget_conditional_compare_gt;
	case 13: return uml_arm64_gadget_conditional_compare_le;
	default: return uml_arm64_gadget_conditional_compare_always;
	}
}

[[nodiscard]] Gadget simd_extract_gadget(bool q, unsigned offset) noexcept
{
	static constexpr std::array<Gadget, 16> q_gadgets{
		uml_arm64_gadget_simd_extract_q0, uml_arm64_gadget_simd_extract_q1,
		uml_arm64_gadget_simd_extract_q2, uml_arm64_gadget_simd_extract_q3,
		uml_arm64_gadget_simd_extract_q4, uml_arm64_gadget_simd_extract_q5,
		uml_arm64_gadget_simd_extract_q6, uml_arm64_gadget_simd_extract_q7,
		uml_arm64_gadget_simd_extract_q8, uml_arm64_gadget_simd_extract_q9,
		uml_arm64_gadget_simd_extract_q10, uml_arm64_gadget_simd_extract_q11,
		uml_arm64_gadget_simd_extract_q12, uml_arm64_gadget_simd_extract_q13,
		uml_arm64_gadget_simd_extract_q14, uml_arm64_gadget_simd_extract_q15,
	};
	static constexpr std::array<Gadget, 8> d_gadgets{
		uml_arm64_gadget_simd_extract_d0, uml_arm64_gadget_simd_extract_d1,
		uml_arm64_gadget_simd_extract_d2, uml_arm64_gadget_simd_extract_d3,
		uml_arm64_gadget_simd_extract_d4, uml_arm64_gadget_simd_extract_d5,
		uml_arm64_gadget_simd_extract_d6, uml_arm64_gadget_simd_extract_d7,
	};
	return q ? q_gadgets[offset] : d_gadgets[offset];
}

[[nodiscard]] Gadget floating_arithmetic_gadget(
	bool is_double, bool one_source, unsigned operation) noexcept
{
	if (one_source) {
		switch (operation) {
		case 0: return is_double ? uml_arm64_gadget_floating_move_double :
			uml_arm64_gadget_floating_move_single;
		case 1: return is_double ? uml_arm64_gadget_floating_absolute_double :
			uml_arm64_gadget_floating_absolute_single;
		case 2: return is_double ? uml_arm64_gadget_floating_negate_double :
			uml_arm64_gadget_floating_negate_single;
		default: return is_double ? uml_arm64_gadget_floating_sqrt_double :
			uml_arm64_gadget_floating_sqrt_single;
		}
	}
	switch (operation) {
	case 0: return is_double ? uml_arm64_gadget_floating_multiply_double :
		uml_arm64_gadget_floating_multiply_single;
	case 1: return is_double ? uml_arm64_gadget_floating_divide_double :
		uml_arm64_gadget_floating_divide_single;
	case 2: return is_double ? uml_arm64_gadget_floating_add_double :
		uml_arm64_gadget_floating_add_single;
	case 3: return is_double ? uml_arm64_gadget_floating_subtract_double :
		uml_arm64_gadget_floating_subtract_single;
	default: return is_double ?
		uml_arm64_gadget_floating_negative_multiply_double :
		uml_arm64_gadget_floating_negative_multiply_single;
	}
}

[[nodiscard]] Gadget floating_multiply_add_gadget(
	bool is_double, bool negate, bool subtract) noexcept
{
	if (!negate && !subtract)
		return is_double ? uml_arm64_gadget_floating_madd_double :
			uml_arm64_gadget_floating_madd_single;
	if (!negate)
		return is_double ? uml_arm64_gadget_floating_msub_double :
			uml_arm64_gadget_floating_msub_single;
	if (!subtract)
		return is_double ? uml_arm64_gadget_floating_nmadd_double :
			uml_arm64_gadget_floating_nmadd_single;
	return is_double ? uml_arm64_gadget_floating_nmsub_double :
		uml_arm64_gadget_floating_nmsub_single;
}

[[nodiscard]] Gadget simd_bitwise_gadget(unsigned operation) noexcept
{
	switch (operation) {
	case 0: return uml_arm64_gadget_simd_and;
	case 1: return uml_arm64_gadget_simd_bic;
	case 2: return uml_arm64_gadget_simd_orr;
	case 3: return uml_arm64_gadget_simd_orn;
	case 4: return uml_arm64_gadget_simd_eor;
	case 5: return uml_arm64_gadget_simd_bsl;
	case 6: return uml_arm64_gadget_simd_bit;
	default: return uml_arm64_gadget_simd_bif;
	}
}

[[nodiscard]] Gadget floating_select_gadget(unsigned condition) noexcept
{
	switch (condition) {
	case 0: return uml_arm64_gadget_floating_select_eq;
	case 1: return uml_arm64_gadget_floating_select_ne;
	case 2: return uml_arm64_gadget_floating_select_hs;
	case 3: return uml_arm64_gadget_floating_select_lo;
	case 4: return uml_arm64_gadget_floating_select_mi;
	case 5: return uml_arm64_gadget_floating_select_pl;
	case 6: return uml_arm64_gadget_floating_select_vs;
	case 7: return uml_arm64_gadget_floating_select_vc;
	case 8: return uml_arm64_gadget_floating_select_hi;
	case 9: return uml_arm64_gadget_floating_select_ls;
	case 10: return uml_arm64_gadget_floating_select_ge;
	case 11: return uml_arm64_gadget_floating_select_lt;
	case 12: return uml_arm64_gadget_floating_select_gt;
	case 13: return uml_arm64_gadget_floating_select_le;
	default: return uml_arm64_gadget_floating_select_always;
	}
}

[[nodiscard]] Gadget floating_conditional_compare_gadget(
	unsigned condition) noexcept
{
	switch (condition) {
	case 0: return uml_arm64_gadget_floating_conditional_compare_eq;
	case 1: return uml_arm64_gadget_floating_conditional_compare_ne;
	case 2: return uml_arm64_gadget_floating_conditional_compare_hs;
	case 3: return uml_arm64_gadget_floating_conditional_compare_lo;
	case 4: return uml_arm64_gadget_floating_conditional_compare_mi;
	case 5: return uml_arm64_gadget_floating_conditional_compare_pl;
	case 6: return uml_arm64_gadget_floating_conditional_compare_vs;
	case 7: return uml_arm64_gadget_floating_conditional_compare_vc;
	case 8: return uml_arm64_gadget_floating_conditional_compare_hi;
	case 9: return uml_arm64_gadget_floating_conditional_compare_ls;
	case 10: return uml_arm64_gadget_floating_conditional_compare_ge;
	case 11: return uml_arm64_gadget_floating_conditional_compare_lt;
	case 12: return uml_arm64_gadget_floating_conditional_compare_gt;
	case 13: return uml_arm64_gadget_floating_conditional_compare_le;
	default: return uml_arm64_gadget_floating_conditional_compare_always;
	}
}

struct SimdImmediate final {
	std::uint64_t pattern{};
	unsigned action{}; // 0 = replace, 1 = ORR, 2 = BIC
};

[[nodiscard]] std::optional<SimdImmediate> decode_simd_immediate(
	std::uint32_t instruction)
{
	const bool q = (instruction & 0x4000'0000U) != 0;
	const bool invert = (instruction & 0x2000'0000U) != 0;
	const unsigned mode = (instruction >> 12) & 15U;
	const auto immediate = static_cast<std::uint8_t>(
		(((instruction >> 16) & 7U) << 5) | ((instruction >> 5) & 31U));
	std::uint64_t pattern;
	const bool combine = (mode & 1U) != 0 && mode != 13 && mode != 15;
	if (mode == 14 && invert) {
		pattern = 0;
		for (unsigned index = 0; index < 8; ++index)
			if ((immediate & (1U << index)) != 0)
				pattern |= 0xffULL << (index * 8);
	} else if (mode == 15) {
		const std::uint64_t sign = immediate >> 7;
		const std::uint64_t exponent_bit = (immediate >> 6) & 1U;
		const std::uint64_t fraction = immediate & 0x3fU;
		if (!invert) {
			const std::uint32_t exponent = ((1U - exponent_bit) << 7) |
				(exponent_bit * 0x7cU) | ((fraction >> 4) & 3U);
			const std::uint32_t fp = (static_cast<std::uint32_t>(sign) << 31) |
				(exponent << 23) |
				(static_cast<std::uint32_t>(fraction & 15U) << 19);
			pattern = fp | (static_cast<std::uint64_t>(fp) << 32);
		} else {
			if (!q)
				return std::nullopt;
			const std::uint64_t exponent = ((1U - exponent_bit) << 10) |
				(exponent_bit * 0x3fcU) | ((fraction >> 4) & 3U);
			pattern = (sign << 63) | (exponent << 52) |
				((fraction & 15U) << 48);
		}
	} else if (mode == 12) {
		const auto element = (static_cast<std::uint64_t>(immediate) << 8) |
			0xffU;
		pattern = element | (element << 32);
	} else if (mode == 13) {
		const auto element = (static_cast<std::uint64_t>(immediate) << 16) |
			0xffffU;
		pattern = element | (element << 32);
	} else {
		const auto decoded = detail::simd_integer_immediate(
			combine ? mode - 1 : mode, immediate);
		if (!decoded)
			return std::nullopt;
		pattern = *decoded;
	}
	if (!combine && invert && mode != 14 && mode != 15)
		pattern = ~pattern;
	return SimdImmediate{pattern, combine ? (invert ? 2U : 1U) : 0U};
}

[[nodiscard]] std::uintptr_t move_wide_operand(std::uint32_t instruction)
{
	const auto rd = instruction & 31U;
	const auto hw = (instruction >> 21) & 3U;
	const auto immediate = (instruction >> 5) & 0xffffU;
	const auto sf = instruction >> 31;
	return rd | (static_cast<std::uintptr_t>(hw) << 8) |
		(static_cast<std::uintptr_t>(immediate) << 16) |
		(static_cast<std::uintptr_t>(sf) << 32);
}

[[nodiscard]] constexpr std::int64_t sign_extend(std::uint64_t value,
						 unsigned bits) noexcept
{
	const auto shift = 64U - bits;
	return static_cast<std::int64_t>(value << shift) >> shift;
}

[[nodiscard]] constexpr bool accesses_fp_state(
	std::uint32_t instruction) noexcept
{
	/* A64 assigns all scalar FP/Advanced SIMD data processing encodings to
	 * op0=x111. SIMD loads/stores use the ordinary load/store classes with
	 * their V bit set. Include FPCR/FPSR system-register accesses as well.
	 * Conservatively treating reads and vector stores as writes keeps the lazy
	 * boundary state simple while scalar syscall paths avoid the 528-byte copy.
	 */
	const bool data_processing =
		(instruction & 0x0e00'0000U) == 0x0e00'0000U;
	const bool vector_load_store =
		(instruction & 0x0a00'0000U) == 0x0800'0000U &&
		(instruction & 0x0400'0000U) != 0;
	const auto system = instruction & 0xffff'ffe0U;
	const bool fp_system_register =
		system == 0xd53b'4400U || system == 0xd51b'4400U ||
		system == 0xd53b'4420U || system == 0xd51b'4420U;
	return data_processing || vector_load_store || fp_system_register;
}

} // namespace

Program::Program(std::uint64_t guest_address, std::size_t instruction_count,
		 std::vector<std::uintptr_t> words, bool accesses_fp_state) noexcept
	: guest_address_{guest_address}, instruction_count_{instruction_count},
	  words_{std::move(words)}, accesses_fp_state_{accesses_fp_state}
{
}

BuildResult Builder::build(
	std::uint64_t guest_address,
	std::span<const std::uint32_t> instructions) const
{
	std::vector<std::uintptr_t> words;
	words.reserve(instructions.size() * 2 + 2);
	std::size_t compiled = 0;
	bool program_accesses_fp_state = false;
	for (const auto instruction : instructions) {
		program_accesses_fp_state = program_accesses_fp_state ||
			accesses_fp_state(instruction);
		const auto address = guest_address + compiled * sizeof(instruction);
		if ((instruction & 0xffff'0000U) == 0) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
#if UML_ENABLE_GADGET_INTERPRETER_RUNPATH
		} else if ((instruction & 0x7c00'0000U) != 0x1400'0000U &&
			   (instruction & 0xff00'0010U) != 0x5400'0000U &&
			   (instruction & 0x7e00'0000U) != 0x3400'0000U &&
			   (instruction & 0x7e00'0000U) != 0x3600'0000U &&
			   (instruction & 0xffff'fc1fU) != 0xd61f'0000U &&
			   (instruction & 0xffff'fc1fU) != 0xd63f'0000U &&
			   (instruction & 0xffff'fc1fU) != 0xd65f'0000U &&
			   (instruction & 0xffe0'001fU) != 0xd400'0001U) {
			emit(words, uml_arm64_gadget_decoded);
			emit(words, instruction);
			emit(words, address);
#endif
		} else if ((instruction & 0x7c00'0000U) == 0x1400'0000U) {
			const auto displacement = sign_extend(
				static_cast<std::uint64_t>(instruction & 0x03ff'ffffU) << 2,
				28);
			const auto target = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + displacement);
			const bool link = (instruction >> 31) != 0;
			emit(words, link ? uml_arm64_gadget_branch_link :
				uml_arm64_gadget_branch);
			emit(words, target);
			if (link)
				emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0xff00'0010U) == 0x5400'0000U) {
			const auto displacement = sign_extend(
				static_cast<std::uint64_t>((instruction >> 5) & 0x7ffffU) << 2,
				21);
			emit(words, conditional_branch_gadget(instruction & 15U));
			emit(words, static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + displacement));
			emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0x7e00'0000U) == 0x3400'0000U) {
			const auto displacement = sign_extend(
				static_cast<std::uint64_t>((instruction >> 5) & 0x7ffffU) << 2,
				21);
			emit(words, uml_arm64_gadget_compare_branch);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>(instruction >> 31) << 8) |
				(static_cast<std::uintptr_t>((instruction >> 24) & 1U) << 9));
			emit(words, static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + displacement));
			emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0x7e00'0000U) == 0x3600'0000U) {
			const auto displacement = sign_extend(
				static_cast<std::uint64_t>((instruction >> 5) & 0x3fffU) << 2,
				16);
			const auto bit = ((instruction >> 31) << 5) |
				((instruction >> 19) & 31U);
			emit(words, uml_arm64_gadget_test_branch);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>(bit) << 8) |
				(static_cast<std::uintptr_t>((instruction >> 24) & 1U) << 16));
			emit(words, static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + displacement));
			emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0xffff'fc1fU) == 0xd61f'0000U ||
			   (instruction & 0xffff'fc1fU) == 0xd63f'0000U ||
			   (instruction & 0xffff'fc1fU) == 0xd65f'0000U) {
			const auto opcode = instruction & 0xffff'fc1fU;
			emit(words, opcode == 0xd61f'0000U ?
				uml_arm64_gadget_branch_register :
				opcode == 0xd63f'0000U ?
				uml_arm64_gadget_branch_link_register :
				uml_arm64_gadget_return);
			emit(words, (instruction >> 5) & 31U);
			if (opcode == 0xd63f'0000U)
				emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0x1f00'0000U) == 0x1000'0000U) {
			const auto immediate = static_cast<std::uint64_t>(
				((instruction >> 5) & 0x7ffffU) << 2) |
				((instruction >> 29) & 3U);
			const auto relative = sign_extend(immediate, 21);
			const auto value = (instruction >> 31) != 0 ?
				static_cast<std::uint64_t>(
					static_cast<std::int64_t>(address & ~0xfffULL) +
					relative * 4096) :
				static_cast<std::uint64_t>(
					static_cast<std::int64_t>(address) + relative);
			emit(words, uml_arm64_gadget_pc_relative);
			emit(words, instruction & 31U);
			emit(words, value);
		} else if ((instruction & 0xffff'ffe0U) == 0xd53b'd040U ||
			   (instruction & 0xffff'ffe0U) == 0xd51b'd040U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'00e0U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'0020U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'4400U ||
			   (instruction & 0xffff'ffe0U) == 0xd51b'4400U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'4420U ||
			   (instruction & 0xffff'ffe0U) == 0xd51b'4420U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'4200U ||
			   (instruction & 0xffff'ffe0U) == 0xd51b'4200U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'e000U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'e040U) {
			emit(words, uml_arm64_gadget_system_register);
			emit(words, instruction);
		} else if ((instruction & 0xffff'f0ffU) == 0xd503'309fU ||
			   (instruction & 0xffff'f0ffU) == 0xd503'30bfU ||
			   (instruction & 0xffff'f0ffU) == 0xd503'30dfU) {
			emit(words, uml_arm64_gadget_memory_barrier);
			emit(words, instruction);
		} else if ((instruction & 0x7f80'0000U) == 0x1280'0000U) {
			emit(words, uml_arm64_gadget_movn);
			emit(words, move_wide_operand(instruction));
		} else if ((instruction & 0x7f80'0000U) == 0x5280'0000U) {
			emit(words, uml_arm64_gadget_movz);
			emit(words, move_wide_operand(instruction));
		} else if ((instruction & 0x7f80'0000U) == 0x7280'0000U) {
			emit(words, uml_arm64_gadget_movk);
			emit(words, move_wide_operand(instruction));
		} else if ((instruction & 0x1f00'0000U) == 0x1100'0000U) {
			emit(words, uml_arm64_gadget_add_sub_immediate);
			emit(words, instruction);
		} else if ((instruction & 0x1f00'0000U) == 0x0b00'0000U) {
			const bool extended = (instruction & 0x0020'0000U) != 0;
			const unsigned shift = (instruction >> 10) &
				(extended ? 0x7U : 0x3fU);
			const unsigned shift_type = (instruction >> 22) & 0x3U;
			const bool is_64_bit = (instruction >> 31) != 0;
			if ((extended && shift > 4) ||
			    (!extended && (shift_type == 3 ||
				(!is_64_bit && shift >= 32)))) {
				emit(words, uml_arm64_gadget_fatal);
				emit(words, address);
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					false, address, instruction};
			}
			emit(words, uml_arm64_gadget_add_sub_register);
			emit(words, instruction);
		} else if ((instruction & 0x1fe0'fc00U) == 0x1a00'0000U) {
			emit(words, uml_arm64_gadget_add_sub_carry);
			emit(words, instruction);
		} else if ((instruction & 0x1f00'0000U) == 0x1b00'0000U) {
			const bool is_64_bit = (instruction >> 31) != 0;
			const auto operation = (instruction >> 21) & 7U;
			const bool valid = ((instruction >> 29) & 3U) == 0 &&
				(operation == 0 || (is_64_bit && (operation == 1 ||
				 operation == 2 || operation == 5 || operation == 6))) &&
				((instruction & 0x0000'8000U) == 0 ||
				 operation == 0 || operation == 1 || operation == 5);
			if (valid) {
				emit(words, uml_arm64_gadget_multiply_add);
				emit(words, instruction);
				emit(words, address);
			} else {
				emit_interpreter_or_fatal(words, instruction, address);
			}
		} else if ((instruction & 0x7fe0'f000U) == 0x1ac0'2000U) {
			emit(words, uml_arm64_gadget_variable_shift);
			emit(words, instruction);
		} else if ((instruction & 0x7fe0'f800U) == 0x1ac0'0800U) {
			emit(words, uml_arm64_gadget_divide);
			emit(words, instruction);
		} else if ((instruction & 0x7fe0'e000U) == 0x1ac0'4000U) {
			const unsigned size = (instruction >> 10) & 3U;
			if (((instruction >> 31) != 0) == (size == 3U)) {
				emit(words, uml_arm64_gadget_crc32);
				emit(words, instruction);
			} else {
				emit_interpreter_or_fatal(words, instruction, address);
			}
		} else if ((instruction & 0xffe0'0000U) == 0x93c0'0000U ||
			   (instruction & 0xffe0'0000U) == 0x1380'0000U) {
			const bool is_64_bit = (instruction >> 31) != 0;
			const unsigned shift = (instruction >> 10) & 0x3fU;
			if (((instruction >> 22) & 1U) == is_64_bit &&
			    (is_64_bit || shift < 32U)) {
				emit(words, uml_arm64_gadget_extract_register);
				emit(words, instruction);
			} else {
				emit_interpreter_or_fatal(words, instruction, address);
			}
		} else if ((instruction & 0x1f00'0000U) == 0x0a00'0000U) {
			const bool is_64_bit = (instruction >> 31) != 0;
			const unsigned shift = (instruction >> 10) & 0x3fU;
			if (!is_64_bit && shift >= 32) {
				emit(words, uml_arm64_gadget_fatal);
				emit(words, address);
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					false, address, instruction};
			}
			emit(words, uml_arm64_gadget_logical_shifted);
			emit(words, instruction);
		} else if ((instruction & 0x3fe0'0000U) == 0x1a80'0000U) {
			emit(words, uml_arm64_gadget_conditional_select);
			emit(words, instruction);
		} else if ((instruction & 0x3fe0'0410U) == 0x3a40'0000U) {
			emit(words, conditional_compare_gadget(
				(instruction >> 12) & 15U));
			emit(words, instruction);
		} else if ((instruction & 0x1f80'0000U) == 0x1200'0000U) {
			const bool is_64_bit = (instruction >> 31) != 0;
			const auto immediate = detail::decode_logical_immediate(
				is_64_bit, (instruction >> 22) & 1U,
				(instruction >> 16) & 0x3fU,
				(instruction >> 10) & 0x3fU);
			if (!immediate) {
				emit(words, uml_arm64_gadget_fatal);
				emit(words, address);
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					false, address, instruction};
			}
			emit(words, uml_arm64_gadget_logical_immediate);
			emit(words, instruction);
			emit(words, *immediate);
		} else if ((instruction & 0x1f80'0000U) == 0x1300'0000U) {
			const bool is_64_bit = (instruction >> 31) != 0;
			const auto n = (instruction >> 22) & 1U;
			const auto rotate = (instruction >> 16) & 0x3fU;
			const auto imms = (instruction >> 10) & 0x3fU;
			const auto operation = (instruction >> 29) & 3U;
			const auto masks = detail::decode_bit_masks(
				is_64_bit, n, rotate, imms, false);
			if (operation == 3 || n != static_cast<unsigned>(is_64_bit) ||
			    (!is_64_bit && ((rotate | imms) & 0x20U) != 0) ||
			    !masks) {
				emit(words, uml_arm64_gadget_fatal);
				emit(words, address);
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					false, address, instruction};
			}
			emit(words, uml_arm64_gadget_bitfield);
			emit(words, instruction);
			emit(words, masks->write);
			emit(words, masks->test);
		} else if ((instruction & 0x7fff'8000U) == 0x5ac0'0000U) {
			const bool is_64_bit = (instruction >> 31) != 0;
			const auto operation = (instruction >> 10) & 0x1fU;
			if (operation > 5 || (!is_64_bit && operation == 3)) {
				emit(words, uml_arm64_gadget_fatal);
				emit(words, address);
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					false, address, instruction};
			}
			emit(words, uml_arm64_gadget_bit_reverse_count);
			emit(words, instruction);
		} else if ((instruction & 0xffff'fc00U) == 0x1e18'dc00U) {
			/* FCVTZS Wd, Sn, #9 occurs in the arm64 LTP build. Keep the
			 * fixed-point immediate native without enabling the interpreter. */
			emit(words, uml_arm64_gadget_floating_to_integer_fixed_s32_9);
			emit(words, instruction);
		} else if ((instruction & 0x7f20'fc00U) == 0x1e20'0000U &&
			   ((instruction >> 16) & 7U) <= 5U) {
			emit(words, uml_arm64_gadget_floating_integer_convert);
			emit(words, instruction);
		} else if ((instruction & 0xff20'fc00U) == 0x1e20'2000U &&
			   ((instruction >> 22) & 3U) <= 1) {
			emit(words, uml_arm64_gadget_floating_compare);
			emit(words, instruction);
		} else if ((instruction & 0xff20'0c00U) == 0x1e20'0400U &&
			   ((instruction >> 22) & 3U) <= 1U) {
			emit(words, floating_conditional_compare_gadget(
				(instruction >> 12) & 15U));
			emit(words, instruction);
		} else if ((instruction & 0xffff'fc00U) == 0x1e22'c000U ||
			   (instruction & 0xffff'fc00U) == 0x1e62'4000U) {
			emit(words, uml_arm64_gadget_floating_convert_precision);
			emit(words, instruction);
		} else if ((instruction & 0xff20'1fe0U) == 0x1e20'1000U) {
			const auto immediate = static_cast<std::uint8_t>(
				((instruction >> 13) & 0xffU) ^ 0x40U);
			const bool negative = (immediate & 0x80U) != 0;
			const auto exponent = (immediate >> 4) & 7U;
			const auto fraction = immediate & 15U;
			double value = static_cast<double>(16U + fraction) /
				static_cast<double>(1U << (7U - exponent));
			if (negative)
				value = -value;
			const bool is_double = (instruction & 0x0040'0000U) != 0;
			const auto bits = is_double ? std::bit_cast<std::uint64_t>(value) :
				std::bit_cast<std::uint32_t>(static_cast<float>(value));
			emit(words, uml_arm64_gadget_floating_move_immediate);
			emit(words, instruction & 31U);
			emit(words, bits);
		} else if ((instruction & 0xff20'7c00U) == 0x1e20'4000U &&
			   ((instruction >> 22) & 3U) <= 1U &&
			   ((((instruction >> 15) & 0x3fU) >= 8U &&
			     ((instruction >> 15) & 0x3fU) <= 12U) ||
			    ((instruction >> 15) & 0x3fU) == 14U ||
			    ((instruction >> 15) & 0x3fU) == 15U)) {
			emit(words, uml_arm64_gadget_floating_round_integral);
			emit(words, instruction);
		} else if ((instruction & 0xff20'7c00U) == 0x1e20'4000U ||
			   (instruction & 0xff20'0c00U) == 0x1e20'0800U) {
			const auto type = (instruction >> 22) & 3U;
			const bool one_source =
				(instruction & 0xff20'7c00U) == 0x1e20'4000U;
			const auto operation = one_source ?
				(instruction >> 15) & 0x3fU :
				(instruction >> 12) & 0xfU;
			const bool native = type <= 1 && (one_source ? operation <= 3 :
				operation <= 3 || operation == 8);
			if (native) {
				emit(words, floating_arithmetic_gadget(
					type == 1, one_source, operation));
				emit(words, (instruction & 31U) |
					(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
					(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16));
			} else {
				emit_interpreter_or_fatal(words, instruction, address);
			}
		} else if ((instruction & 0xff00'0000U) == 0x1f00'0000U &&
			   ((instruction >> 22) & 3U) <= 1) {
			emit(words, floating_multiply_add_gadget(
				((instruction >> 22) & 3U) == 1,
				(instruction & 0x0020'0000U) != 0,
				(instruction & 0x0000'8000U) != 0));
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
				(static_cast<std::uintptr_t>((instruction >> 10) & 31U) << 16) |
				(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 24));
		} else if ((instruction & 0xff20'fc00U) == 0x7e20'd400U) {
			emit(words, uml_arm64_gadget_floating_absolute_difference);
			emit(words, instruction);
		} else if (((instruction & 0xbf20'fc00U) == 0x0e00'1800U ||
			    (instruction & 0xbf20'fc00U) == 0x0e00'2800U ||
			    (instruction & 0xbf20'fc00U) == 0x0e00'3800U ||
			    (instruction & 0xbf20'fc00U) == 0x0e00'5800U ||
			    (instruction & 0xbf20'fc00U) == 0x0e00'6800U ||
			    (instruction & 0xbf20'fc00U) == 0x0e00'7800U) &&
			   (((instruction >> 22) & 3U) < 3U ||
			    (instruction & 0x4000'0000U) != 0)) {
			emit(words, uml_arm64_gadget_simd_permute);
			emit(words, instruction);
		} else if ((instruction & 0xbfe0'8c00U) == 0x0e00'0000U) {
			emit(words, uml_arm64_gadget_simd_table_lookup);
			emit(words, instruction);
		} else if ((instruction & 0x8f80'fc00U) == 0x0f00'a400U) {
			const auto encoded = (instruction >> 16) & 0x7fU;
			const auto source_bits = std::bit_floor(encoded);
			if (source_bits >= 8U && source_bits <= 32U &&
			    encoded - source_bits < source_bits) {
				emit(words, uml_arm64_gadget_simd_shift_left_long);
				emit(words, instruction);
			} else {
				emit_interpreter_or_fatal(words, instruction, address);
			}
		} else if ((instruction & 0x9f20'fc00U) == 0x0e20'4400U ||
			   (instruction & 0x9f20'fc00U) == 0x0e20'5400U) {
			emit(words, uml_arm64_gadget_simd_shift_register);
			emit(words, instruction);
		} else if ((instruction & 0x9f20'0c00U) == 0x0e20'0000U &&
			   (((instruction >> 12) & 15U) <= 3U ||
			    ((instruction >> 12) & 15U) == 12U) &&
			   ((instruction >> 22) & 3U) < 3U) {
			emit(words, uml_arm64_gadget_simd_arithmetic_wide);
			emit(words, instruction);
		} else if ((instruction & 0xafbb'fc00U) == 0x0e21'd800U ||
			   (instruction & 0xafbb'fc00U) == 0x2e21'd800U ||
			   (instruction & 0xafbb'fc00U) == 0x0ea1'b800U ||
			   (instruction & 0xafbb'fc00U) == 0x2ea1'b800U) {
			emit(words, uml_arm64_gadget_simd_integer_float_convert);
			emit(words, instruction);
		} else if ((instruction & 0xbf3f'fc00U) == 0x0e20'f800U ||
			   (instruction & 0xbf3f'fc00U) == 0x2e20'f800U) {
			if ((instruction & 0x0040'0000U) == 0 ||
			    (instruction & 0x4000'0000U) != 0) {
				emit(words, uml_arm64_gadget_simd_floating_unary);
				emit(words, instruction);
			} else {
				emit_interpreter_or_fatal(words, instruction, address);
			}
		} else if ((instruction & 0xbf20'fc00U) == 0x0e00'0400U ||
			   (instruction & 0xff20'fc00U) == 0x5e00'0400U) {
			emit(words, uml_arm64_gadget_simd_duplicate_element);
			emit(words, instruction);
		} else if (((instruction & 0x9f00'fc00U) == 0x0f00'0400U ||
			    (instruction & 0xdf00'fc00U) == 0x5f00'0400U) &&
			   ((instruction >> 16) & 0x7fU) >= 8U) {
			emit(words, uml_arm64_gadget_simd_shift_right);
			emit(words, instruction);
		} else if ((instruction & 0x9f00'fc00U) == 0x0f00'5400U) {
			const auto encoded = (instruction >> 16) & 0x7fU;
			const auto lane_bits = std::bit_floor(encoded);
			if (lane_bits >= 8U && lane_bits <= 64U &&
			    encoded - lane_bits < lane_bits &&
			    (lane_bits != 64U || (instruction & 0x4000'0000U) != 0)) {
				emit(words, uml_arm64_gadget_simd_shift_left);
				emit(words, instruction);
			} else {
				emit_interpreter_or_fatal(words, instruction, address);
			}
		} else if ((instruction & 0x9f20'fc00U) == 0x0e20'1c00U) {
			const auto operation = ((instruction >> 22) & 3U) |
				((instruction >> 27) & 4U);
			emit(words, simd_bitwise_gadget(operation));
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
				(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16) |
				(static_cast<std::uintptr_t>((instruction >> 30) & 1U) << 24));
		} else if ((instruction & 0xbfe0'8400U) == 0x2e00'0000U) {
			const bool q = (instruction & 0x4000'0000U) != 0;
			const unsigned offset = (instruction >> 11) & 15U;
			if (offset < (q ? 16U : 8U)) {
				emit(words, simd_extract_gadget(q, offset));
				emit(words, (instruction & 31U) |
					(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
					(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16));
			} else {
				emit_interpreter_or_fatal(words, instruction, address);
			}
		} else if ((instruction & 0x9f3f'fc00U) == 0x0e31'b800U) {
			emit(words, uml_arm64_gadget_simd_addv);
			emit(words, instruction);
		} else if ((instruction & 0xbf20'fc00U) == 0x0e20'0800U &&
			   ((instruction >> 22) & 3U) < 3U) {
			emit(words, uml_arm64_gadget_simd_reverse_64);
			emit(words, instruction);
		} else if ((instruction & 0xbf20'fc00U) == 0x2e20'0800U &&
			   ((instruction >> 22) & 3U) < 2U) {
			emit(words, uml_arm64_gadget_simd_reverse_32);
			emit(words, instruction);
		} else if ((instruction & 0xbf20'fc00U) == 0x0e20'1800U &&
			   ((instruction >> 22) & 3U) == 0U) {
			emit(words, uml_arm64_gadget_simd_reverse_16);
			emit(words, instruction);
		} else if ((instruction & 0xbfff'fc00U) == 0x2e20'5800U) {
			emit(words, uml_arm64_gadget_simd_not);
			emit(words, instruction);
		} else if ((instruction & 0xbf20'fc00U) == 0x0e20'5800U) {
			emit(words, uml_arm64_gadget_simd_count_bits);
			emit(words, instruction);
		} else if ((instruction & 0x9f20'fc00U) == 0x0e20'6400U ||
			   (instruction & 0x9f20'fc00U) == 0x0e20'6c00U ||
			   (instruction & 0x9f20'fc00U) == 0x0e20'a400U ||
			   (instruction & 0x9f20'fc00U) == 0x0e20'ac00U) {
			emit(words, uml_arm64_gadget_simd_min_max);
			emit(words, instruction);
		} else if ((instruction & 0x9f20'fc00U) == 0x0e20'8400U ||
			   (instruction & 0xbf20'fc00U) == 0x0e20'bc00U ||
			   (instruction & 0xdf20'fc00U) == 0x5e20'8400U) {
			emit(words, uml_arm64_gadget_simd_add_sub_pairwise);
			emit(words, instruction);
		} else if ((instruction & 0xbf20'fc00U) == 0x0e20'9c00U &&
			   ((instruction >> 22) & 3U) < 3U) {
			emit(words, uml_arm64_gadget_simd_multiply);
			emit(words, instruction);
		} else if ((instruction & 0x9f20'fc00U) == 0x0e20'3400U ||
			   (instruction & 0x9f20'fc00U) == 0x0e20'3c00U ||
			   (instruction & 0x9f20'fc00U) == 0x0e20'8c00U) {
			emit(words, uml_arm64_gadget_simd_compare_register);
			emit(words, instruction);
		} else if ((instruction & 0xbf00'fc00U) == 0x0f00'8400U ||
			   (instruction & 0xbf00'fc00U) == 0x0f00'8c00U) {
			emit(words, uml_arm64_gadget_simd_shift_right_narrow);
			emit(words, instruction);
		} else if ((instruction & 0xbf3f'fc00U) == 0x0e21'2800U &&
			   ((instruction >> 22) & 3U) < 3U) {
			emit(words, uml_arm64_gadget_simd_narrow_move);
			emit(words, instruction);
		} else if ((instruction & 0x9f20'fc00U) == 0x0e20'4000U ||
			   (instruction & 0x9f20'fc00U) == 0x0e20'6000U) {
			emit(words, uml_arm64_gadget_simd_arithmetic_narrow);
			emit(words, instruction);
		} else if (((instruction & 0x9f3f'fc00U) == 0x0e20'8800U ||
			    (instruction & 0x9f3f'fc00U) == 0x0e20'9800U ||
			    (instruction & 0x9f3f'fc00U) == 0x0e20'a800U ||
			    (instruction & 0x9f3f'fc00U) == 0x1e20'8800U ||
			    (instruction & 0x9f3f'fc00U) == 0x1e20'9800U)) {
			emit(words, uml_arm64_gadget_simd_compare_zero);
			emit(words, instruction);
		} else if ((instruction & 0x9ff8'0c00U) == 0x0f00'0400U) {
			const auto decoded = decode_simd_immediate(instruction);
			if (decoded) {
				emit(words, uml_arm64_gadget_simd_modified_immediate);
				emit(words, (instruction & 31U) |
					(static_cast<std::uintptr_t>((instruction >> 30) & 1U) << 8) |
					(static_cast<std::uintptr_t>(decoded->action) << 16));
				emit(words, decoded->pattern);
			} else {
				emit_interpreter_or_fatal(words, instruction, address);
			}
		} else if ((instruction & 0xbfe0'fc00U) == 0x0e00'3c00U ||
			   (instruction & 0xbfe0'fc00U) == 0x0e00'2c00U) {
			emit(words, uml_arm64_gadget_simd_vector_element_to_gpr);
			emit(words, instruction);
		} else if ((instruction & 0xffe0'fc00U) == 0x4e00'1c00U) {
			emit(words, uml_arm64_gadget_simd_gpr_to_vector_element);
			emit(words, instruction);
		} else if ((instruction & 0xffe0'8400U) == 0x6e00'0400U) {
			const auto destination_immediate = (instruction >> 16) & 31U;
			const auto element_shift = destination_immediate == 0 ? 4U :
				static_cast<unsigned>(std::countr_zero(destination_immediate));
			const auto source_immediate = (instruction >> 11) & 15U;
			if (element_shift <= 3U &&
			    (source_immediate & ((1U << element_shift) - 1U)) == 0U) {
				emit(words, uml_arm64_gadget_simd_insert_element);
				emit(words, instruction);
			} else {
				emit_interpreter_or_fatal(words, instruction, address);
			}
		} else if ((instruction & 0xffff'fc00U) == 0x9e66'0000U ||
			   (instruction & 0xffff'fc00U) == 0x9e67'0000U ||
			   (instruction & 0xffff'fc00U) == 0x1e26'0000U ||
			   (instruction & 0xffff'fc00U) == 0x1e27'0000U ||
			   (instruction & 0xffff'fc00U) == 0x9eae'0000U ||
			   (instruction & 0xffff'fc00U) == 0x9eaf'0000U) {
			const auto opcode = instruction & 0xffff'fc00U;
			Gadget move = uml_arm64_gadget_gpr_to_vector_64_high;
			if (opcode == 0x9e66'0000U)
				move = uml_arm64_gadget_vector_to_gpr_64_low;
			else if (opcode == 0x9e67'0000U)
				move = uml_arm64_gadget_gpr_to_vector_64_low;
			else if (opcode == 0x1e26'0000U)
				move = uml_arm64_gadget_vector_to_gpr_32_low;
			else if (opcode == 0x1e27'0000U)
				move = uml_arm64_gadget_gpr_to_vector_32_low;
			else if (opcode == 0x9eae'0000U)
				move = uml_arm64_gadget_vector_to_gpr_64_high;
			emit(words, move);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8));
		} else if ((instruction & 0xff20'0c00U) == 0x1e20'0c00U &&
			   ((instruction >> 22) & 3U) <= 1) {
			emit(words, floating_select_gadget((instruction >> 12) & 15U));
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
				(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16) |
				(static_cast<std::uintptr_t>((instruction >> 22) & 1U) << 24));
		} else if ((instruction & 0xffff'ffe0U) == 0xd50b'7b20U ||
			   (instruction & 0xffff'ffe0U) == 0xd50b'7520U) {
			/* The guest cache operations have no extra state to maintain:
			 * executable-page invalidation is tracked by the VM/TLB layer. */
			emit(words, uml_arm64_gadget_nop);
		} else if ((instruction & 0xffff'f01fU) == 0xd503'201fU) {
			emit(words, uml_arm64_gadget_nop);
		} else if ((instruction & 0xbfe0'fc00U) == 0x0e00'0c00U) {
			const auto immediate = (instruction >> 16) & 31U;
			const auto element_size = immediate & 1U ? 0U :
				immediate & 2U ? 1U : immediate & 4U ? 2U :
				immediate & 8U ? 3U : 4U;
			if (element_size == 4U) {
				emit(words, uml_arm64_gadget_fatal);
				emit(words, address);
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					false, address, instruction};
			}
			emit(words, uml_arm64_gadget_dup_gpr_to_vector);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
				(static_cast<std::uintptr_t>(element_size) << 16) |
				(static_cast<std::uintptr_t>((instruction >> 30) & 1U) << 24));
		} else if ((instruction & 0xbf20'0000U) == 0x0c00'0000U) {
			const unsigned opcode = (instruction >> 12) & 15U;
			if (opcode == 7 || opcode == 10 || opcode == 6 || opcode == 2) {
				emit(words, uml_arm64_gadget_load_store_simd_structure);
				emit(words, instruction);
				emit(words, address);
			} else {
				emit_interpreter_or_fatal(words, instruction, address);
			}
		} else if ((instruction & 0xbf80'0000U) == 0x0d00'0000U ||
			   (instruction & 0xbf80'0000U) == 0x0d80'0000U) {
			const unsigned opcode = (instruction >> 13) & 7U;
			const bool replicate = (opcode == 6 || opcode == 7) &&
				(instruction & 0x0000'1000U) == 0 &&
				(instruction & 0x0040'0000U) != 0;
			if (replicate) {
				emit(words, uml_arm64_gadget_load_simd_replicate);
				emit(words, instruction);
				emit(words, address);
			} else {
				const unsigned base_opcode = opcode & ~1U;
				const unsigned size = (instruction >> 10) & 3U;
				const bool valid_lane = base_opcode == 0 || base_opcode == 2 ||
					(base_opcode == 4 && size <= 1);
				if (valid_lane) {
					emit(words, uml_arm64_gadget_load_store_simd_lane);
					emit(words, instruction);
					emit(words, address);
				} else {
					emit_interpreter_or_fatal(words, instruction, address);
				}
			}
		} else if ((instruction & 0xffc0'0000U) == 0xb940'0000U ||
			   (instruction & 0xffc0'0000U) == 0xf940'0000U ||
			   (instruction & 0xffc0'0000U) == 0xb900'0000U ||
			   (instruction & 0xffc0'0000U) == 0xf900'0000U) {
			const auto opcode = instruction & 0xffc0'0000U;
			const auto gadget = opcode == 0xb940'0000U ?
				uml_arm64_gadget_load32_unsigned :
				opcode == 0xf940'0000U ?
				uml_arm64_gadget_load64_unsigned :
				opcode == 0xb900'0000U ?
				uml_arm64_gadget_store32_unsigned :
				uml_arm64_gadget_store64_unsigned;
			emit(words, gadget);
			const auto reg = instruction & 31U;
			const auto base = (instruction >> 5) & 31U;
			const auto scale = opcode == 0xb940'0000U ||
				opcode == 0xb900'0000U ? 2U : 3U;
			const auto offset = ((instruction >> 10) & 0xfffU) << scale;
			emit(words, reg | (static_cast<std::uintptr_t>(base) << 8) |
				(static_cast<std::uintptr_t>(offset) << 16));
			/* The fast gadget branches to the generic static decoder when
			 * the runtime address crosses a guest page. Keep the decoder's
			 * instruction/PC pair adjacent in the threaded stream.
			 */
			emit(words, instruction);
			emit(words, address);
		} else if ((instruction & 0x3f00'0000U) == 0x0800'0000U &&
			   ((instruction >> 10) & 31U) == 31U &&
			   (instruction & 0x0080'0000U) != 0) {
			emit(words, uml_arm64_gadget_load_store_ordered);
			emit(words, instruction);
			emit(words, address);
		} else if ((instruction & 0x3f00'0000U) == 0x0800'0000U &&
			   ((instruction >> 10) & 31U) == 31U &&
			   (instruction & 0x0080'0000U) == 0) {
			emit(words, uml_arm64_gadget_load_store_exclusive);
			emit(words, instruction);
			emit(words, address);
		} else if ((instruction & 0x3b00'0000U) == 0x3900'0000U &&
			   (instruction & 0x0400'0000U) == 0) {
			const auto size_code = instruction >> 30;
			const auto operation = (instruction >> 22) & 3U;
			if (size_code == 3 && operation == 2) {
				/* PRFM is architecturally only a hint. */
				emit(words, uml_arm64_gadget_nop);
			} else if ((size_code >= 2 && operation == 3) ||
				   (size_code == 3 && operation > 1)) {
				emit_interpreter_or_fatal(words, instruction, address);
			} else {
				emit(words, uml_arm64_gadget_load_store_integer_unsigned);
				emit(words, instruction);
				emit(words, address);
			}
		} else if ((instruction & 0x3b00'0000U) == 0x3900'0000U) {
			const auto size_code = instruction >> 30;
			const auto operation = (instruction >> 22) & 3U;
			if (operation >= 2 && size_code != 0) {
				emit_interpreter_or_fatal(words, instruction, address);
			} else {
				emit(words, uml_arm64_gadget_load_store_vector_unsigned);
				emit(words, instruction);
				emit(words, address);
			}
		} else if ((instruction & 0x3b00'0000U) == 0x3800'0000U &&
			   (instruction & 0x0400'0000U) == 0) {
			const auto size_code = instruction >> 30;
			const auto operation = (instruction >> 22) & 3U;
			const bool valid_transfer =
				(size_code != 3 || operation <= 1) &&
				(size_code != 2 || operation != 3);
			const bool register_offset =
				(instruction & 0x0020'0000U) != 0;
			const auto option = (instruction >> 13) & 7U;
			const bool valid_offset = !register_offset || option == 2 ||
				option == 3 || option == 6 || option == 7;
			if (valid_transfer && valid_offset) {
				emit(words, uml_arm64_gadget_load_store_integer_other);
				emit(words, instruction);
				emit(words, address);
			} else {
				emit_interpreter_or_fatal(words, instruction, address);
			}
		} else if ((instruction & 0x3b00'0000U) == 0x3800'0000U) {
			const auto size_code = instruction >> 30;
			const auto operation = (instruction >> 22) & 3U;
			const bool valid_transfer = operation < 2 || size_code == 0;
			const bool register_offset =
				(instruction & 0x0020'0000U) != 0;
			const auto option = (instruction >> 13) & 7U;
			const bool valid_offset = !register_offset || option == 2 ||
				option == 3 || option == 6 || option == 7;
			if (valid_transfer && valid_offset) {
				emit(words, uml_arm64_gadget_load_store_vector_other);
				emit(words, instruction);
				emit(words, address);
			} else {
				emit_interpreter_or_fatal(words, instruction, address);
			}
		} else if ((instruction & 0x3a00'0000U) == 0x2800'0000U &&
			   (instruction & 0x0400'0000U) == 0 &&
			   ((instruction >> 30) == 0 || (instruction >> 30) == 2 ||
			    ((instruction >> 30) == 1 &&
			     (instruction & 0x0040'0000U) != 0))) {
			emit(words, uml_arm64_gadget_load_store_pair);
			emit(words, instruction);
			emit(words, address);
		} else if ((instruction & 0x3a00'0000U) == 0x2800'0000U &&
			   (instruction & 0x0400'0000U) != 0 &&
			   (instruction >> 30) != 3) {
			emit(words, uml_arm64_gadget_load_store_vector_pair);
			emit(words, instruction);
			emit(words, address);
		} else if ((instruction & 0xffe0'001fU) == 0xd400'0001U) {
			emit(words, uml_arm64_gadget_svc);
			emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else {
			emit_interpreter_or_fatal(words, instruction, address);
		}
		++compiled;
	}
	emit(words, uml_arm64_gadget_interrupt);
	emit(words, guest_address + compiled * sizeof(std::uint32_t));
	return {Program{guest_address, compiled, std::move(words),
		program_accesses_fp_state}, true,
		guest_address + compiled * sizeof(std::uint32_t), 0};
}

Event execute(const Program &program, CpuState &cpu, Tlb &tlb) noexcept
{
	if (program.words().empty())
		return Event::fatal;
	if (program.accesses_fp_state())
		cpu.access_fp_state();
	return static_cast<Event>(
		uml_arm64_gadget_enter(program.words().data(), &cpu, &tlb));
}

} // namespace uml::backend::arm64::gadgets
