// SPDX-License-Identifier: GPL-2.0-or-later
#include "program.hpp"
#include "software_helpers.hpp"

#include "cpu/arm64/layout/assembly_layout.hpp"

#include "../detail/atomic_floating.hpp"
#include "../detail/floating_compare.hpp"
#include "../detail/bfloat_conversion.hpp"
#include "../detail/bfloat_dot.hpp"
#include "../detail/bfloat_matrix.hpp"
#include "../detail/bfloat_multiply.hpp"
#include "../detail/half_fused_long.hpp"
#include "../detail/half_dot.hpp"
#include "../detail/half_matrix.hpp"
#include "../detail/floating_complex.hpp"
#include "../detail/floating_arithmetic.hpp"
#include "../detail/floating_conversion.hpp"
#include "../detail/floating_fused.hpp"
#include "../detail/floating_horizontal.hpp"
#include "../detail/floating_indexed.hpp"
#include "../detail/floating_integer_conversion.hpp"
#include "../detail/floating_reciprocal.hpp"
#include "../detail/floating_reciprocal_software.hpp"
#include "../detail/floating_round.hpp"
#include "../detail/floating_scale.hpp"
#include "../detail/fp_state_access.hpp"
#include "../detail/fp8.hpp"
#include "../detail/gcs.hpp"
#include "../detail/instruction_helpers.hpp"
#include "../detail/integer_multiply.hpp"
#include "../detail/pointer_auth.hpp"
#include "cpu/host_capabilities.hpp"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace uml::cpu::arm64::gadgets {
namespace {

struct EngineResult final {
	std::int32_t event;
	std::uint32_t reserved;
	std::size_t executed;
};

extern "C" {
EngineResult uml_arm64_gadget_enter(const std::uintptr_t *program,
				    CpuState *cpu, ::uml::memory::Tlb *tlb,
				    std::size_t initial_instructions);
void uml_arm64_gadget_move_wide_constant();
void uml_arm64_gadget_move_wide_keep_w0();
void uml_arm64_gadget_move_wide_keep_w1();
void uml_arm64_gadget_move_wide_keep_x0();
void uml_arm64_gadget_move_wide_keep_x1();
void uml_arm64_gadget_move_wide_keep_x2();
void uml_arm64_gadget_move_wide_keep_x3();
#define UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(prefix) \
	void uml_arm64_gadget_add_sub_immediate_##prefix##_rr(); \
	void uml_arm64_gadget_add_sub_immediate_##prefix##_rs(); \
	void uml_arm64_gadget_add_sub_immediate_##prefix##_sr(); \
	void uml_arm64_gadget_add_sub_immediate_##prefix##_ss();
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(w_add_n)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(w_add_s)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(w_sub_n)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(w_sub_s)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(x_add_n)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(x_add_s)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(x_sub_n)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(x_sub_s)
#undef UML_DECLARE_ADD_SUB_IMMEDIATE_MODES
#define UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix) \
	void uml_arm64_gadget_add_sub_register_##prefix##_r(); \
	void uml_arm64_gadget_add_sub_register_##prefix##_d();
#define UML_DECLARE_ADD_SUB_SHIFTED_SOURCES(prefix) \
	UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix##_rr) \
	UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix##_rz) \
	UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix##_zr) \
	UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix##_zz)
#define UML_DECLARE_ADD_SUB_SHIFTED_TRANSFORMS(prefix) \
	UML_DECLARE_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_n) \
	UML_DECLARE_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_l) \
	UML_DECLARE_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_r) \
	UML_DECLARE_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_a)
#define UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix) \
	UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix##_r) \
	UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix##_z)
#define UML_DECLARE_ADD_SUB_EXTENDED_TRANSFORMS(prefix) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e0n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e0s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e1n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e1s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e2n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e2s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e3n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e3s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e4n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e4s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e5n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e5s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e6n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e6s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e7n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e7s)
#define UML_DECLARE_ADD_SUB_REGISTER_ALL(prefix) \
	UML_DECLARE_ADD_SUB_SHIFTED_TRANSFORMS(prefix) \
	UML_DECLARE_ADD_SUB_EXTENDED_TRANSFORMS(prefix)
UML_DECLARE_ADD_SUB_REGISTER_ALL(w_add_n)
UML_DECLARE_ADD_SUB_REGISTER_ALL(w_add_s)
UML_DECLARE_ADD_SUB_REGISTER_ALL(w_sub_n)
UML_DECLARE_ADD_SUB_REGISTER_ALL(w_sub_s)
UML_DECLARE_ADD_SUB_REGISTER_ALL(x_add_n)
UML_DECLARE_ADD_SUB_REGISTER_ALL(x_add_s)
UML_DECLARE_ADD_SUB_REGISTER_ALL(x_sub_n)
UML_DECLARE_ADD_SUB_REGISTER_ALL(x_sub_s)
#undef UML_DECLARE_ADD_SUB_REGISTER_ALL
#undef UML_DECLARE_ADD_SUB_EXTENDED_TRANSFORMS
#undef UML_DECLARE_ADD_SUB_EXTENDED_SOURCES
#undef UML_DECLARE_ADD_SUB_SHIFTED_TRANSFORMS
#undef UML_DECLARE_ADD_SUB_SHIFTED_SOURCES
#undef UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS
#define UML_DECLARE_ADD_SUB_CARRY_RESULT(prefix) \
	void uml_arm64_gadget_add_sub_carry_##prefix##_r();
#define UML_DECLARE_ADD_SUB_CARRY_FLAGS(prefix) \
	UML_DECLARE_ADD_SUB_CARRY_RESULT(prefix) \
	void uml_arm64_gadget_add_sub_carry_##prefix##_z();
#define UML_DECLARE_ADD_SUB_CARRY_SOURCES(prefix, result) \
	result(prefix##_rr) \
	result(prefix##_eq) \
	result(prefix##_rz) \
	result(prefix##_zr) \
	result(prefix##_zz)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(w_adc_n,
	UML_DECLARE_ADD_SUB_CARRY_RESULT)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(w_adc_s,
	UML_DECLARE_ADD_SUB_CARRY_FLAGS)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(w_sbc_n,
	UML_DECLARE_ADD_SUB_CARRY_RESULT)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(w_sbc_s,
	UML_DECLARE_ADD_SUB_CARRY_FLAGS)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(x_adc_n,
	UML_DECLARE_ADD_SUB_CARRY_RESULT)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(x_adc_s,
	UML_DECLARE_ADD_SUB_CARRY_FLAGS)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(x_sbc_n,
	UML_DECLARE_ADD_SUB_CARRY_RESULT)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(x_sbc_s,
	UML_DECLARE_ADD_SUB_CARRY_FLAGS)
#undef UML_DECLARE_ADD_SUB_CARRY_SOURCES
#undef UML_DECLARE_ADD_SUB_CARRY_FLAGS
#undef UML_DECLARE_ADD_SUB_CARRY_RESULT
#define UML_DECLARE_MTE_POINTER_VARIANTS(operation) \
	void uml_arm64_gadget_mte_##operation##_rrr(); \
	void uml_arm64_gadget_mte_##operation##_rrz(); \
	void uml_arm64_gadget_mte_##operation##_rsr(); \
	void uml_arm64_gadget_mte_##operation##_rsz(); \
	void uml_arm64_gadget_mte_##operation##_srr(); \
	void uml_arm64_gadget_mte_##operation##_srz(); \
	void uml_arm64_gadget_mte_##operation##_ssr(); \
	void uml_arm64_gadget_mte_##operation##_ssz();
UML_DECLARE_MTE_POINTER_VARIANTS(gmi)
UML_DECLARE_MTE_POINTER_VARIANTS(subp)
UML_DECLARE_MTE_POINTER_VARIANTS(subps)
#undef UML_DECLARE_MTE_POINTER_VARIANTS
#define UML_DECLARE_MTE_TAG_VARIANTS(operation) \
	void uml_arm64_gadget_mte_##operation##_rr(); \
	void uml_arm64_gadget_mte_##operation##_rs(); \
	void uml_arm64_gadget_mte_##operation##_sr(); \
	void uml_arm64_gadget_mte_##operation##_ss();
UML_DECLARE_MTE_TAG_VARIANTS(addg)
UML_DECLARE_MTE_TAG_VARIANTS(subg)
#undef UML_DECLARE_MTE_TAG_VARIANTS
#define UML_DECLARE_MTE_IRG_VARIANTS() \
	void uml_arm64_gadget_mte_irg_rrr(); \
	void uml_arm64_gadget_mte_irg_rrs(); \
	void uml_arm64_gadget_mte_irg_rzr(); \
	void uml_arm64_gadget_mte_irg_rzs(); \
	void uml_arm64_gadget_mte_irg_srr(); \
	void uml_arm64_gadget_mte_irg_srs(); \
	void uml_arm64_gadget_mte_irg_szr(); \
	void uml_arm64_gadget_mte_irg_szs();
UML_DECLARE_MTE_IRG_VARIANTS()
#undef UML_DECLARE_MTE_IRG_VARIANTS
#define UML_DECLARE_CPA_MODES(operation, shift) \
	void uml_arm64_gadget_cpa_##operation##_##shift##_rrr(); \
	void uml_arm64_gadget_cpa_##operation##_##shift##_rrs(); \
	void uml_arm64_gadget_cpa_##operation##_##shift##_rzr(); \
	void uml_arm64_gadget_cpa_##operation##_##shift##_rzs(); \
	void uml_arm64_gadget_cpa_##operation##_##shift##_srr(); \
	void uml_arm64_gadget_cpa_##operation##_##shift##_srs(); \
	void uml_arm64_gadget_cpa_##operation##_##shift##_szr(); \
	void uml_arm64_gadget_cpa_##operation##_##shift##_szs();
#define UML_DECLARE_CPA_SHIFTS(operation) \
	UML_DECLARE_CPA_MODES(operation, 0) \
	UML_DECLARE_CPA_MODES(operation, 1) \
	UML_DECLARE_CPA_MODES(operation, 2) \
	UML_DECLARE_CPA_MODES(operation, 3) \
	UML_DECLARE_CPA_MODES(operation, 4) \
	UML_DECLARE_CPA_MODES(operation, 5) \
	UML_DECLARE_CPA_MODES(operation, 6) \
	UML_DECLARE_CPA_MODES(operation, 7)
UML_DECLARE_CPA_SHIFTS(addpt)
UML_DECLARE_CPA_SHIFTS(subpt)
#undef UML_DECLARE_CPA_SHIFTS
#undef UML_DECLARE_CPA_MODES
#define UML_DECLARE_CPA_MULTIPLY_MODES(operation) \
	void uml_arm64_gadget_cpa_##operation##_rrrr(); \
	void uml_arm64_gadget_cpa_##operation##_rrrz(); \
	void uml_arm64_gadget_cpa_##operation##_rrzr(); \
	void uml_arm64_gadget_cpa_##operation##_rrzz(); \
	void uml_arm64_gadget_cpa_##operation##_rzrr(); \
	void uml_arm64_gadget_cpa_##operation##_rzrz(); \
	void uml_arm64_gadget_cpa_##operation##_rzzr(); \
	void uml_arm64_gadget_cpa_##operation##_rzzz(); \
	void uml_arm64_gadget_cpa_##operation##_zrrr(); \
	void uml_arm64_gadget_cpa_##operation##_zrrz(); \
	void uml_arm64_gadget_cpa_##operation##_zrzr(); \
	void uml_arm64_gadget_cpa_##operation##_zrzz(); \
	void uml_arm64_gadget_cpa_##operation##_zzrr(); \
	void uml_arm64_gadget_cpa_##operation##_zzrz(); \
	void uml_arm64_gadget_cpa_##operation##_zzzr(); \
	void uml_arm64_gadget_cpa_##operation##_zzzz();
UML_DECLARE_CPA_MULTIPLY_MODES(maddpt)
UML_DECLARE_CPA_MULTIPLY_MODES(msubpt)
#undef UML_DECLARE_CPA_MULTIPLY_MODES
#define UML_DECLARE_MULTIPLY_RESULT(prefix) \
	void uml_arm64_gadget_multiply_add_##prefix##_r();
#define UML_DECLARE_MULTIPLY_ADDENDS(prefix) \
	UML_DECLARE_MULTIPLY_RESULT(prefix##_r) \
	UML_DECLARE_MULTIPLY_RESULT(prefix##_z)
#define UML_DECLARE_MULTIPLY_SOURCES(prefix) \
	UML_DECLARE_MULTIPLY_ADDENDS(prefix##_p) \
	UML_DECLARE_MULTIPLY_ADDENDS(prefix##_z)
UML_DECLARE_MULTIPLY_SOURCES(w_madd)
UML_DECLARE_MULTIPLY_SOURCES(w_msub)
UML_DECLARE_MULTIPLY_SOURCES(x_madd)
UML_DECLARE_MULTIPLY_SOURCES(x_msub)
UML_DECLARE_MULTIPLY_SOURCES(smaddl)
UML_DECLARE_MULTIPLY_SOURCES(smsubl)
UML_DECLARE_MULTIPLY_SOURCES(umaddl)
UML_DECLARE_MULTIPLY_SOURCES(umsubl)
#undef UML_DECLARE_MULTIPLY_SOURCES
#undef UML_DECLARE_MULTIPLY_ADDENDS
#define UML_DECLARE_MULTIPLY_HIGH_SOURCES(prefix) \
	UML_DECLARE_MULTIPLY_RESULT(prefix##_p_z) \
	UML_DECLARE_MULTIPLY_RESULT(prefix##_z_z)
UML_DECLARE_MULTIPLY_HIGH_SOURCES(smulh)
UML_DECLARE_MULTIPLY_HIGH_SOURCES(umulh)
#undef UML_DECLARE_MULTIPLY_HIGH_SOURCES
#undef UML_DECLARE_MULTIPLY_RESULT
#define UML_DECLARE_BINARY_SOURCES(kind, prefix) \
	void uml_arm64_gadget_##kind##_##prefix##_rr_r(); \
	void uml_arm64_gadget_##kind##_##prefix##_eq_r(); \
	void uml_arm64_gadget_##kind##_##prefix##_rz_r(); \
	void uml_arm64_gadget_##kind##_##prefix##_zr_r(); \
	void uml_arm64_gadget_##kind##_##prefix##_zz_r();
UML_DECLARE_BINARY_SOURCES(variable_shift, w_lsl)
UML_DECLARE_BINARY_SOURCES(variable_shift, w_lsr)
UML_DECLARE_BINARY_SOURCES(variable_shift, w_asr)
UML_DECLARE_BINARY_SOURCES(variable_shift, w_ror)
UML_DECLARE_BINARY_SOURCES(variable_shift, x_lsl)
UML_DECLARE_BINARY_SOURCES(variable_shift, x_lsr)
UML_DECLARE_BINARY_SOURCES(variable_shift, x_asr)
UML_DECLARE_BINARY_SOURCES(variable_shift, x_ror)
UML_DECLARE_BINARY_SOURCES(divide, w_u)
UML_DECLARE_BINARY_SOURCES(divide, w_s)
UML_DECLARE_BINARY_SOURCES(divide, x_u)
UML_DECLARE_BINARY_SOURCES(divide, x_s)
#undef UML_DECLARE_BINARY_SOURCES
#define UML_DECLARE_CRC_SOURCES(prefix) \
	void uml_arm64_gadget_crc32_##prefix##_rr(); \
	void uml_arm64_gadget_crc32_##prefix##_same(); \
	void uml_arm64_gadget_crc32_##prefix##_rz(); \
	void uml_arm64_gadget_crc32_##prefix##_zr(); \
	void uml_arm64_gadget_crc32_##prefix##_zz(); \
	void uml_arm64_gadget_crc32_software_##prefix##_rr(); \
	void uml_arm64_gadget_crc32_software_##prefix##_same(); \
	void uml_arm64_gadget_crc32_software_##prefix##_rz(); \
	void uml_arm64_gadget_crc32_software_##prefix##_zr(); \
	void uml_arm64_gadget_crc32_software_##prefix##_zz();
UML_DECLARE_CRC_SOURCES(b)
UML_DECLARE_CRC_SOURCES(h)
UML_DECLARE_CRC_SOURCES(w)
UML_DECLARE_CRC_SOURCES(x)
UML_DECLARE_CRC_SOURCES(cb)
UML_DECLARE_CRC_SOURCES(ch)
UML_DECLARE_CRC_SOURCES(cw)
UML_DECLARE_CRC_SOURCES(cx)
#undef UML_DECLARE_CRC_SOURCES
#define UML_DECLARE_EXTRACT_DESTINATIONS(prefix) \
	void uml_arm64_gadget_extract_register_##prefix##_r(); \
	void uml_arm64_gadget_extract_register_##prefix##_z();
#define UML_DECLARE_EXTRACT_SOURCES(prefix) \
	UML_DECLARE_EXTRACT_DESTINATIONS(prefix##_rr) \
	UML_DECLARE_EXTRACT_DESTINATIONS(prefix##_rz) \
	UML_DECLARE_EXTRACT_DESTINATIONS(prefix##_zr) \
	UML_DECLARE_EXTRACT_DESTINATIONS(prefix##_zz)
UML_DECLARE_EXTRACT_SOURCES(w_n)
UML_DECLARE_EXTRACT_SOURCES(w_s)
UML_DECLARE_EXTRACT_SOURCES(x_n)
UML_DECLARE_EXTRACT_SOURCES(x_s)
#undef UML_DECLARE_EXTRACT_SOURCES
#undef UML_DECLARE_EXTRACT_DESTINATIONS
#define UML_DECLARE_CONDITIONAL_SELECT_FAST(name) \
	void uml_arm64_gadget_conditional_select_fast_##name##_w_sel(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_x_sel(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_w_inc(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_x_inc(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_w_inv(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_x_inv(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_w_neg(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_x_neg();
UML_DECLARE_CONDITIONAL_SELECT_FAST(eq)
UML_DECLARE_CONDITIONAL_SELECT_FAST(ne)
UML_DECLARE_CONDITIONAL_SELECT_FAST(hs)
UML_DECLARE_CONDITIONAL_SELECT_FAST(lo)
UML_DECLARE_CONDITIONAL_SELECT_FAST(mi)
UML_DECLARE_CONDITIONAL_SELECT_FAST(pl)
UML_DECLARE_CONDITIONAL_SELECT_FAST(vs)
UML_DECLARE_CONDITIONAL_SELECT_FAST(vc)
UML_DECLARE_CONDITIONAL_SELECT_FAST(hi)
UML_DECLARE_CONDITIONAL_SELECT_FAST(ls)
UML_DECLARE_CONDITIONAL_SELECT_FAST(ge)
UML_DECLARE_CONDITIONAL_SELECT_FAST(lt)
UML_DECLARE_CONDITIONAL_SELECT_FAST(gt)
UML_DECLARE_CONDITIONAL_SELECT_FAST(le)
#undef UML_DECLARE_CONDITIONAL_SELECT_FAST
void uml_arm64_gadget_conditional_select_fast_always_w();
void uml_arm64_gadget_conditional_select_fast_always_x();
#define UML_DECLARE_CONDITIONAL_COMPARE_FAST(name) \
	void uml_arm64_gadget_conditional_compare_fast_##name##_w_cmn_r(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_w_cmn_i(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_x_cmn_r(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_x_cmn_i(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_w_cmp_r(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_w_cmp_i(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_x_cmp_r(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_x_cmp_i();
UML_DECLARE_CONDITIONAL_COMPARE_FAST(eq)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(ne)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(hs)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(lo)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(mi)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(pl)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(vs)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(vc)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(hi)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(ls)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(ge)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(lt)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(gt)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(le)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(always)
#undef UML_DECLARE_CONDITIONAL_COMPARE_FAST
#define UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES(prefix) \
	void uml_arm64_gadget_logical_shifted_##prefix##_rr_r(); \
	void uml_arm64_gadget_logical_shifted_##prefix##_rz_r(); \
	void uml_arm64_gadget_logical_shifted_##prefix##_zr_r(); \
	void uml_arm64_gadget_logical_shifted_##prefix##_zz_r();
#define UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(prefix) \
	UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_n) \
	UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_l) \
	UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_r) \
	UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_a) \
	UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_o)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(w_and_p)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(w_and_n)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(w_orr_p)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(w_orr_n)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(w_eor_p)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(w_eor_n)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(x_and_p)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(x_and_n)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(x_orr_p)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(x_orr_n)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(x_eor_p)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(x_eor_n)
#undef UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS
#undef UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES
#define UML_DECLARE_LOGICAL_SHIFTED_FLAG_DESTINATIONS(prefix) \
	void uml_arm64_gadget_logical_shifted_##prefix##_r(); \
	void uml_arm64_gadget_logical_shifted_##prefix##_z();
#define UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES(prefix) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_DESTINATIONS(prefix##_rr) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_DESTINATIONS(prefix##_rz) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_DESTINATIONS(prefix##_zr) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_DESTINATIONS(prefix##_zz)
#define UML_DECLARE_LOGICAL_SHIFTED_FLAG_SHIFTS(prefix) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_n) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_l) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_r) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_a) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_o)
UML_DECLARE_LOGICAL_SHIFTED_FLAG_SHIFTS(w_ands_p)
UML_DECLARE_LOGICAL_SHIFTED_FLAG_SHIFTS(w_ands_n)
UML_DECLARE_LOGICAL_SHIFTED_FLAG_SHIFTS(x_ands_p)
UML_DECLARE_LOGICAL_SHIFTED_FLAG_SHIFTS(x_ands_n)
#undef UML_DECLARE_LOGICAL_SHIFTED_FLAG_SHIFTS
#undef UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES
#undef UML_DECLARE_LOGICAL_SHIFTED_FLAG_DESTINATIONS
#define UML_DECLARE_LOGICAL_IMMEDIATE_MODES(prefix) \
	void uml_arm64_gadget_logical_immediate_##prefix##_rr(); \
	void uml_arm64_gadget_logical_immediate_##prefix##_rs(); \
	void uml_arm64_gadget_logical_immediate_##prefix##_zr(); \
	void uml_arm64_gadget_logical_immediate_##prefix##_zs();
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(w_and)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(w_orr)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(w_eor)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(w_ands)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(x_and)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(x_orr)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(x_eor)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(x_ands)
#undef UML_DECLARE_LOGICAL_IMMEDIATE_MODES
#define UML_DECLARE_BITFIELD_SOURCES(prefix) \
	void uml_arm64_gadget_bitfield_##prefix##_n_r(); \
	void uml_arm64_gadget_bitfield_##prefix##_n_z(); \
	void uml_arm64_gadget_bitfield_##prefix##_r_r(); \
	void uml_arm64_gadget_bitfield_##prefix##_r_z();
UML_DECLARE_BITFIELD_SOURCES(w_s)
UML_DECLARE_BITFIELD_SOURCES(w_b)
UML_DECLARE_BITFIELD_SOURCES(w_u)
UML_DECLARE_BITFIELD_SOURCES(x_s)
UML_DECLARE_BITFIELD_SOURCES(x_b)
UML_DECLARE_BITFIELD_SOURCES(x_u)
#undef UML_DECLARE_BITFIELD_SOURCES
#define UML_DECLARE_BIT_REVERSE_COUNT(prefix) \
	void uml_arm64_gadget_bit_reverse_count_##prefix();
UML_DECLARE_BIT_REVERSE_COUNT(w_rbit)
UML_DECLARE_BIT_REVERSE_COUNT(w_rev16)
UML_DECLARE_BIT_REVERSE_COUNT(w_rev)
UML_DECLARE_BIT_REVERSE_COUNT(w_clz)
UML_DECLARE_BIT_REVERSE_COUNT(w_cls)
UML_DECLARE_BIT_REVERSE_COUNT(w_ctz)
UML_DECLARE_BIT_REVERSE_COUNT(w_cnt)
UML_DECLARE_BIT_REVERSE_COUNT(w_abs)
UML_DECLARE_BIT_REVERSE_COUNT(x_rbit)
UML_DECLARE_BIT_REVERSE_COUNT(x_rev16)
UML_DECLARE_BIT_REVERSE_COUNT(x_rev32)
UML_DECLARE_BIT_REVERSE_COUNT(x_rev)
UML_DECLARE_BIT_REVERSE_COUNT(x_clz)
UML_DECLARE_BIT_REVERSE_COUNT(x_cls)
UML_DECLARE_BIT_REVERSE_COUNT(x_ctz)
UML_DECLARE_BIT_REVERSE_COUNT(x_cnt)
UML_DECLARE_BIT_REVERSE_COUNT(x_abs)
UML_DECLARE_BIT_REVERSE_COUNT(w_ctz_native)
UML_DECLARE_BIT_REVERSE_COUNT(w_cnt_native)
UML_DECLARE_BIT_REVERSE_COUNT(w_abs_native)
UML_DECLARE_BIT_REVERSE_COUNT(x_ctz_native)
UML_DECLARE_BIT_REVERSE_COUNT(x_cnt_native)
UML_DECLARE_BIT_REVERSE_COUNT(x_abs_native)
#undef UML_DECLARE_BIT_REVERSE_COUNT
void uml_arm64_gadget_xpaci();
void uml_arm64_gadget_xpacd();
void uml_arm64_gadget_xpaclri();
#define DECLARE_POINTER_AUTH(name) \
	void uml_arm64_gadget_pauth_##name##_r(); \
	void uml_arm64_gadget_pauth_##name##_z();
DECLARE_POINTER_AUTH(pacia)
DECLARE_POINTER_AUTH(pacib)
DECLARE_POINTER_AUTH(pacda)
DECLARE_POINTER_AUTH(pacdb)
DECLARE_POINTER_AUTH(autia)
DECLARE_POINTER_AUTH(autib)
DECLARE_POINTER_AUTH(autda)
DECLARE_POINTER_AUTH(autdb)
#undef DECLARE_POINTER_AUTH
void uml_arm64_gadget_pauth_pacga_r();
void uml_arm64_gadget_pauth_pacga_z();
#define UML_DECLARE_INTEGER_MIN_MAX_FAST(name) \
	void uml_arm64_gadget_integer_##name##_fast_w_r(); \
	void uml_arm64_gadget_integer_##name##_fast_w_i(); \
	void uml_arm64_gadget_integer_##name##_fast_x_r(); \
	void uml_arm64_gadget_integer_##name##_fast_x_i();
UML_DECLARE_INTEGER_MIN_MAX_FAST(smax)
UML_DECLARE_INTEGER_MIN_MAX_FAST(umax)
UML_DECLARE_INTEGER_MIN_MAX_FAST(smin)
UML_DECLARE_INTEGER_MIN_MAX_FAST(umin)
#undef UML_DECLARE_INTEGER_MIN_MAX_FAST
void uml_arm64_gadget_integer_min_max_copy_w();
void uml_arm64_gadget_integer_min_max_copy_x();
void uml_arm64_gadget_cfinv();
void uml_arm64_gadget_axflag();
void uml_arm64_gadget_axflag_native();
void uml_arm64_gadget_xaflag();
void uml_arm64_gadget_xaflag_native();
void uml_arm64_gadget_rmif_fast();
void uml_arm64_gadget_rmif_zero();
void uml_arm64_gadget_setf_fast_8();
void uml_arm64_gadget_setf_fast_16();
void uml_arm64_gadget_setf_native_8();
void uml_arm64_gadget_setf_native_16();
void uml_arm64_gadget_floating_move_immediate();
void uml_arm64_gadget_floating_move_half();
void uml_arm64_gadget_floating_move_single();
void uml_arm64_gadget_floating_move_double();
void uml_arm64_gadget_floating_absolute_half();
void uml_arm64_gadget_floating_absolute_single();
void uml_arm64_gadget_floating_absolute_double();
void uml_arm64_gadget_floating_negate_half();
void uml_arm64_gadget_floating_negate_single();
void uml_arm64_gadget_floating_negate_double();
void uml_arm64_gadget_floating_sqrt_half();
void uml_arm64_gadget_floating_sqrt_single();
void uml_arm64_gadget_floating_sqrt_double();
void uml_arm64_gadget_floating_multiply_half();
void uml_arm64_gadget_floating_multiply_single();
void uml_arm64_gadget_floating_multiply_double();
void uml_arm64_gadget_floating_divide_half();
void uml_arm64_gadget_floating_divide_single();
void uml_arm64_gadget_floating_divide_double();
void uml_arm64_gadget_floating_add_half();
void uml_arm64_gadget_floating_add_single();
void uml_arm64_gadget_floating_add_double();
void uml_arm64_gadget_floating_subtract_half();
void uml_arm64_gadget_floating_subtract_single();
void uml_arm64_gadget_floating_subtract_double();
void uml_arm64_gadget_floating_negative_multiply_half();
void uml_arm64_gadget_floating_negative_multiply_single();
void uml_arm64_gadget_floating_negative_multiply_double();
#define UML_DECLARE_SIMD_BASIC_FLOATING(operation) \
	void uml_arm64_gadget_simd_basic_##operation##_4h(); \
	void uml_arm64_gadget_simd_basic_##operation##_8h(); \
	void uml_arm64_gadget_simd_basic_##operation##_2s(); \
	void uml_arm64_gadget_simd_basic_##operation##_4s(); \
	void uml_arm64_gadget_simd_basic_##operation##_2d();
UML_DECLARE_SIMD_BASIC_FLOATING(fmul)
UML_DECLARE_SIMD_BASIC_FLOATING(fdiv)
UML_DECLARE_SIMD_BASIC_FLOATING(fadd)
UML_DECLARE_SIMD_BASIC_FLOATING(fsub)
UML_DECLARE_SIMD_BASIC_FLOATING(fsqrt)
#undef UML_DECLARE_SIMD_BASIC_FLOATING
void uml_arm64_gadget_floating_madd_half();
void uml_arm64_gadget_floating_madd_single();
void uml_arm64_gadget_floating_madd_double();
void uml_arm64_gadget_floating_msub_half();
void uml_arm64_gadget_floating_msub_single();
void uml_arm64_gadget_floating_msub_double();
void uml_arm64_gadget_floating_nmadd_half();
void uml_arm64_gadget_floating_nmadd_single();
void uml_arm64_gadget_floating_nmadd_double();
void uml_arm64_gadget_floating_nmsub_half();
void uml_arm64_gadget_floating_nmsub_single();
void uml_arm64_gadget_floating_nmsub_double();
#define UML_DECLARE_SIMD_FUSED(operation) \
	void uml_arm64_gadget_simd_fused_##operation##_4h(); \
	void uml_arm64_gadget_simd_fused_##operation##_8h(); \
	void uml_arm64_gadget_simd_fused_##operation##_2s(); \
	void uml_arm64_gadget_simd_fused_##operation##_4s(); \
	void uml_arm64_gadget_simd_fused_##operation##_2d();
UML_DECLARE_SIMD_FUSED(fmla)
UML_DECLARE_SIMD_FUSED(fmls)
#undef UML_DECLARE_SIMD_FUSED
#define UML_DECLARE_FLOATING_CONVERT(source, destination) \
	void uml_arm64_gadget_floating_convert_##source##_to_##destination();
UML_DECLARE_FLOATING_CONVERT(h, s)
UML_DECLARE_FLOATING_CONVERT(h, d)
UML_DECLARE_FLOATING_CONVERT(s, h)
UML_DECLARE_FLOATING_CONVERT(s, d)
UML_DECLARE_FLOATING_CONVERT(d, h)
UML_DECLARE_FLOATING_CONVERT(d, s)
#undef UML_DECLARE_FLOATING_CONVERT
void uml_arm64_gadget_floating_javascript_convert();
void uml_arm64_gadget_floating_javascript_convert_discard();
void uml_arm64_gadget_floating_javascript_convert_software_fallback();
void uml_arm64_gadget_floating_to_integer_software_fallback();
#define UML_DECLARE_FLOATING_ROUND_SCALAR(name) \
	void uml_arm64_gadget_floating_round_##name##_h(); \
	void uml_arm64_gadget_floating_round_##name##_s(); \
	void uml_arm64_gadget_floating_round_##name##_d();
UML_DECLARE_FLOATING_ROUND_SCALAR(n)
UML_DECLARE_FLOATING_ROUND_SCALAR(p)
UML_DECLARE_FLOATING_ROUND_SCALAR(m)
UML_DECLARE_FLOATING_ROUND_SCALAR(z)
UML_DECLARE_FLOATING_ROUND_SCALAR(a)
UML_DECLARE_FLOATING_ROUND_SCALAR(x)
UML_DECLARE_FLOATING_ROUND_SCALAR(i)
#undef UML_DECLARE_FLOATING_ROUND_SCALAR
#define UML_DECLARE_FLOATING_ROUND_VECTOR(name) \
	void uml_arm64_gadget_simd_round_##name##_4h(); \
	void uml_arm64_gadget_simd_round_##name##_8h(); \
	void uml_arm64_gadget_simd_round_##name##_2s(); \
	void uml_arm64_gadget_simd_round_##name##_4s(); \
	void uml_arm64_gadget_simd_round_##name##_2d();
UML_DECLARE_FLOATING_ROUND_VECTOR(n)
UML_DECLARE_FLOATING_ROUND_VECTOR(p)
UML_DECLARE_FLOATING_ROUND_VECTOR(m)
UML_DECLARE_FLOATING_ROUND_VECTOR(z)
UML_DECLARE_FLOATING_ROUND_VECTOR(a)
UML_DECLARE_FLOATING_ROUND_VECTOR(x)
UML_DECLARE_FLOATING_ROUND_VECTOR(i)
#undef UML_DECLARE_FLOATING_ROUND_VECTOR
#define UML_DECLARE_FLOATING_ROUND_SIZED(name) \
	void uml_arm64_gadget_floating_round_##name##_s(); \
	void uml_arm64_gadget_floating_round_##name##_d(); \
	void uml_arm64_gadget_simd_round_##name##_2s(); \
	void uml_arm64_gadget_simd_round_##name##_4s(); \
	void uml_arm64_gadget_simd_round_##name##_2d();
UML_DECLARE_FLOATING_ROUND_SIZED(32x)
UML_DECLARE_FLOATING_ROUND_SIZED(32z)
UML_DECLARE_FLOATING_ROUND_SIZED(64x)
UML_DECLARE_FLOATING_ROUND_SIZED(64z)
#undef UML_DECLARE_FLOATING_ROUND_SIZED
void uml_arm64_gadget_floating_round_sized_software();
#define UML_DECLARE_FLOATING_HORIZONTAL_SCALAR(name) \
	void uml_arm64_gadget_floating_horizontal_##name##_h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_d();
#define UML_DECLARE_FLOATING_HORIZONTAL_VECTOR(name) \
	void uml_arm64_gadget_floating_horizontal_##name##_4h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_8h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_2s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_4s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_2d();
#define UML_DECLARE_FLOATING_HORIZONTAL_BINARY(name) \
	UML_DECLARE_FLOATING_HORIZONTAL_SCALAR(name##_scalar) \
	UML_DECLARE_FLOATING_HORIZONTAL_VECTOR(name##_vector)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fabd)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fabd_checked)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(faddp_checked)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fmax)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fmaxnm)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fmin)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fminnm)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fmax_ah)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fmaxnm_ah)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fmin_ah)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fminnm_ah)
UML_DECLARE_FLOATING_HORIZONTAL_VECTOR(famax_native)
UML_DECLARE_FLOATING_HORIZONTAL_VECTOR(famin_native)
void uml_arm64_gadget_floating_horizontal_absolute_software();
void uml_arm64_gadget_floating_scale_native_4h();
void uml_arm64_gadget_floating_scale_native_8h();
void uml_arm64_gadget_floating_scale_native_2s();
void uml_arm64_gadget_floating_scale_native_4s();
void uml_arm64_gadget_floating_scale_native_2d();
void uml_arm64_gadget_floating_scale_software_4h();
void uml_arm64_gadget_floating_scale_software_8h();
void uml_arm64_gadget_floating_scale_software_2s();
void uml_arm64_gadget_floating_scale_software_4s();
void uml_arm64_gadget_floating_scale_software_2d();
void uml_arm64_gadget_bfloat_convert_scalar_native();
void uml_arm64_gadget_bfloat_convert_lower_native();
void uml_arm64_gadget_bfloat_convert_upper_native();
void uml_arm64_gadget_bfloat_convert_software();
void uml_arm64_gadget_bfloat_dot_native_vector_2s();
void uml_arm64_gadget_bfloat_dot_native_vector_4s();
#define UML_DECLARE_BFLOAT_DOT_INDEXED(shape, index) \
	void uml_arm64_gadget_bfloat_dot_native_indexed_##shape##_##index();
UML_DECLARE_BFLOAT_DOT_INDEXED(2s, 0)
UML_DECLARE_BFLOAT_DOT_INDEXED(2s, 1)
UML_DECLARE_BFLOAT_DOT_INDEXED(2s, 2)
UML_DECLARE_BFLOAT_DOT_INDEXED(2s, 3)
UML_DECLARE_BFLOAT_DOT_INDEXED(4s, 0)
UML_DECLARE_BFLOAT_DOT_INDEXED(4s, 1)
UML_DECLARE_BFLOAT_DOT_INDEXED(4s, 2)
UML_DECLARE_BFLOAT_DOT_INDEXED(4s, 3)
#undef UML_DECLARE_BFLOAT_DOT_INDEXED
void uml_arm64_gadget_bfloat_dot_software();
void uml_arm64_gadget_bfloat_matrix_native();
void uml_arm64_gadget_bfloat_matrix_software();
#define UML_DECLARE_HALF_FUSED_LONG_VARIANTS(name) \
	void uml_arm64_gadget_half_fused_long_native_##name();
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_add_lower_2s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_add_lower_4s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_add_upper_2s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_add_upper_4s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_sub_lower_2s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_sub_lower_4s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_sub_upper_2s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_sub_upper_4s)
#define UML_DECLARE_HALF_FUSED_LONG_INDEXED(operation, half, shape) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_0) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_1) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_2) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_3) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_4) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_5) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_6) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_7)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(add, lower, 2s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(add, lower, 4s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(add, upper, 2s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(add, upper, 4s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(sub, lower, 2s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(sub, lower, 4s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(sub, upper, 2s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(sub, upper, 4s)
#undef UML_DECLARE_HALF_FUSED_LONG_INDEXED
#undef UML_DECLARE_HALF_FUSED_LONG_VARIANTS
void uml_arm64_gadget_half_fused_long_software();
void uml_arm64_gadget_half_matrix_native_non_widening();
void uml_arm64_gadget_half_matrix_native_widening();
void uml_arm64_gadget_half_matrix_software();
#define UML_DECLARE_HALF_DOT_VARIANTS(name) \
	void uml_arm64_gadget_half_dot_native_##name();
UML_DECLARE_HALF_DOT_VARIANTS(vector_2s)
UML_DECLARE_HALF_DOT_VARIANTS(vector_4s)
#define UML_DECLARE_HALF_DOT_INDEXED(shape, index) \
	UML_DECLARE_HALF_DOT_VARIANTS(indexed_##shape##_##index)
UML_DECLARE_HALF_DOT_INDEXED(2s, 0)
UML_DECLARE_HALF_DOT_INDEXED(2s, 1)
UML_DECLARE_HALF_DOT_INDEXED(2s, 2)
UML_DECLARE_HALF_DOT_INDEXED(2s, 3)
UML_DECLARE_HALF_DOT_INDEXED(4s, 0)
UML_DECLARE_HALF_DOT_INDEXED(4s, 1)
UML_DECLARE_HALF_DOT_INDEXED(4s, 2)
UML_DECLARE_HALF_DOT_INDEXED(4s, 3)
#undef UML_DECLARE_HALF_DOT_INDEXED
#undef UML_DECLARE_HALF_DOT_VARIANTS
void uml_arm64_gadget_half_dot_software();
void uml_arm64_gadget_bfloat_multiply_long_native_vector_b();
void uml_arm64_gadget_bfloat_multiply_long_native_vector_t();
#define UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(half, index) \
	void uml_arm64_gadget_bfloat_multiply_long_native_indexed_##half##_##index();
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 0)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 1)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 2)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 3)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 4)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 5)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 6)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 7)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 0)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 1)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 2)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 3)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 4)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 5)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 6)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 7)
#undef UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED
void uml_arm64_gadget_bfloat_multiply_long_software();
#undef UML_DECLARE_FLOATING_HORIZONTAL_BINARY
#define UML_DECLARE_FLOATING_HORIZONTAL_PAIR(name) \
	UML_DECLARE_FLOATING_HORIZONTAL_SCALAR(name##_scalar) \
	UML_DECLARE_FLOATING_HORIZONTAL_VECTOR(name##_vector)
UML_DECLARE_FLOATING_HORIZONTAL_PAIR(faddp)
UML_DECLARE_FLOATING_HORIZONTAL_PAIR(fmaxp)
UML_DECLARE_FLOATING_HORIZONTAL_PAIR(fmaxnmp)
UML_DECLARE_FLOATING_HORIZONTAL_PAIR(fminp)
UML_DECLARE_FLOATING_HORIZONTAL_PAIR(fminnmp)
UML_DECLARE_FLOATING_HORIZONTAL_PAIR(fmaxp_ah)
UML_DECLARE_FLOATING_HORIZONTAL_PAIR(fmaxnmp_ah)
UML_DECLARE_FLOATING_HORIZONTAL_PAIR(fminp_ah)
UML_DECLARE_FLOATING_HORIZONTAL_PAIR(fminnmp_ah)
#undef UML_DECLARE_FLOATING_HORIZONTAL_PAIR
#define UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(name) \
	void uml_arm64_gadget_floating_horizontal_##name##_4h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_8h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_4s();
UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(fmaxv)
UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(fmaxnmv)
UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(fminv)
UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(fminnmv)
UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(fmaxv_ah)
UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(fmaxnmv_ah)
UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(fminv_ah)
UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(fminnmv_ah)
#undef UML_DECLARE_FLOATING_HORIZONTAL_REDUCE
#undef UML_DECLARE_FLOATING_HORIZONTAL_VECTOR
#undef UML_DECLARE_FLOATING_HORIZONTAL_SCALAR
#define UML_DECLARE_FLOATING_RECIPROCAL_SCALAR(host, name) \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_h(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_s(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_d();
#define UML_DECLARE_FLOATING_RECIPROCAL_VECTOR(host, name) \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4h(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_8h(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2s(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4s(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2d();
#define UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR(name) \
	UML_DECLARE_FLOATING_RECIPROCAL_SCALAR(native, name) \
	UML_DECLARE_FLOATING_RECIPROCAL_SCALAR(checked, name)
#define UML_DECLARE_FLOATING_RECIPROCAL_VECTOR_PAIR(name) \
	UML_DECLARE_FLOATING_RECIPROCAL_VECTOR(native, name) \
	UML_DECLARE_FLOATING_RECIPROCAL_VECTOR(checked, name)
UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR(frecpe)
UML_DECLARE_FLOATING_RECIPROCAL_VECTOR_PAIR(frecpe)
UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR(frecps)
UML_DECLARE_FLOATING_RECIPROCAL_VECTOR_PAIR(frecps)
UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR(frecpx)
UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR(frsqrte)
UML_DECLARE_FLOATING_RECIPROCAL_VECTOR_PAIR(frsqrte)
UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR(frsqrts)
UML_DECLARE_FLOATING_RECIPROCAL_VECTOR_PAIR(frsqrts)
#undef UML_DECLARE_FLOATING_RECIPROCAL_VECTOR_PAIR
#undef UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR
#undef UML_DECLARE_FLOATING_RECIPROCAL_VECTOR
#undef UML_DECLARE_FLOATING_RECIPROCAL_SCALAR
#define UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(name, shape) \
	void uml_arm64_gadget_floating_complex_##name##_##shape##_0(); \
	void uml_arm64_gadget_floating_complex_##name##_##shape##_90(); \
	void uml_arm64_gadget_floating_complex_##name##_##shape##_180(); \
	void uml_arm64_gadget_floating_complex_##name##_##shape##_270();
#define UML_DECLARE_FLOATING_COMPLEX_ADD(shape) \
	void uml_arm64_gadget_floating_complex_fcadd_##shape##_90(); \
	void uml_arm64_gadget_floating_complex_fcadd_##shape##_270();
UML_DECLARE_FLOATING_COMPLEX_ADD(4h)
UML_DECLARE_FLOATING_COMPLEX_ADD(8h)
UML_DECLARE_FLOATING_COMPLEX_ADD(2s)
UML_DECLARE_FLOATING_COMPLEX_ADD(4s)
UML_DECLARE_FLOATING_COMPLEX_ADD(2d)
#undef UML_DECLARE_FLOATING_COMPLEX_ADD
UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(fcmla, 4h)
UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(fcmla, 8h)
UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(fcmla, 2s)
UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(fcmla, 4s)
UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(fcmla, 2d)
#define UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(shape, index) \
	UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(fcmla_element, shape##_##index)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(4h, 0)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(4h, 1)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(8h, 0)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(8h, 1)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(8h, 2)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(8h, 3)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(4s, 0)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(4s, 1)
#undef UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX
#undef UML_DECLARE_FLOATING_COMPLEX_ROTATIONS
#define UML_DECLARE_FLOATING_COMPARE(width) \
	void uml_arm64_gadget_floating_compare_quiet_##width##_register(); \
	void uml_arm64_gadget_floating_compare_quiet_##width##_zero(); \
	void uml_arm64_gadget_floating_compare_signaling_##width##_register(); \
	void uml_arm64_gadget_floating_compare_signaling_##width##_zero();
UML_DECLARE_FLOATING_COMPARE(h)
UML_DECLARE_FLOATING_COMPARE(s)
UML_DECLARE_FLOATING_COMPARE(d)
#undef UML_DECLARE_FLOATING_COMPARE
#define UML_DECLARE_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, width) \
	void uml_arm64_gadget_floating_conditional_compare_##name##_quiet_##width(); \
	void uml_arm64_gadget_floating_conditional_compare_##name##_signaling_##width();
#define UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(name) \
	UML_DECLARE_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, h) \
	UML_DECLARE_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, s) \
	UML_DECLARE_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, d)
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
#undef UML_DECLARE_FLOATING_CONDITIONAL_COMPARE
#undef UML_DECLARE_FLOATING_CONDITIONAL_COMPARE_WIDTH
#define UML_DECLARE_SIMD_FLOATING_COMPARE(name) \
	void uml_arm64_gadget_simd_fcmp_##name##_h(); \
	void uml_arm64_gadget_simd_fcmp_##name##_s(); \
	void uml_arm64_gadget_simd_fcmp_##name##_d(); \
	void uml_arm64_gadget_simd_fcmp_##name##_4h(); \
	void uml_arm64_gadget_simd_fcmp_##name##_8h(); \
	void uml_arm64_gadget_simd_fcmp_##name##_2s(); \
	void uml_arm64_gadget_simd_fcmp_##name##_4s(); \
	void uml_arm64_gadget_simd_fcmp_##name##_2d();
UML_DECLARE_SIMD_FLOATING_COMPARE(eq)
UML_DECLARE_SIMD_FLOATING_COMPARE(ge)
UML_DECLARE_SIMD_FLOATING_COMPARE(gt)
UML_DECLARE_SIMD_FLOATING_COMPARE(facge)
UML_DECLARE_SIMD_FLOATING_COMPARE(facgt)
UML_DECLARE_SIMD_FLOATING_COMPARE(eq0)
UML_DECLARE_SIMD_FLOATING_COMPARE(ge0)
UML_DECLARE_SIMD_FLOATING_COMPARE(gt0)
UML_DECLARE_SIMD_FLOATING_COMPARE(le0)
UML_DECLARE_SIMD_FLOATING_COMPARE(lt0)
#undef UML_DECLARE_SIMD_FLOATING_COMPARE
void uml_arm64_gadget_simd_fcvtl_hs();
void uml_arm64_gadget_simd_fcvtl2_hs();
void uml_arm64_gadget_simd_fcvtl_sd();
void uml_arm64_gadget_simd_fcvtl2_sd();
void uml_arm64_gadget_simd_fcvtn_sh();
void uml_arm64_gadget_simd_fcvtn2_sh();
void uml_arm64_gadget_simd_fcvtn_ds();
void uml_arm64_gadget_simd_fcvtn2_ds();
void uml_arm64_gadget_simd_fcvtxn_scalar();
void uml_arm64_gadget_simd_fcvtxn_vector();
void uml_arm64_gadget_simd_fcvtxn2_vector();
#define UML_DECLARE_FLOATING_TO_GPR(operation, floating) \
	void uml_arm64_gadget_floating_to_gpr_##operation##_w_##floating(); \
	void uml_arm64_gadget_floating_to_gpr_##operation##_x_##floating(); \
	void uml_arm64_gadget_floating_to_gpr_##operation##_w_##floating##_discard(); \
	void uml_arm64_gadget_floating_to_gpr_##operation##_x_##floating##_discard();
#define UML_DECLARE_FLOATING_TO_GPR_WIDTHS(operation) \
	UML_DECLARE_FLOATING_TO_GPR(operation, h) \
	UML_DECLARE_FLOATING_TO_GPR(operation, s) \
	UML_DECLARE_FLOATING_TO_GPR(operation, d)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtns)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtnu)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtps)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtpu)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtms)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtmu)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtzs)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtzu)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtas)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtau)
#undef UML_DECLARE_FLOATING_TO_GPR_WIDTHS
#undef UML_DECLARE_FLOATING_TO_GPR
#define UML_DECLARE_SIMD_FLOATING_TO_INTEGER(operation) \
	void uml_arm64_gadget_simd_##operation##_h(); \
	void uml_arm64_gadget_simd_##operation##_s(); \
	void uml_arm64_gadget_simd_##operation##_d(); \
	void uml_arm64_gadget_simd_##operation##_4h(); \
	void uml_arm64_gadget_simd_##operation##_8h(); \
	void uml_arm64_gadget_simd_##operation##_2s(); \
	void uml_arm64_gadget_simd_##operation##_4s(); \
	void uml_arm64_gadget_simd_##operation##_2d();
UML_DECLARE_SIMD_FLOATING_TO_INTEGER(fcvtns)
UML_DECLARE_SIMD_FLOATING_TO_INTEGER(fcvtnu)
UML_DECLARE_SIMD_FLOATING_TO_INTEGER(fcvtps)
UML_DECLARE_SIMD_FLOATING_TO_INTEGER(fcvtpu)
UML_DECLARE_SIMD_FLOATING_TO_INTEGER(fcvtms)
UML_DECLARE_SIMD_FLOATING_TO_INTEGER(fcvtmu)
UML_DECLARE_SIMD_FLOATING_TO_INTEGER(fcvtzs)
UML_DECLARE_SIMD_FLOATING_TO_INTEGER(fcvtzu)
UML_DECLARE_SIMD_FLOATING_TO_INTEGER(fcvtas)
UML_DECLARE_SIMD_FLOATING_TO_INTEGER(fcvtau)
#undef UML_DECLARE_SIMD_FLOATING_TO_INTEGER
#define UML_DECLARE_FLOATING_TO_SIMD_INTEGER(operation) \
	void uml_arm64_gadget_floating_to_simd_##operation##_w_h(); \
	void uml_arm64_gadget_floating_to_simd_##operation##_x_h(); \
	void uml_arm64_gadget_floating_to_simd_##operation##_x_s(); \
	void uml_arm64_gadget_floating_to_simd_##operation##_w_d();
UML_DECLARE_FLOATING_TO_SIMD_INTEGER(fcvtns)
UML_DECLARE_FLOATING_TO_SIMD_INTEGER(fcvtnu)
UML_DECLARE_FLOATING_TO_SIMD_INTEGER(fcvtps)
UML_DECLARE_FLOATING_TO_SIMD_INTEGER(fcvtpu)
UML_DECLARE_FLOATING_TO_SIMD_INTEGER(fcvtms)
UML_DECLARE_FLOATING_TO_SIMD_INTEGER(fcvtmu)
UML_DECLARE_FLOATING_TO_SIMD_INTEGER(fcvtzs)
UML_DECLARE_FLOATING_TO_SIMD_INTEGER(fcvtzu)
UML_DECLARE_FLOATING_TO_SIMD_INTEGER(fcvtas)
UML_DECLARE_FLOATING_TO_SIMD_INTEGER(fcvtau)
#undef UML_DECLARE_FLOATING_TO_SIMD_INTEGER
#define UML_DECLARE_FIXED_TABLE(name) \
	void uml_arm64_gadget_##name(); \
	void uml_arm64_gadget_##name##_end();
#define UML_DECLARE_FLOATING_TO_GPR_FIXED(operation, floating) \
	UML_DECLARE_FIXED_TABLE(floating_to_gpr_fixed_##operation##_w_##floating) \
	UML_DECLARE_FIXED_TABLE(floating_to_gpr_fixed_##operation##_x_##floating) \
	UML_DECLARE_FIXED_TABLE( \
		floating_to_gpr_fixed_##operation##_w_##floating##_discard) \
	UML_DECLARE_FIXED_TABLE( \
		floating_to_gpr_fixed_##operation##_x_##floating##_discard)
#define UML_DECLARE_FLOATING_TO_GPR_FIXED_WIDTHS(operation) \
	UML_DECLARE_FLOATING_TO_GPR_FIXED(operation, h) \
	UML_DECLARE_FLOATING_TO_GPR_FIXED(operation, s) \
	UML_DECLARE_FLOATING_TO_GPR_FIXED(operation, d)
UML_DECLARE_FLOATING_TO_GPR_FIXED_WIDTHS(fcvtzs)
UML_DECLARE_FLOATING_TO_GPR_FIXED_WIDTHS(fcvtzu)
#undef UML_DECLARE_FLOATING_TO_GPR_FIXED_WIDTHS
#undef UML_DECLARE_FLOATING_TO_GPR_FIXED
#define UML_DECLARE_SIMD_FLOATING_TO_INTEGER_FIXED(operation) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_h) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_s) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_d) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_4h) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_8h) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_2s) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_4s) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_2d)
UML_DECLARE_SIMD_FLOATING_TO_INTEGER_FIXED(fcvtzs)
UML_DECLARE_SIMD_FLOATING_TO_INTEGER_FIXED(fcvtzu)
#undef UML_DECLARE_SIMD_FLOATING_TO_INTEGER_FIXED
#undef UML_DECLARE_FIXED_TABLE
#define UML_DECLARE_GPR_TO_FLOATING(operation, integer, floating, zero) \
	void uml_arm64_gadget_gpr_to_floating_##operation##_##integer##_##floating##zero();
#define UML_DECLARE_GPR_TO_FLOATING_WIDTHS(operation, integer, zero) \
	UML_DECLARE_GPR_TO_FLOATING(operation, integer, h, zero) \
	UML_DECLARE_GPR_TO_FLOATING(operation, integer, s, zero) \
	UML_DECLARE_GPR_TO_FLOATING(operation, integer, d, zero)
#define UML_DECLARE_GPR_TO_FLOATING_VARIANTS(operation) \
	UML_DECLARE_GPR_TO_FLOATING_WIDTHS(operation, w, 0) \
	UML_DECLARE_GPR_TO_FLOATING_WIDTHS(operation, x, 0) \
	UML_DECLARE_GPR_TO_FLOATING_WIDTHS(operation, w, 1) \
	UML_DECLARE_GPR_TO_FLOATING_WIDTHS(operation, x, 1)
UML_DECLARE_GPR_TO_FLOATING_VARIANTS(scvtf)
UML_DECLARE_GPR_TO_FLOATING_VARIANTS(ucvtf)
#undef UML_DECLARE_GPR_TO_FLOATING_VARIANTS
#undef UML_DECLARE_GPR_TO_FLOATING_WIDTHS
#undef UML_DECLARE_GPR_TO_FLOATING
void uml_arm64_gadget_integer_to_floating_software_fallback();
#define UML_DECLARE_SIMD_INTEGER_TO_FLOATING(operation) \
	void uml_arm64_gadget_simd_##operation##_h(); \
	void uml_arm64_gadget_simd_##operation##_s(); \
	void uml_arm64_gadget_simd_##operation##_d(); \
	void uml_arm64_gadget_simd_##operation##_4h(); \
	void uml_arm64_gadget_simd_##operation##_8h(); \
	void uml_arm64_gadget_simd_##operation##_2s(); \
	void uml_arm64_gadget_simd_##operation##_4s(); \
	void uml_arm64_gadget_simd_##operation##_2d(); \
	void uml_arm64_gadget_simd_cross_##operation##_h_w(); \
	void uml_arm64_gadget_simd_cross_##operation##_h_x(); \
	void uml_arm64_gadget_simd_cross_##operation##_s_x(); \
	void uml_arm64_gadget_simd_cross_##operation##_d_w();
UML_DECLARE_SIMD_INTEGER_TO_FLOATING(scvtf)
UML_DECLARE_SIMD_INTEGER_TO_FLOATING(ucvtf)
#undef UML_DECLARE_SIMD_INTEGER_TO_FLOATING
#define UML_DECLARE_INTEGER_FLOAT_FIXED_TABLE(name) \
	void uml_arm64_gadget_##name(); \
	void uml_arm64_gadget_##name##_end();
#define UML_DECLARE_GPR_TO_FLOATING_FIXED(operation, integer, floating, zero) \
	UML_DECLARE_INTEGER_FLOAT_FIXED_TABLE( \
		gpr_to_floating_fixed_##operation##_##integer##_##floating##zero)
#define UML_DECLARE_GPR_TO_FLOATING_FIXED_WIDTHS(operation, integer, zero) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED(operation, integer, h, zero) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED(operation, integer, s, zero) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED(operation, integer, d, zero)
#define UML_DECLARE_GPR_TO_FLOATING_FIXED_VARIANTS(operation) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED_WIDTHS(operation, w, 0) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED_WIDTHS(operation, x, 0) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED_WIDTHS(operation, w, 1) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED_WIDTHS(operation, x, 1)
UML_DECLARE_GPR_TO_FLOATING_FIXED_VARIANTS(scvtf)
UML_DECLARE_GPR_TO_FLOATING_FIXED_VARIANTS(ucvtf)
#undef UML_DECLARE_GPR_TO_FLOATING_FIXED_VARIANTS
#undef UML_DECLARE_GPR_TO_FLOATING_FIXED_WIDTHS
#undef UML_DECLARE_GPR_TO_FLOATING_FIXED
#define UML_DECLARE_SIMD_INTEGER_TO_FLOATING_FIXED(operation) \
	UML_DECLARE_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_h) \
	UML_DECLARE_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_s) \
	UML_DECLARE_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_d) \
	UML_DECLARE_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_4h) \
	UML_DECLARE_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_8h) \
	UML_DECLARE_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_2s) \
	UML_DECLARE_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_4s) \
	UML_DECLARE_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_2d)
UML_DECLARE_SIMD_INTEGER_TO_FLOATING_FIXED(scvtf)
UML_DECLARE_SIMD_INTEGER_TO_FLOATING_FIXED(ucvtf)
#undef UML_DECLARE_SIMD_INTEGER_TO_FLOATING_FIXED
#undef UML_DECLARE_INTEGER_FLOAT_FIXED_TABLE
#define UML_DECLARE_SIMD_BITWISE(name) \
	void uml_arm64_gadget_##name##_8b(); \
	void uml_arm64_gadget_##name##_16b();
UML_DECLARE_SIMD_BITWISE(simd_and)
UML_DECLARE_SIMD_BITWISE(simd_bic)
UML_DECLARE_SIMD_BITWISE(simd_orr)
UML_DECLARE_SIMD_BITWISE(simd_orn)
UML_DECLARE_SIMD_BITWISE(simd_eor)
UML_DECLARE_SIMD_BITWISE(simd_bsl)
UML_DECLARE_SIMD_BITWISE(simd_bit)
UML_DECLARE_SIMD_BITWISE(simd_bif)
#undef UML_DECLARE_SIMD_BITWISE
void uml_arm64_gadget_simd_mov_8b();
void uml_arm64_gadget_simd_mov_16b();
#define UML_DECLARE_SIMD_IMMEDIATE(name) \
	void uml_arm64_gadget_simd_immediate_##name##_8b(); \
	void uml_arm64_gadget_simd_immediate_##name##_16b();
UML_DECLARE_SIMD_IMMEDIATE(replace)
UML_DECLARE_SIMD_IMMEDIATE(orr)
UML_DECLARE_SIMD_IMMEDIATE(bic)
#undef UML_DECLARE_SIMD_IMMEDIATE
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
#define UML_DECLARE_SIMD_SHIFT_SHAPES(name) \
	void uml_arm64_gadget_##name##_8b(); \
	void uml_arm64_gadget_##name##_4h(); \
	void uml_arm64_gadget_##name##_2s(); \
	void uml_arm64_gadget_##name##_16b(); \
	void uml_arm64_gadget_##name##_8h(); \
	void uml_arm64_gadget_##name##_4s(); \
	void uml_arm64_gadget_##name##_2d(); \
	void uml_arm64_gadget_##name##_d();
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_sshr)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_ushr)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_ssra)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_usra)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_srshr)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_urshr)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_srsra)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_ursra)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_sri)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_left_shl)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_left_sli)
#undef UML_DECLARE_SIMD_SHIFT_SHAPES
#define UML_DECLARE_SIMD_SAT_SHIFT_LEFT(name) \
	void uml_arm64_gadget_simd_shift_left_##name##_8b(); \
	void uml_arm64_gadget_simd_shift_left_##name##_4h(); \
	void uml_arm64_gadget_simd_shift_left_##name##_2s(); \
	void uml_arm64_gadget_simd_shift_left_##name##_16b(); \
	void uml_arm64_gadget_simd_shift_left_##name##_8h(); \
	void uml_arm64_gadget_simd_shift_left_##name##_4s(); \
	void uml_arm64_gadget_simd_shift_left_##name##_2d(); \
	void uml_arm64_gadget_simd_shift_left_##name##_b(); \
	void uml_arm64_gadget_simd_shift_left_##name##_h(); \
	void uml_arm64_gadget_simd_shift_left_##name##_s(); \
	void uml_arm64_gadget_simd_shift_left_##name##_d();
UML_DECLARE_SIMD_SAT_SHIFT_LEFT(sqshl)
UML_DECLARE_SIMD_SAT_SHIFT_LEFT(uqshl)
#undef UML_DECLARE_SIMD_SAT_SHIFT_LEFT
#define UML_SIMD_FOR_EACH_SHIFT_8(M, shape) \
	M(shape, 0) M(shape, 1) M(shape, 2) M(shape, 3) \
	M(shape, 4) M(shape, 5) M(shape, 6) M(shape, 7)
#define UML_SIMD_FOR_EACH_SHIFT_16(M, shape) \
	UML_SIMD_FOR_EACH_SHIFT_8(M, shape) \
	M(shape, 8) M(shape, 9) M(shape, 10) M(shape, 11) \
	M(shape, 12) M(shape, 13) M(shape, 14) M(shape, 15)
#define UML_SIMD_FOR_EACH_SHIFT_32(M, shape) \
	UML_SIMD_FOR_EACH_SHIFT_16(M, shape) \
	M(shape, 16) M(shape, 17) M(shape, 18) M(shape, 19) \
	M(shape, 20) M(shape, 21) M(shape, 22) M(shape, 23) \
	M(shape, 24) M(shape, 25) M(shape, 26) M(shape, 27) \
	M(shape, 28) M(shape, 29) M(shape, 30) M(shape, 31)
#define UML_SIMD_FOR_EACH_SHIFT_64(M, shape) \
	UML_SIMD_FOR_EACH_SHIFT_32(M, shape) \
	M(shape, 32) M(shape, 33) M(shape, 34) M(shape, 35) \
	M(shape, 36) M(shape, 37) M(shape, 38) M(shape, 39) \
	M(shape, 40) M(shape, 41) M(shape, 42) M(shape, 43) \
	M(shape, 44) M(shape, 45) M(shape, 46) M(shape, 47) \
	M(shape, 48) M(shape, 49) M(shape, 50) M(shape, 51) \
	M(shape, 52) M(shape, 53) M(shape, 54) M(shape, 55) \
	M(shape, 56) M(shape, 57) M(shape, 58) M(shape, 59) \
	M(shape, 60) M(shape, 61) M(shape, 62) M(shape, 63)
#define UML_DECLARE_SIMD_SQSHLU(shape, shift) \
	void uml_arm64_gadget_simd_shift_left_sqshlu_##shape##_##shift();
UML_SIMD_FOR_EACH_SHIFT_8(UML_DECLARE_SIMD_SQSHLU, 8b)
UML_SIMD_FOR_EACH_SHIFT_16(UML_DECLARE_SIMD_SQSHLU, 4h)
UML_SIMD_FOR_EACH_SHIFT_32(UML_DECLARE_SIMD_SQSHLU, 2s)
UML_SIMD_FOR_EACH_SHIFT_8(UML_DECLARE_SIMD_SQSHLU, 16b)
UML_SIMD_FOR_EACH_SHIFT_16(UML_DECLARE_SIMD_SQSHLU, 8h)
UML_SIMD_FOR_EACH_SHIFT_32(UML_DECLARE_SIMD_SQSHLU, 4s)
UML_SIMD_FOR_EACH_SHIFT_64(UML_DECLARE_SIMD_SQSHLU, 2d)
UML_SIMD_FOR_EACH_SHIFT_8(UML_DECLARE_SIMD_SQSHLU, b)
UML_SIMD_FOR_EACH_SHIFT_16(UML_DECLARE_SIMD_SQSHLU, h)
UML_SIMD_FOR_EACH_SHIFT_32(UML_DECLARE_SIMD_SQSHLU, s)
UML_SIMD_FOR_EACH_SHIFT_64(UML_DECLARE_SIMD_SQSHLU, d)
#undef UML_DECLARE_SIMD_SQSHLU
#define UML_DECLARE_SIMD_SHIFT_NARROW(source, destination) \
	void uml_arm64_gadget_simd_shrn_##source##_to_##destination(); \
	void uml_arm64_gadget_simd_shrn2_##source##_to_##destination(); \
	void uml_arm64_gadget_simd_rshrn_##source##_to_##destination(); \
	void uml_arm64_gadget_simd_rshrn2_##source##_to_##destination();
UML_DECLARE_SIMD_SHIFT_NARROW(8h, 8b)
UML_DECLARE_SIMD_SHIFT_NARROW(4s, 4h)
UML_DECLARE_SIMD_SHIFT_NARROW(2d, 2s)
#undef UML_DECLARE_SIMD_SHIFT_NARROW
#define UML_DECLARE_SIMD_SAT_NARROW_SHAPE(operation, source, destination) \
	void uml_arm64_gadget_simd_##operation##_##source##_##destination##_low(); \
	void uml_arm64_gadget_simd_##operation##_##source##_##destination##_high(); \
	void uml_arm64_gadget_simd_##operation##_##source##_##destination##_scalar();
#define UML_DECLARE_SIMD_SAT_NARROW(operation) \
	UML_DECLARE_SIMD_SAT_NARROW_SHAPE(operation, h, b) \
	UML_DECLARE_SIMD_SAT_NARROW_SHAPE(operation, s, h) \
	UML_DECLARE_SIMD_SAT_NARROW_SHAPE(operation, d, s)
UML_DECLARE_SIMD_SAT_NARROW(sqshrn)
UML_DECLARE_SIMD_SAT_NARROW(sqrshrn)
UML_DECLARE_SIMD_SAT_NARROW(sqshrun)
UML_DECLARE_SIMD_SAT_NARROW(sqrshrun)
UML_DECLARE_SIMD_SAT_NARROW(uqshrn)
UML_DECLARE_SIMD_SAT_NARROW(uqrshrn)
#undef UML_DECLARE_SIMD_SAT_NARROW
#undef UML_DECLARE_SIMD_SAT_NARROW_SHAPE
#define UML_DECLARE_SIMD_SHIFT_LEFT_LONG(name) \
	void uml_arm64_gadget_simd_##name##_8h_8b(); \
	void uml_arm64_gadget_simd_##name##_4s_4h(); \
	void uml_arm64_gadget_simd_##name##_2d_2s();
UML_DECLARE_SIMD_SHIFT_LEFT_LONG(sshll)
UML_DECLARE_SIMD_SHIFT_LEFT_LONG(sshll2)
UML_DECLARE_SIMD_SHIFT_LEFT_LONG(ushll)
UML_DECLARE_SIMD_SHIFT_LEFT_LONG(ushll2)
#undef UML_DECLARE_SIMD_SHIFT_LEFT_LONG
#define UML_DECLARE_SIMD_TABLE_COUNT(operation, count) \
	void uml_arm64_gadget_simd_##operation##_##count##_8b(); \
	void uml_arm64_gadget_simd_##operation##_##count##_16b();
#define UML_DECLARE_SIMD_TABLE(operation) \
	UML_DECLARE_SIMD_TABLE_COUNT(operation, 1) \
	UML_DECLARE_SIMD_TABLE_COUNT(operation, 2) \
	UML_DECLARE_SIMD_TABLE_COUNT(operation, 3) \
	UML_DECLARE_SIMD_TABLE_COUNT(operation, 4)
UML_DECLARE_SIMD_TABLE(tbl)
UML_DECLARE_SIMD_TABLE(tbx)
#undef UML_DECLARE_SIMD_TABLE
#undef UML_DECLARE_SIMD_TABLE_COUNT
#define UML_DECLARE_SIMD_LUT_PART(name, part) \
	void uml_arm64_gadget_simd_lut_native_##name##_##part();
UML_DECLARE_SIMD_LUT_PART(luti2_b, 0)
UML_DECLARE_SIMD_LUT_PART(luti2_b, 1)
UML_DECLARE_SIMD_LUT_PART(luti2_b, 2)
UML_DECLARE_SIMD_LUT_PART(luti2_b, 3)
UML_DECLARE_SIMD_LUT_PART(luti2_h, 0)
UML_DECLARE_SIMD_LUT_PART(luti2_h, 1)
UML_DECLARE_SIMD_LUT_PART(luti2_h, 2)
UML_DECLARE_SIMD_LUT_PART(luti2_h, 3)
UML_DECLARE_SIMD_LUT_PART(luti2_h, 4)
UML_DECLARE_SIMD_LUT_PART(luti2_h, 5)
UML_DECLARE_SIMD_LUT_PART(luti2_h, 6)
UML_DECLARE_SIMD_LUT_PART(luti2_h, 7)
UML_DECLARE_SIMD_LUT_PART(luti4_b, 0)
UML_DECLARE_SIMD_LUT_PART(luti4_b, 1)
UML_DECLARE_SIMD_LUT_PART(luti4_h, 0)
UML_DECLARE_SIMD_LUT_PART(luti4_h, 1)
UML_DECLARE_SIMD_LUT_PART(luti4_h, 2)
UML_DECLARE_SIMD_LUT_PART(luti4_h, 3)
#undef UML_DECLARE_SIMD_LUT_PART
void uml_arm64_gadget_simd_lut_software();
#define UML_DECLARE_SIMD_PERMUTE(name) \
	void uml_arm64_gadget_simd_permute_##name##_8b(); \
	void uml_arm64_gadget_simd_permute_##name##_4h(); \
	void uml_arm64_gadget_simd_permute_##name##_2s(); \
	void uml_arm64_gadget_simd_permute_##name##_16b(); \
	void uml_arm64_gadget_simd_permute_##name##_8h(); \
	void uml_arm64_gadget_simd_permute_##name##_4s(); \
	void uml_arm64_gadget_simd_permute_##name##_2d();
UML_DECLARE_SIMD_PERMUTE(uzp1)
UML_DECLARE_SIMD_PERMUTE(trn1)
UML_DECLARE_SIMD_PERMUTE(zip1)
UML_DECLARE_SIMD_PERMUTE(uzp2)
UML_DECLARE_SIMD_PERMUTE(trn2)
UML_DECLARE_SIMD_PERMUTE(zip2)
#undef UML_DECLARE_SIMD_PERMUTE
#define UML_DECLARE_SIMD_DUPLICATE_ELEMENT(name) \
	void uml_arm64_gadget_simd_duplicate_element_##name();
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(8b)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(16b)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(4h)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(8h)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(2s)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(4s)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(2d)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(b)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(h)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(s)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(d)
#undef UML_DECLARE_SIMD_DUPLICATE_ELEMENT
#define UML_DECLARE_SIMD_DUPLICATE_GENERAL(name) \
	void uml_arm64_gadget_simd_duplicate_general_##name();
UML_DECLARE_SIMD_DUPLICATE_GENERAL(8b)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(16b)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(4h)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(8h)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(2s)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(4s)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(2d)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(zero_8b)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(zero_16b)
#undef UML_DECLARE_SIMD_DUPLICATE_GENERAL
#define UML_DECLARE_SIMD_INSERT_GENERAL(size) \
	void uml_arm64_gadget_simd_insert_general_##size(); \
	void uml_arm64_gadget_simd_insert_general_zero_##size();
UML_DECLARE_SIMD_INSERT_GENERAL(b)
UML_DECLARE_SIMD_INSERT_GENERAL(h)
UML_DECLARE_SIMD_INSERT_GENERAL(s)
UML_DECLARE_SIMD_INSERT_GENERAL(d)
#undef UML_DECLARE_SIMD_INSERT_GENERAL
#define UML_DECLARE_SIMD_INSERT_ELEMENT(size) \
	void uml_arm64_gadget_simd_insert_element_##size();
UML_DECLARE_SIMD_INSERT_ELEMENT(b)
UML_DECLARE_SIMD_INSERT_ELEMENT(h)
UML_DECLARE_SIMD_INSERT_ELEMENT(s)
UML_DECLARE_SIMD_INSERT_ELEMENT(d)
#undef UML_DECLARE_SIMD_INSERT_ELEMENT
#define UML_DECLARE_SIMD_LANE_TO_GPR(name) \
	void uml_arm64_gadget_simd_lane_to_gpr_##name();
UML_DECLARE_SIMD_LANE_TO_GPR(smov_w_b)
UML_DECLARE_SIMD_LANE_TO_GPR(smov_w_h)
UML_DECLARE_SIMD_LANE_TO_GPR(smov_x_b)
UML_DECLARE_SIMD_LANE_TO_GPR(smov_x_h)
UML_DECLARE_SIMD_LANE_TO_GPR(smov_x_s)
UML_DECLARE_SIMD_LANE_TO_GPR(umov_w_b)
UML_DECLARE_SIMD_LANE_TO_GPR(umov_w_h)
UML_DECLARE_SIMD_LANE_TO_GPR(umov_w_s)
UML_DECLARE_SIMD_LANE_TO_GPR(umov_x_d)
UML_DECLARE_SIMD_LANE_TO_GPR(discard)
#undef UML_DECLARE_SIMD_LANE_TO_GPR
#define UML_DECLARE_SIMD_FLOATING_SIGN(name) \
	void uml_arm64_gadget_simd_floating_##name();
UML_DECLARE_SIMD_FLOATING_SIGN(fabs_4h)
UML_DECLARE_SIMD_FLOATING_SIGN(fabs_8h)
UML_DECLARE_SIMD_FLOATING_SIGN(fabs_2s)
UML_DECLARE_SIMD_FLOATING_SIGN(fabs_4s)
UML_DECLARE_SIMD_FLOATING_SIGN(fabs_2d)
UML_DECLARE_SIMD_FLOATING_SIGN(fneg_4h)
UML_DECLARE_SIMD_FLOATING_SIGN(fneg_8h)
UML_DECLARE_SIMD_FLOATING_SIGN(fneg_2s)
UML_DECLARE_SIMD_FLOATING_SIGN(fneg_4s)
UML_DECLARE_SIMD_FLOATING_SIGN(fneg_2d)
#undef UML_DECLARE_SIMD_FLOATING_SIGN
void uml_arm64_gadget_simd_addp_scalar();
void uml_arm64_gadget_simd_aese_native();
void uml_arm64_gadget_simd_aese_soft();
void uml_arm64_gadget_simd_aesd_native();
void uml_arm64_gadget_simd_aesd_soft();
void uml_arm64_gadget_simd_aesmc_native();
void uml_arm64_gadget_simd_aesmc_soft();
void uml_arm64_gadget_simd_aesimc_native();
void uml_arm64_gadget_simd_aesimc_soft();
void uml_arm64_gadget_simd_sha1c();
void uml_arm64_gadget_simd_sha1c_soft();
void uml_arm64_gadget_simd_sha1p();
void uml_arm64_gadget_simd_sha1p_soft();
void uml_arm64_gadget_simd_sha1m();
void uml_arm64_gadget_simd_sha1m_soft();
void uml_arm64_gadget_simd_sha1su0();
void uml_arm64_gadget_simd_sha1su0_soft();
void uml_arm64_gadget_simd_sha256h();
void uml_arm64_gadget_simd_sha256h_soft();
void uml_arm64_gadget_simd_sha256h2();
void uml_arm64_gadget_simd_sha256h2_soft();
void uml_arm64_gadget_simd_sha256su1();
void uml_arm64_gadget_simd_sha256su1_soft();
void uml_arm64_gadget_simd_sha1h();
void uml_arm64_gadget_simd_sha1h_soft();
void uml_arm64_gadget_simd_sha1su1();
void uml_arm64_gadget_simd_sha1su1_soft();
void uml_arm64_gadget_simd_sha256su0();
void uml_arm64_gadget_simd_sha256su0_soft();
void uml_arm64_gadget_simd_sha512h();
void uml_arm64_gadget_simd_sha512h_soft();
void uml_arm64_gadget_simd_sha512h2();
void uml_arm64_gadget_simd_sha512h2_soft();
void uml_arm64_gadget_simd_sha512su0();
void uml_arm64_gadget_simd_sha512su0_soft();
void uml_arm64_gadget_simd_sha512su1();
void uml_arm64_gadget_simd_sha512su1_soft();
void uml_arm64_gadget_simd_sm3partw1();
void uml_arm64_gadget_simd_sm3partw2();
void uml_arm64_gadget_simd_sm3ss1();
void uml_arm64_gadget_simd_sm3partw1_soft();
void uml_arm64_gadget_simd_sm3partw2_soft();
void uml_arm64_gadget_simd_sm3ss1_soft();
#define UML_DECLARE_SIMD_SM3TT(name) \
	void uml_arm64_gadget_simd_##name##_0(); \
	void uml_arm64_gadget_simd_##name##_1(); \
	void uml_arm64_gadget_simd_##name##_2(); \
	void uml_arm64_gadget_simd_##name##_3(); \
	void uml_arm64_gadget_simd_##name##_0_soft(); \
	void uml_arm64_gadget_simd_##name##_1_soft(); \
	void uml_arm64_gadget_simd_##name##_2_soft(); \
	void uml_arm64_gadget_simd_##name##_3_soft();
UML_DECLARE_SIMD_SM3TT(sm3tt1a)
UML_DECLARE_SIMD_SM3TT(sm3tt1b)
UML_DECLARE_SIMD_SM3TT(sm3tt2a)
UML_DECLARE_SIMD_SM3TT(sm3tt2b)
#undef UML_DECLARE_SIMD_SM3TT
void uml_arm64_gadget_simd_sm4e();
void uml_arm64_gadget_simd_sm4ekey();
void uml_arm64_gadget_simd_sm4e_soft();
void uml_arm64_gadget_simd_sm4ekey_soft();
#define UML_DECLARE_SIMD_INDEXED_ONE(name, shape, index) \
	void uml_arm64_gadget_simd_indexed_##name##_##shape##_##index();
#define UML_DECLARE_SIMD_INDEXED_4(name, shape) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 0) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 1) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 2) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 3)
#define UML_DECLARE_SIMD_INDEXED_2(name, shape) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 0) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 1)
#define UML_DECLARE_SIMD_INDEXED_8(name, shape) \
	UML_DECLARE_SIMD_INDEXED_4(name, shape) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 4) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 5) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 6) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 7)
#define UML_DECLARE_SIMD_INDEXED_SAME(name) \
	UML_DECLARE_SIMD_INDEXED_8(name, 4h) \
	UML_DECLARE_SIMD_INDEXED_8(name, 8h) \
	UML_DECLARE_SIMD_INDEXED_4(name, 2s) \
	UML_DECLARE_SIMD_INDEXED_4(name, 4s)
UML_DECLARE_SIMD_INDEXED_SAME(mla)
UML_DECLARE_SIMD_INDEXED_SAME(mls)
UML_DECLARE_SIMD_INDEXED_SAME(mul)
#undef UML_DECLARE_SIMD_INDEXED_SAME
#define UML_DECLARE_SIMD_INDEXED_LONG(name) \
	UML_DECLARE_SIMD_INDEXED_8(name, h_low) \
	UML_DECLARE_SIMD_INDEXED_8(name, h_high) \
	UML_DECLARE_SIMD_INDEXED_4(name, s_low) \
	UML_DECLARE_SIMD_INDEXED_4(name, s_high)
UML_DECLARE_SIMD_INDEXED_LONG(smlal)
UML_DECLARE_SIMD_INDEXED_LONG(smlsl)
UML_DECLARE_SIMD_INDEXED_LONG(smull)
UML_DECLARE_SIMD_INDEXED_LONG(umlal)
UML_DECLARE_SIMD_INDEXED_LONG(umlsl)
UML_DECLARE_SIMD_INDEXED_LONG(umull)
#undef UML_DECLARE_SIMD_INDEXED_LONG
#define UML_DECLARE_SIMD_INDEXED_SATURATING(name) \
	UML_DECLARE_SIMD_INDEXED_8(name, h_low) \
	UML_DECLARE_SIMD_INDEXED_8(name, h_high) \
	UML_DECLARE_SIMD_INDEXED_8(name, h_scalar) \
	UML_DECLARE_SIMD_INDEXED_4(name, s_low) \
	UML_DECLARE_SIMD_INDEXED_4(name, s_high) \
	UML_DECLARE_SIMD_INDEXED_4(name, s_scalar)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqdmlal)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqdmlsl)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqdmull)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqdmulh)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqrdmulh)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqrdmlah)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqrdmlsh)
#undef UML_DECLARE_SIMD_INDEXED_SATURATING
#define UML_DECLARE_SIMD_INDEXED_FLOATING(name) \
	UML_DECLARE_SIMD_INDEXED_8(name, h) \
	UML_DECLARE_SIMD_INDEXED_8(name, 4h) \
	UML_DECLARE_SIMD_INDEXED_8(name, 8h) \
	UML_DECLARE_SIMD_INDEXED_4(name, s) \
	UML_DECLARE_SIMD_INDEXED_4(name, 2s) \
	UML_DECLARE_SIMD_INDEXED_4(name, 4s) \
	UML_DECLARE_SIMD_INDEXED_2(name, d) \
	UML_DECLARE_SIMD_INDEXED_2(name, 2d)
UML_DECLARE_SIMD_INDEXED_FLOATING(fmla)
UML_DECLARE_SIMD_INDEXED_FLOATING(fmls)
UML_DECLARE_SIMD_INDEXED_FLOATING(fmul)
UML_DECLARE_SIMD_INDEXED_FLOATING(fmulx)
UML_DECLARE_SIMD_INDEXED_FLOATING(fmla_checked)
UML_DECLARE_SIMD_INDEXED_FLOATING(fmls_checked)
UML_DECLARE_SIMD_INDEXED_FLOATING(fmul_checked)
UML_DECLARE_SIMD_INDEXED_FLOATING(fmulx_checked)
#undef UML_DECLARE_SIMD_INDEXED_FLOATING
void uml_arm64_gadget_simd_fmulx_h();
void uml_arm64_gadget_simd_fmulx_4h();
void uml_arm64_gadget_simd_fmulx_8h();
void uml_arm64_gadget_simd_fmulx_s();
void uml_arm64_gadget_simd_fmulx_2s();
void uml_arm64_gadget_simd_fmulx_4s();
void uml_arm64_gadget_simd_fmulx_d();
void uml_arm64_gadget_simd_fmulx_2d();
#undef UML_DECLARE_SIMD_INDEXED_8
#undef UML_DECLARE_SIMD_INDEXED_4
#undef UML_DECLARE_SIMD_INDEXED_2
#undef UML_DECLARE_SIMD_INDEXED_ONE
void uml_arm64_gadget_simd_eor3_native();
void uml_arm64_gadget_simd_eor3_soft();
void uml_arm64_gadget_simd_bcax_native();
void uml_arm64_gadget_simd_bcax_soft();
void uml_arm64_gadget_simd_rax1_native();
void uml_arm64_gadget_simd_rax1_soft();
void uml_arm64_gadget_simd_pmull_8b_direct();
void uml_arm64_gadget_simd_pmull_8b2_direct();
void uml_arm64_gadget_simd_pmull_1d_native_direct();
void uml_arm64_gadget_simd_pmull_1d2_native_direct();
void uml_arm64_gadget_simd_pmull_1d_soft_direct();
void uml_arm64_gadget_simd_pmull_1d2_soft_direct();
#define UML_DECLARE_SIMD_XAR(rotate) \
	void uml_arm64_gadget_simd_xar_native_##rotate(); \
	void uml_arm64_gadget_simd_xar_soft_##rotate();
UML_DECLARE_SIMD_XAR(0) UML_DECLARE_SIMD_XAR(1)
UML_DECLARE_SIMD_XAR(2) UML_DECLARE_SIMD_XAR(3)
UML_DECLARE_SIMD_XAR(4) UML_DECLARE_SIMD_XAR(5)
UML_DECLARE_SIMD_XAR(6) UML_DECLARE_SIMD_XAR(7)
UML_DECLARE_SIMD_XAR(8) UML_DECLARE_SIMD_XAR(9)
UML_DECLARE_SIMD_XAR(10) UML_DECLARE_SIMD_XAR(11)
UML_DECLARE_SIMD_XAR(12) UML_DECLARE_SIMD_XAR(13)
UML_DECLARE_SIMD_XAR(14) UML_DECLARE_SIMD_XAR(15)
UML_DECLARE_SIMD_XAR(16) UML_DECLARE_SIMD_XAR(17)
UML_DECLARE_SIMD_XAR(18) UML_DECLARE_SIMD_XAR(19)
UML_DECLARE_SIMD_XAR(20) UML_DECLARE_SIMD_XAR(21)
UML_DECLARE_SIMD_XAR(22) UML_DECLARE_SIMD_XAR(23)
UML_DECLARE_SIMD_XAR(24) UML_DECLARE_SIMD_XAR(25)
UML_DECLARE_SIMD_XAR(26) UML_DECLARE_SIMD_XAR(27)
UML_DECLARE_SIMD_XAR(28) UML_DECLARE_SIMD_XAR(29)
UML_DECLARE_SIMD_XAR(30) UML_DECLARE_SIMD_XAR(31)
UML_DECLARE_SIMD_XAR(32) UML_DECLARE_SIMD_XAR(33)
UML_DECLARE_SIMD_XAR(34) UML_DECLARE_SIMD_XAR(35)
UML_DECLARE_SIMD_XAR(36) UML_DECLARE_SIMD_XAR(37)
UML_DECLARE_SIMD_XAR(38) UML_DECLARE_SIMD_XAR(39)
UML_DECLARE_SIMD_XAR(40) UML_DECLARE_SIMD_XAR(41)
UML_DECLARE_SIMD_XAR(42) UML_DECLARE_SIMD_XAR(43)
UML_DECLARE_SIMD_XAR(44) UML_DECLARE_SIMD_XAR(45)
UML_DECLARE_SIMD_XAR(46) UML_DECLARE_SIMD_XAR(47)
UML_DECLARE_SIMD_XAR(48) UML_DECLARE_SIMD_XAR(49)
UML_DECLARE_SIMD_XAR(50) UML_DECLARE_SIMD_XAR(51)
UML_DECLARE_SIMD_XAR(52) UML_DECLARE_SIMD_XAR(53)
UML_DECLARE_SIMD_XAR(54) UML_DECLARE_SIMD_XAR(55)
UML_DECLARE_SIMD_XAR(56) UML_DECLARE_SIMD_XAR(57)
UML_DECLARE_SIMD_XAR(58) UML_DECLARE_SIMD_XAR(59)
UML_DECLARE_SIMD_XAR(60) UML_DECLARE_SIMD_XAR(61)
UML_DECLARE_SIMD_XAR(62) UML_DECLARE_SIMD_XAR(63)
#undef UML_DECLARE_SIMD_XAR
#define UML_DECLARE_SIMD_THREE_SAME_V6(name) \
	void uml_arm64_gadget_simd_three_same_##name##_8b(); \
	void uml_arm64_gadget_simd_three_same_##name##_4h(); \
	void uml_arm64_gadget_simd_three_same_##name##_2s(); \
	void uml_arm64_gadget_simd_three_same_##name##_16b(); \
	void uml_arm64_gadget_simd_three_same_##name##_8h(); \
	void uml_arm64_gadget_simd_three_same_##name##_4s();
#define UML_DECLARE_SIMD_THREE_SAME_V7(name) \
	UML_DECLARE_SIMD_THREE_SAME_V6(name) \
	void uml_arm64_gadget_simd_three_same_##name##_2d();
#define UML_DECLARE_SIMD_THREE_SAME_S4(name) \
	void uml_arm64_gadget_simd_three_same_##name##_b(); \
	void uml_arm64_gadget_simd_three_same_##name##_h(); \
	void uml_arm64_gadget_simd_three_same_##name##_s(); \
	void uml_arm64_gadget_simd_three_same_##name##_d();
UML_DECLARE_SIMD_THREE_SAME_V6(shadd)
UML_DECLARE_SIMD_THREE_SAME_V6(uhadd)
UML_DECLARE_SIMD_THREE_SAME_V6(srhadd)
UML_DECLARE_SIMD_THREE_SAME_V6(urhadd)
UML_DECLARE_SIMD_THREE_SAME_V6(shsub)
UML_DECLARE_SIMD_THREE_SAME_V6(uhsub)
UML_DECLARE_SIMD_THREE_SAME_V7(sqadd)
UML_DECLARE_SIMD_THREE_SAME_S4(sqadd)
UML_DECLARE_SIMD_THREE_SAME_V7(uqadd)
UML_DECLARE_SIMD_THREE_SAME_S4(uqadd)
UML_DECLARE_SIMD_THREE_SAME_V7(sqsub)
UML_DECLARE_SIMD_THREE_SAME_S4(sqsub)
UML_DECLARE_SIMD_THREE_SAME_V7(uqsub)
UML_DECLARE_SIMD_THREE_SAME_S4(uqsub)
UML_DECLARE_SIMD_THREE_SAME_V7(sshl)
void uml_arm64_gadget_simd_three_same_sshl_d();
UML_DECLARE_SIMD_THREE_SAME_V7(ushl)
void uml_arm64_gadget_simd_three_same_ushl_d();
UML_DECLARE_SIMD_THREE_SAME_V7(sqshl)
UML_DECLARE_SIMD_THREE_SAME_S4(sqshl)
UML_DECLARE_SIMD_THREE_SAME_V7(uqshl)
UML_DECLARE_SIMD_THREE_SAME_S4(uqshl)
UML_DECLARE_SIMD_THREE_SAME_V7(srshl)
void uml_arm64_gadget_simd_three_same_srshl_d();
UML_DECLARE_SIMD_THREE_SAME_V7(urshl)
void uml_arm64_gadget_simd_three_same_urshl_d();
UML_DECLARE_SIMD_THREE_SAME_V7(sqrshl)
UML_DECLARE_SIMD_THREE_SAME_S4(sqrshl)
UML_DECLARE_SIMD_THREE_SAME_V7(uqrshl)
UML_DECLARE_SIMD_THREE_SAME_S4(uqrshl)
UML_DECLARE_SIMD_THREE_SAME_V7(cmgt)
void uml_arm64_gadget_simd_three_same_cmgt_d();
UML_DECLARE_SIMD_THREE_SAME_V7(cmhi)
void uml_arm64_gadget_simd_three_same_cmhi_d();
UML_DECLARE_SIMD_THREE_SAME_V7(cmge)
void uml_arm64_gadget_simd_three_same_cmge_d();
UML_DECLARE_SIMD_THREE_SAME_V7(cmhs)
void uml_arm64_gadget_simd_three_same_cmhs_d();
UML_DECLARE_SIMD_THREE_SAME_V6(smax)
UML_DECLARE_SIMD_THREE_SAME_V6(umax)
UML_DECLARE_SIMD_THREE_SAME_V6(smin)
UML_DECLARE_SIMD_THREE_SAME_V6(umin)
UML_DECLARE_SIMD_THREE_SAME_V6(sabd)
UML_DECLARE_SIMD_THREE_SAME_V6(uabd)
UML_DECLARE_SIMD_THREE_SAME_V6(saba)
UML_DECLARE_SIMD_THREE_SAME_V6(uaba)
UML_DECLARE_SIMD_THREE_SAME_V7(add)
void uml_arm64_gadget_simd_three_same_add_d();
UML_DECLARE_SIMD_THREE_SAME_V7(sub)
void uml_arm64_gadget_simd_three_same_sub_d();
UML_DECLARE_SIMD_THREE_SAME_V7(cmtst)
void uml_arm64_gadget_simd_three_same_cmtst_d();
UML_DECLARE_SIMD_THREE_SAME_V7(cmeq)
void uml_arm64_gadget_simd_three_same_cmeq_d();
UML_DECLARE_SIMD_THREE_SAME_V6(mla)
UML_DECLARE_SIMD_THREE_SAME_V6(mls)
UML_DECLARE_SIMD_THREE_SAME_V6(mul)
void uml_arm64_gadget_simd_three_same_pmul_8b();
void uml_arm64_gadget_simd_three_same_pmul_16b();
UML_DECLARE_SIMD_THREE_SAME_V6(smaxp)
UML_DECLARE_SIMD_THREE_SAME_V6(umaxp)
UML_DECLARE_SIMD_THREE_SAME_V6(sminp)
UML_DECLARE_SIMD_THREE_SAME_V6(uminp)
UML_DECLARE_SIMD_THREE_SAME_V7(addp)
void uml_arm64_gadget_simd_three_same_sqdmulh_4h();
void uml_arm64_gadget_simd_three_same_sqdmulh_2s();
void uml_arm64_gadget_simd_three_same_sqdmulh_8h();
void uml_arm64_gadget_simd_three_same_sqdmulh_4s();
void uml_arm64_gadget_simd_three_same_sqdmulh_h();
void uml_arm64_gadget_simd_three_same_sqdmulh_s();
void uml_arm64_gadget_simd_three_same_sqrdmulh_4h();
void uml_arm64_gadget_simd_three_same_sqrdmulh_2s();
void uml_arm64_gadget_simd_three_same_sqrdmulh_8h();
void uml_arm64_gadget_simd_three_same_sqrdmulh_4s();
void uml_arm64_gadget_simd_three_same_sqrdmulh_h();
void uml_arm64_gadget_simd_three_same_sqrdmulh_s();
#define UML_DECLARE_SIMD_RDM(name) \
	void uml_arm64_gadget_simd_three_same_##name##_4h(); \
	void uml_arm64_gadget_simd_three_same_##name##_2s(); \
	void uml_arm64_gadget_simd_three_same_##name##_8h(); \
	void uml_arm64_gadget_simd_three_same_##name##_4s(); \
	void uml_arm64_gadget_simd_three_same_##name##_h(); \
	void uml_arm64_gadget_simd_three_same_##name##_s();
UML_DECLARE_SIMD_RDM(sqrdmlah)
UML_DECLARE_SIMD_RDM(sqrdmlsh)
#undef UML_DECLARE_SIMD_RDM
void uml_arm64_gadget_simd_rdm_software();
void uml_arm64_gadget_simd_dot_vector_sdot_2s();
void uml_arm64_gadget_simd_dot_vector_sdot_4s();
void uml_arm64_gadget_simd_dot_vector_udot_2s();
void uml_arm64_gadget_simd_dot_vector_udot_4s();
void uml_arm64_gadget_simd_dot_vector_usdot_2s();
void uml_arm64_gadget_simd_dot_vector_usdot_4s();
void uml_arm64_gadget_simd_dot_vector_sdot_2s_soft();
void uml_arm64_gadget_simd_dot_vector_sdot_4s_soft();
void uml_arm64_gadget_simd_dot_vector_udot_2s_soft();
void uml_arm64_gadget_simd_dot_vector_udot_4s_soft();
void uml_arm64_gadget_simd_dot_vector_usdot_2s_soft();
void uml_arm64_gadget_simd_dot_vector_usdot_4s_soft();
void uml_arm64_gadget_simd_matrix_smmla();
void uml_arm64_gadget_simd_matrix_ummla();
void uml_arm64_gadget_simd_matrix_usmmla();
void uml_arm64_gadget_simd_matrix_smmla_soft();
void uml_arm64_gadget_simd_matrix_ummla_soft();
void uml_arm64_gadget_simd_matrix_usmmla_soft();
#define UML_DECLARE_SIMD_DOT_INDEXED_SHAPE(name, shape) \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_0(); \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_1(); \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_2(); \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_3();
#define UML_DECLARE_SIMD_DOT_INDEXED(name) \
	UML_DECLARE_SIMD_DOT_INDEXED_SHAPE(name, 2s) \
	UML_DECLARE_SIMD_DOT_INDEXED_SHAPE(name, 4s)
UML_DECLARE_SIMD_DOT_INDEXED(sdot)
UML_DECLARE_SIMD_DOT_INDEXED(udot)
UML_DECLARE_SIMD_DOT_INDEXED(sudot)
UML_DECLARE_SIMD_DOT_INDEXED(usdot)
#define UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(name, shape) \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_0_soft(); \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_1_soft(); \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_2_soft(); \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_3_soft();
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(sdot, 2s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(sdot, 4s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(udot, 2s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(udot, 4s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(sudot, 2s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(sudot, 4s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(usdot, 2s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(usdot, 4s)
#undef UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE
#undef UML_DECLARE_SIMD_DOT_INDEXED
#undef UML_DECLARE_SIMD_DOT_INDEXED_SHAPE
#undef UML_DECLARE_SIMD_THREE_SAME_S4
#undef UML_DECLARE_SIMD_THREE_SAME_V7
#undef UML_DECLARE_SIMD_THREE_SAME_V6
#define UML_DECLARE_SIMD_WIDENING_V6(name) \
	void uml_arm64_gadget_simd_widening_##name##_8b(); \
	void uml_arm64_gadget_simd_widening_##name##_4h(); \
	void uml_arm64_gadget_simd_widening_##name##_2s(); \
	void uml_arm64_gadget_simd_widening_##name##_16b(); \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s();
#define UML_DECLARE_SIMD_HIGH_NARROW(name) \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s(); \
	void uml_arm64_gadget_simd_widening_##name##_2d(); \
	void uml_arm64_gadget_simd_widening_##name##_8h2(); \
	void uml_arm64_gadget_simd_widening_##name##_4s2(); \
	void uml_arm64_gadget_simd_widening_##name##_2d2();
#define UML_DECLARE_SIMD_SATURATING_LONG(name) \
	void uml_arm64_gadget_simd_widening_##name##_4h(); \
	void uml_arm64_gadget_simd_widening_##name##_2s(); \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s(); \
	void uml_arm64_gadget_simd_widening_##name##_h(); \
	void uml_arm64_gadget_simd_widening_##name##_s();
UML_DECLARE_SIMD_WIDENING_V6(saddl)
UML_DECLARE_SIMD_WIDENING_V6(uaddl)
UML_DECLARE_SIMD_WIDENING_V6(saddw)
UML_DECLARE_SIMD_WIDENING_V6(uaddw)
UML_DECLARE_SIMD_WIDENING_V6(ssubl)
UML_DECLARE_SIMD_WIDENING_V6(usubl)
UML_DECLARE_SIMD_WIDENING_V6(ssubw)
UML_DECLARE_SIMD_WIDENING_V6(usubw)
UML_DECLARE_SIMD_HIGH_NARROW(addhn)
UML_DECLARE_SIMD_HIGH_NARROW(raddhn)
UML_DECLARE_SIMD_HIGH_NARROW(subhn)
UML_DECLARE_SIMD_HIGH_NARROW(rsubhn)
UML_DECLARE_SIMD_WIDENING_V6(sabdl)
UML_DECLARE_SIMD_WIDENING_V6(uabdl)
UML_DECLARE_SIMD_WIDENING_V6(sabal)
UML_DECLARE_SIMD_WIDENING_V6(uabal)
UML_DECLARE_SIMD_WIDENING_V6(smlal)
UML_DECLARE_SIMD_WIDENING_V6(umlal)
UML_DECLARE_SIMD_WIDENING_V6(smlsl)
UML_DECLARE_SIMD_WIDENING_V6(umlsl)
UML_DECLARE_SIMD_WIDENING_V6(smull)
UML_DECLARE_SIMD_WIDENING_V6(umull)
UML_DECLARE_SIMD_SATURATING_LONG(sqdmlal)
UML_DECLARE_SIMD_SATURATING_LONG(sqdmlsl)
UML_DECLARE_SIMD_SATURATING_LONG(sqdmull)
#undef UML_DECLARE_SIMD_SATURATING_LONG
#undef UML_DECLARE_SIMD_HIGH_NARROW
#undef UML_DECLARE_SIMD_WIDENING_V6
#define UML_DECLARE_SIMD_UNARY_V6(name) \
	void uml_arm64_gadget_simd_unary_##name##_8b(); \
	void uml_arm64_gadget_simd_unary_##name##_4h(); \
	void uml_arm64_gadget_simd_unary_##name##_2s(); \
	void uml_arm64_gadget_simd_unary_##name##_16b(); \
	void uml_arm64_gadget_simd_unary_##name##_8h(); \
	void uml_arm64_gadget_simd_unary_##name##_4s();
#define UML_DECLARE_SIMD_UNARY_V7(name) \
	UML_DECLARE_SIMD_UNARY_V6(name) \
	void uml_arm64_gadget_simd_unary_##name##_2d();
#define UML_DECLARE_SIMD_UNARY_S4(name) \
	void uml_arm64_gadget_simd_unary_##name##_b(); \
	void uml_arm64_gadget_simd_unary_##name##_h(); \
	void uml_arm64_gadget_simd_unary_##name##_s(); \
	void uml_arm64_gadget_simd_unary_##name##_d();
#define UML_DECLARE_SIMD_NARROW(name) \
	void uml_arm64_gadget_simd_unary_##name##_8h(); \
	void uml_arm64_gadget_simd_unary_##name##_4s(); \
	void uml_arm64_gadget_simd_unary_##name##_2d(); \
	void uml_arm64_gadget_simd_unary_##name##_8h2(); \
	void uml_arm64_gadget_simd_unary_##name##_4s2(); \
	void uml_arm64_gadget_simd_unary_##name##_2d2();
UML_DECLARE_SIMD_UNARY_V6(saddlp)
UML_DECLARE_SIMD_UNARY_V6(uaddlp)
UML_DECLARE_SIMD_UNARY_V6(sadalp)
UML_DECLARE_SIMD_UNARY_V6(uadalp)
UML_DECLARE_SIMD_UNARY_V7(suqadd)
UML_DECLARE_SIMD_UNARY_S4(suqadd)
UML_DECLARE_SIMD_UNARY_V7(usqadd)
UML_DECLARE_SIMD_UNARY_S4(usqadd)
UML_DECLARE_SIMD_UNARY_V6(cls)
UML_DECLARE_SIMD_UNARY_V6(clz)
UML_DECLARE_SIMD_UNARY_V7(sqabs)
UML_DECLARE_SIMD_UNARY_S4(sqabs)
UML_DECLARE_SIMD_UNARY_V7(sqneg)
UML_DECLARE_SIMD_UNARY_S4(sqneg)
UML_DECLARE_SIMD_UNARY_V7(abs)
void uml_arm64_gadget_simd_unary_abs_d();
UML_DECLARE_SIMD_UNARY_V7(neg)
void uml_arm64_gadget_simd_unary_neg_d();
void uml_arm64_gadget_simd_unary_cnt_8b();
void uml_arm64_gadget_simd_unary_cnt_16b();
void uml_arm64_gadget_simd_unary_not_8b();
void uml_arm64_gadget_simd_unary_not_16b();
void uml_arm64_gadget_simd_unary_rbit_8b();
void uml_arm64_gadget_simd_unary_rbit_16b();
UML_DECLARE_SIMD_UNARY_V7(cmgt0)
void uml_arm64_gadget_simd_unary_cmgt0_d();
UML_DECLARE_SIMD_UNARY_V7(cmge0)
void uml_arm64_gadget_simd_unary_cmge0_d();
UML_DECLARE_SIMD_UNARY_V7(cmeq0)
void uml_arm64_gadget_simd_unary_cmeq0_d();
UML_DECLARE_SIMD_UNARY_V7(cmle0)
void uml_arm64_gadget_simd_unary_cmle0_d();
UML_DECLARE_SIMD_UNARY_V7(cmlt0)
void uml_arm64_gadget_simd_unary_cmlt0_d();
UML_DECLARE_SIMD_NARROW(xtn)
UML_DECLARE_SIMD_NARROW(sqxtn)
UML_DECLARE_SIMD_NARROW(uqxtn)
UML_DECLARE_SIMD_NARROW(sqxtun)
void uml_arm64_gadget_simd_unary_sqxtn_b();
void uml_arm64_gadget_simd_unary_sqxtn_h();
void uml_arm64_gadget_simd_unary_sqxtn_s();
void uml_arm64_gadget_simd_unary_uqxtn_b();
void uml_arm64_gadget_simd_unary_uqxtn_h();
void uml_arm64_gadget_simd_unary_uqxtn_s();
void uml_arm64_gadget_simd_unary_sqxtun_b();
void uml_arm64_gadget_simd_unary_sqxtun_h();
void uml_arm64_gadget_simd_unary_sqxtun_s();
void uml_arm64_gadget_simd_unary_rev64_8b();
void uml_arm64_gadget_simd_unary_rev64_4h();
void uml_arm64_gadget_simd_unary_rev64_2s();
void uml_arm64_gadget_simd_unary_rev64_16b();
void uml_arm64_gadget_simd_unary_rev64_8h();
void uml_arm64_gadget_simd_unary_rev64_4s();
void uml_arm64_gadget_simd_unary_rev32_8b();
void uml_arm64_gadget_simd_unary_rev32_4h();
void uml_arm64_gadget_simd_unary_rev32_16b();
void uml_arm64_gadget_simd_unary_rev32_8h();
void uml_arm64_gadget_simd_unary_rev16_8b();
void uml_arm64_gadget_simd_unary_rev16_16b();
void uml_arm64_gadget_simd_unary_shll_8b();
void uml_arm64_gadget_simd_unary_shll_4h();
void uml_arm64_gadget_simd_unary_shll_2s();
void uml_arm64_gadget_simd_unary_shll_16b();
void uml_arm64_gadget_simd_unary_shll_8h();
void uml_arm64_gadget_simd_unary_shll_4s();
#define UML_DECLARE_SIMD_REDUCE_5(name) \
	void uml_arm64_gadget_simd_reduce_##name##_8b(); \
	void uml_arm64_gadget_simd_reduce_##name##_4h(); \
	void uml_arm64_gadget_simd_reduce_##name##_16b(); \
	void uml_arm64_gadget_simd_reduce_##name##_8h(); \
	void uml_arm64_gadget_simd_reduce_##name##_4s();
UML_DECLARE_SIMD_REDUCE_5(saddlv)
UML_DECLARE_SIMD_REDUCE_5(uaddlv)
UML_DECLARE_SIMD_REDUCE_5(smaxv)
UML_DECLARE_SIMD_REDUCE_5(umaxv)
UML_DECLARE_SIMD_REDUCE_5(sminv)
UML_DECLARE_SIMD_REDUCE_5(uminv)
UML_DECLARE_SIMD_REDUCE_5(addv)
#undef UML_DECLARE_SIMD_REDUCE_5
void uml_arm64_gadget_simd_unary_urecpe_2s();
void uml_arm64_gadget_simd_unary_urecpe_4s();
void uml_arm64_gadget_simd_unary_ursqrte_2s();
void uml_arm64_gadget_simd_unary_ursqrte_4s();
#undef UML_DECLARE_SIMD_NARROW
#undef UML_DECLARE_SIMD_UNARY_S4
#undef UML_DECLARE_SIMD_UNARY_V7
#undef UML_DECLARE_SIMD_UNARY_V6
void uml_arm64_gadget_vector_to_gpr_16_low();
void uml_arm64_gadget_gpr_to_vector_16_low();
void uml_arm64_gadget_gpr_to_vector_16_low_zero();
void uml_arm64_gadget_vector_to_gpr_32_low();
void uml_arm64_gadget_gpr_to_vector_32_low();
void uml_arm64_gadget_gpr_to_vector_32_low_zero();
void uml_arm64_gadget_vector_to_gpr_64_low();
void uml_arm64_gadget_gpr_to_vector_64_low();
void uml_arm64_gadget_gpr_to_vector_64_low_zero();
void uml_arm64_gadget_vector_to_gpr_64_high();
void uml_arm64_gadget_gpr_to_vector_64_high();
void uml_arm64_gadget_gpr_to_vector_64_high_zero();
#define UML_DECLARE_FLOATING_SELECT(name) \
	void uml_arm64_gadget_floating_select_##name##_h(); \
	void uml_arm64_gadget_floating_select_##name##_s(); \
	void uml_arm64_gadget_floating_select_##name##_d();
UML_DECLARE_FLOATING_SELECT(eq)
UML_DECLARE_FLOATING_SELECT(ne)
UML_DECLARE_FLOATING_SELECT(hs)
UML_DECLARE_FLOATING_SELECT(lo)
UML_DECLARE_FLOATING_SELECT(mi)
UML_DECLARE_FLOATING_SELECT(pl)
UML_DECLARE_FLOATING_SELECT(vs)
UML_DECLARE_FLOATING_SELECT(vc)
UML_DECLARE_FLOATING_SELECT(hi)
UML_DECLARE_FLOATING_SELECT(ls)
UML_DECLARE_FLOATING_SELECT(ge)
UML_DECLARE_FLOATING_SELECT(lt)
UML_DECLARE_FLOATING_SELECT(gt)
UML_DECLARE_FLOATING_SELECT(le)
UML_DECLARE_FLOATING_SELECT(always)
#undef UML_DECLARE_FLOATING_SELECT
void uml_arm64_gadget_pc_relative();
void uml_arm64_gadget_fp8_software();
extern void (*uml_arm64_fp8_native_up_table[])();
extern void (*uml_arm64_fp8_native_down_table[])();
extern void (*uml_arm64_fp8_native_dot_h_vector_table[])();
extern void (*uml_arm64_fp8_native_dot_h_indexed_table[])();
extern void (*uml_arm64_fp8_native_dot_s_vector_table[])();
extern void (*uml_arm64_fp8_native_dot_s_indexed_table[])();
extern void (*uml_arm64_fp8_native_mul_h_vector_table[])();
extern void (*uml_arm64_fp8_native_mul_h_indexed_table[])();
extern void (*uml_arm64_fp8_native_mul_s_vector_table[])();
extern void (*uml_arm64_fp8_native_mul_s_indexed_table[])();
extern void (*uml_arm64_fp8_native_matrix_table[])();
void uml_arm64_gadget_sysreg_tls_read();
void uml_arm64_gadget_sysreg_tls_write_r();
void uml_arm64_gadget_sysreg_tls_write_z();
void uml_arm64_gadget_sysreg_zero_read();
void uml_arm64_gadget_sysreg_dczid_read();
void uml_arm64_gadget_sysreg_ctr_read();
void uml_arm64_gadget_sysreg_fpcr_read();
void uml_arm64_gadget_sysreg_fpcr_write_r();
void uml_arm64_gadget_sysreg_fpcr_write_z();
void uml_arm64_gadget_sysreg_fpmr_read();
void uml_arm64_gadget_sysreg_fpmr_write_r();
void uml_arm64_gadget_sysreg_fpmr_write_z();
void uml_arm64_gadget_sysreg_fpsr_read();
void uml_arm64_gadget_sysreg_fpsr_write_r();
void uml_arm64_gadget_sysreg_fpsr_write_z();
void uml_arm64_gadget_sysreg_nzcv_read();
void uml_arm64_gadget_sysreg_nzcv_write_r();
void uml_arm64_gadget_sysreg_nzcv_write_z();
void uml_arm64_gadget_sysreg_cntfrq_read();
void uml_arm64_gadget_sysreg_cntvct_read();
void uml_arm64_gadget_cache_zero_r();
void uml_arm64_gadget_cache_zero_z();
void uml_arm64_gadget_code_synchronize_r();
void uml_arm64_gadget_code_synchronize_z();
void uml_arm64_gadget_gcs_popm_r();
void uml_arm64_gadget_gcs_popm_z();
void uml_arm64_gadget_gcs_pushm_r();
void uml_arm64_gadget_gcs_pushm_z();
void uml_arm64_gadget_gcs_ss1_r();
void uml_arm64_gadget_gcs_ss1_z();
void uml_arm64_gadget_gcs_ss2_r();
void uml_arm64_gadget_gcs_ss2_z();
void uml_arm64_gadget_gcs_store_rr();
void uml_arm64_gadget_gcs_store_rz();
void uml_arm64_gadget_gcs_store_sr();
void uml_arm64_gadget_gcs_store_sz();
#define UML_DECLARE_PSTATE_FIELD(name) \
	void uml_arm64_gadget_pstate_##name##_0(); \
	void uml_arm64_gadget_pstate_##name##_1();
UML_DECLARE_PSTATE_FIELD(ssbs)
UML_DECLARE_PSTATE_FIELD(dit)
UML_DECLARE_PSTATE_FIELD(tco)
UML_DECLARE_PSTATE_FIELD(sm)
UML_DECLARE_PSTATE_FIELD(za)
UML_DECLARE_PSTATE_FIELD(smza)
#undef UML_DECLARE_PSTATE_FIELD
#define UML_DECLARE_PSTATE_DAIF(name, immediate) \
	void uml_arm64_gadget_pstate_daif_##name##_##immediate();
#define UML_DECLARE_PSTATE_DAIF_IMMEDIATE(immediate) \
	UML_DECLARE_PSTATE_DAIF(set, immediate) \
	UML_DECLARE_PSTATE_DAIF(clear, immediate)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(0)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(1)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(2)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(3)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(4)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(5)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(6)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(7)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(8)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(9)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(10)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(11)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(12)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(13)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(14)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(15)
#undef UML_DECLARE_PSTATE_DAIF_IMMEDIATE
#undef UML_DECLARE_PSTATE_DAIF
#define DECLARE_MEMORY_BARRIER(kind, option) \
	void uml_arm64_gadget_##kind##_##option();
#define DECLARE_MEMORY_BARRIER_OPTION(option) \
	DECLARE_MEMORY_BARRIER(dmb, option) \
	DECLARE_MEMORY_BARRIER(dsb, option)
DECLARE_MEMORY_BARRIER_OPTION(0)
DECLARE_MEMORY_BARRIER_OPTION(1)
DECLARE_MEMORY_BARRIER_OPTION(2)
DECLARE_MEMORY_BARRIER_OPTION(3)
DECLARE_MEMORY_BARRIER_OPTION(4)
DECLARE_MEMORY_BARRIER_OPTION(5)
DECLARE_MEMORY_BARRIER_OPTION(6)
DECLARE_MEMORY_BARRIER_OPTION(7)
DECLARE_MEMORY_BARRIER_OPTION(8)
DECLARE_MEMORY_BARRIER_OPTION(9)
DECLARE_MEMORY_BARRIER_OPTION(10)
DECLARE_MEMORY_BARRIER_OPTION(11)
DECLARE_MEMORY_BARRIER_OPTION(12)
DECLARE_MEMORY_BARRIER_OPTION(13)
DECLARE_MEMORY_BARRIER_OPTION(14)
DECLARE_MEMORY_BARRIER_OPTION(15)
#undef DECLARE_MEMORY_BARRIER_OPTION
#undef DECLARE_MEMORY_BARRIER
void uml_arm64_gadget_isb();
void uml_arm64_gadget_speculation_barrier();
void uml_arm64_gadget_speculation_barrier_fallback();
#define DECLARE_NXS_BARRIER(option) \
	void uml_arm64_gadget_dsb_nxs_##option();
DECLARE_NXS_BARRIER(0)
DECLARE_NXS_BARRIER(1)
DECLARE_NXS_BARRIER(2)
DECLARE_NXS_BARRIER(3)
#undef DECLARE_NXS_BARRIER
void uml_arm64_gadget_clear_exclusive();
void uml_arm64_gadget_atomic_sp_alignment_read();
void uml_arm64_gadget_atomic_sp_alignment_write();
void uml_arm64_gadget_yield();
void uml_arm64_gadget_csdb();
void uml_arm64_gadget_dgh();
void uml_arm64_gadget_clrbhb();
void uml_arm64_gadget_nop();
#define UML_DECLARE_SCALAR_UNSIGNED(name) \
	void uml_arm64_gadget_##name##_r(); \
	void uml_arm64_gadget_##name##_z(); \
	void uml_arm64_gadget_##name##_r_sp(); \
	void uml_arm64_gadget_##name##_z_sp();
UML_DECLARE_SCALAR_UNSIGNED(load32_unsigned_fast)
UML_DECLARE_SCALAR_UNSIGNED(load64_unsigned_fast)
UML_DECLARE_SCALAR_UNSIGNED(store32_unsigned_fast)
UML_DECLARE_SCALAR_UNSIGNED(store64_unsigned_fast)
#define UML_DECLARE_SCALAR_UNSIGNED_NARROW(name) \
	UML_DECLARE_SCALAR_UNSIGNED(name)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load8_unsigned_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load16_unsigned_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load8_signed_w_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load8_signed_x_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load16_signed_w_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load16_signed_x_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load32_signed_x_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(store8_unsigned_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(store16_unsigned_fast)
#undef UML_DECLARE_SCALAR_UNSIGNED_NARROW
#undef UML_DECLARE_SCALAR_UNSIGNED
#define UML_DECLARE_VECTOR_UNSIGNED(width) \
	void uml_arm64_gadget_vector_load##width##_unsigned_fast(); \
	void uml_arm64_gadget_vector_load##width##_unsigned_fast_sp(); \
	void uml_arm64_gadget_vector_store##width##_unsigned_fast(); \
	void uml_arm64_gadget_vector_store##width##_unsigned_fast_sp();
UML_DECLARE_VECTOR_UNSIGNED(8)
UML_DECLARE_VECTOR_UNSIGNED(16)
UML_DECLARE_VECTOR_UNSIGNED(32)
UML_DECLARE_VECTOR_UNSIGNED(64)
UML_DECLARE_VECTOR_UNSIGNED(128)
#undef UML_DECLARE_VECTOR_UNSIGNED
#define UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, mode) \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_r(); \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_z(); \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_r_sp(); \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_z_sp();
#define UML_DECLARE_SCALAR_OTHER_IMMEDIATE_MODE(name, mode) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, mode)
#define UML_DECLARE_SCALAR_OTHER_COMMON(name) \
	UML_DECLARE_SCALAR_OTHER_IMMEDIATE_MODE(name, offset) \
	UML_DECLARE_SCALAR_OTHER_IMMEDIATE_MODE(name, pre) \
	UML_DECLARE_SCALAR_OTHER_IMMEDIATE_MODE(name, post) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, uxtw_0) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, x_0) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, sxtw_0) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, uxtw_1) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, x_1) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, sxtw_1)
#define UML_DECLARE_SCALAR_OTHER_STORE(name) \
	UML_DECLARE_SCALAR_OTHER_COMMON(name)
#define UML_DECLARE_SCALAR_OTHER_LOAD(name) \
	UML_DECLARE_SCALAR_OTHER_COMMON(name) \
	void uml_arm64_gadget_scalar_other_##name##_post_suppress_r();
UML_DECLARE_SCALAR_OTHER_STORE(store8)
UML_DECLARE_SCALAR_OTHER_LOAD(load8u)
UML_DECLARE_SCALAR_OTHER_LOAD(load8sx)
UML_DECLARE_SCALAR_OTHER_LOAD(load8sw)
UML_DECLARE_SCALAR_OTHER_STORE(store16)
UML_DECLARE_SCALAR_OTHER_LOAD(load16u)
UML_DECLARE_SCALAR_OTHER_LOAD(load16sx)
UML_DECLARE_SCALAR_OTHER_LOAD(load16sw)
UML_DECLARE_SCALAR_OTHER_STORE(store32)
UML_DECLARE_SCALAR_OTHER_LOAD(load32u)
UML_DECLARE_SCALAR_OTHER_LOAD(load32sx)
UML_DECLARE_SCALAR_OTHER_STORE(store64)
UML_DECLARE_SCALAR_OTHER_LOAD(load64u)
#undef UML_DECLARE_SCALAR_OTHER_LOAD
#undef UML_DECLARE_SCALAR_OTHER_STORE
#undef UML_DECLARE_SCALAR_OTHER_COMMON
#undef UML_DECLARE_SCALAR_OTHER_IMMEDIATE_MODE
#undef UML_DECLARE_SCALAR_OTHER_REGISTER_MODE
#define UML_DECLARE_VECTOR_OTHER_MODE(name, mode) \
	void uml_arm64_gadget_vector_other_##name##_##mode(); \
	void uml_arm64_gadget_vector_other_##name##_##mode##_sp();
#define UML_DECLARE_VECTOR_OTHER(name) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, offset) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, pre) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, post) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, uxtw_0) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, x_0) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, sxtw_0) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, uxtw_1) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, x_1) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, sxtw_1)
UML_DECLARE_VECTOR_OTHER(store8)
UML_DECLARE_VECTOR_OTHER(load8)
UML_DECLARE_VECTOR_OTHER(store16)
UML_DECLARE_VECTOR_OTHER(load16)
UML_DECLARE_VECTOR_OTHER(store32)
UML_DECLARE_VECTOR_OTHER(load32)
UML_DECLARE_VECTOR_OTHER(store64)
UML_DECLARE_VECTOR_OTHER(load64)
UML_DECLARE_VECTOR_OTHER(store128)
UML_DECLARE_VECTOR_OTHER(load128)
#undef UML_DECLARE_VECTOR_OTHER
#undef UML_DECLARE_VECTOR_OTHER_MODE
#define UML_DECLARE_INTEGER_PAIR_MODE(name, mode) \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_rr(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_rz(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_zr(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_zz(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_rr_sp(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_rz_sp(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_zr_sp(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_zz_sp();
#define UML_DECLARE_INTEGER_PAIR_STORE(name) \
	UML_DECLARE_INTEGER_PAIR_MODE(name, 0) \
	UML_DECLARE_INTEGER_PAIR_MODE(name, 1) \
	UML_DECLARE_INTEGER_PAIR_MODE(name, 2)
#define UML_DECLARE_INTEGER_PAIR_LOAD(name) \
	UML_DECLARE_INTEGER_PAIR_STORE(name) \
	UML_DECLARE_INTEGER_PAIR_MODE(name, 3)
UML_DECLARE_INTEGER_PAIR_STORE(store32)
UML_DECLARE_INTEGER_PAIR_LOAD(load32)
UML_DECLARE_INTEGER_PAIR_LOAD(load32s)
UML_DECLARE_INTEGER_PAIR_STORE(store64)
UML_DECLARE_INTEGER_PAIR_LOAD(load64)
#undef UML_DECLARE_INTEGER_PAIR_LOAD
#undef UML_DECLARE_INTEGER_PAIR_STORE
#undef UML_DECLARE_INTEGER_PAIR_MODE
#define UML_DECLARE_VECTOR_PAIR(name) \
	void uml_arm64_gadget_vector_pair_##name##_0(); \
	void uml_arm64_gadget_vector_pair_##name##_1(); \
	void uml_arm64_gadget_vector_pair_##name##_2(); \
	void uml_arm64_gadget_vector_pair_##name##_0_sp(); \
	void uml_arm64_gadget_vector_pair_##name##_1_sp(); \
	void uml_arm64_gadget_vector_pair_##name##_2_sp();
UML_DECLARE_VECTOR_PAIR(store32)
UML_DECLARE_VECTOR_PAIR(load32)
UML_DECLARE_VECTOR_PAIR(store64)
UML_DECLARE_VECTOR_PAIR(load64)
UML_DECLARE_VECTOR_PAIR(store128)
UML_DECLARE_VECTOR_PAIR(load128)
#undef UML_DECLARE_VECTOR_PAIR
#define DECLARE_POINTER_AUTH_LOAD_MODES(key, mode) \
	void uml_arm64_gadget_pauth_load_##key##_##mode##_rr(); \
	void uml_arm64_gadget_pauth_load_##key##_##mode##_rz(); \
	void uml_arm64_gadget_pauth_load_##key##_##mode##_sr(); \
	void uml_arm64_gadget_pauth_load_##key##_##mode##_sz();
DECLARE_POINTER_AUTH_LOAD_MODES(a, o)
DECLARE_POINTER_AUTH_LOAD_MODES(a, w)
DECLARE_POINTER_AUTH_LOAD_MODES(b, o)
DECLARE_POINTER_AUTH_LOAD_MODES(b, w)
#undef DECLARE_POINTER_AUTH_LOAD_MODES
#define UML_DECLARE_SIMD_STRUCTURE_DIRECTION(base, direction) \
	void uml_arm64_gadget_simd_structure_##base##_##direction##_0(); \
	void uml_arm64_gadget_simd_structure_##base##_##direction##_1(); \
	void uml_arm64_gadget_simd_structure_##base##_##direction##_2(); \
	void uml_arm64_gadget_simd_structure_##base##_##direction##_s0(); \
	void uml_arm64_gadget_simd_structure_##base##_##direction##_s1(); \
	void uml_arm64_gadget_simd_structure_##base##_##direction##_s2();
#define UML_DECLARE_SIMD_STRUCTURE(base) \
	UML_DECLARE_SIMD_STRUCTURE_DIRECTION(base, store) \
	UML_DECLARE_SIMD_STRUCTURE_DIRECTION(base, load)
UML_DECLARE_SIMD_STRUCTURE(contig_d_1)
UML_DECLARE_SIMD_STRUCTURE(contig_d_2)
UML_DECLARE_SIMD_STRUCTURE(contig_d_3)
UML_DECLARE_SIMD_STRUCTURE(contig_d_4)
UML_DECLARE_SIMD_STRUCTURE(contig_q_1)
UML_DECLARE_SIMD_STRUCTURE(contig_q_2)
UML_DECLARE_SIMD_STRUCTURE(contig_q_3)
UML_DECLARE_SIMD_STRUCTURE(contig_q_4)
#define UML_DECLARE_SIMD_STRUCTURE_INTERLEAVED(count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_d_b_##count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_d_h_##count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_d_s_##count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_q_b_##count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_q_h_##count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_q_s_##count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_q_d_##count)
UML_DECLARE_SIMD_STRUCTURE_INTERLEAVED(2)
UML_DECLARE_SIMD_STRUCTURE_INTERLEAVED(3)
UML_DECLARE_SIMD_STRUCTURE_INTERLEAVED(4)
#undef UML_DECLARE_SIMD_STRUCTURE_INTERLEAVED
#undef UML_DECLARE_SIMD_STRUCTURE
#undef UML_DECLARE_SIMD_STRUCTURE_DIRECTION
#define UML_DECLARE_SIMD_REPLICATE_COUNT(type, count) \
	void uml_arm64_gadget_simd_replicate_##type##_##count##_0(); \
	void uml_arm64_gadget_simd_replicate_##type##_##count##_1(); \
	void uml_arm64_gadget_simd_replicate_##type##_##count##_2(); \
	void uml_arm64_gadget_simd_replicate_##type##_##count##_s0(); \
	void uml_arm64_gadget_simd_replicate_##type##_##count##_s1(); \
	void uml_arm64_gadget_simd_replicate_##type##_##count##_s2();
#define UML_DECLARE_SIMD_REPLICATE(type) \
	UML_DECLARE_SIMD_REPLICATE_COUNT(type, 1) \
	UML_DECLARE_SIMD_REPLICATE_COUNT(type, 2) \
	UML_DECLARE_SIMD_REPLICATE_COUNT(type, 3) \
	UML_DECLARE_SIMD_REPLICATE_COUNT(type, 4)
UML_DECLARE_SIMD_REPLICATE(b_d)
UML_DECLARE_SIMD_REPLICATE(b_q)
UML_DECLARE_SIMD_REPLICATE(h_d)
UML_DECLARE_SIMD_REPLICATE(h_q)
UML_DECLARE_SIMD_REPLICATE(s_d)
UML_DECLARE_SIMD_REPLICATE(s_q)
UML_DECLARE_SIMD_REPLICATE(d_d)
UML_DECLARE_SIMD_REPLICATE(d_q)
#undef UML_DECLARE_SIMD_REPLICATE
#undef UML_DECLARE_SIMD_REPLICATE_COUNT
#define UML_DECLARE_SIMD_LANE_DIRECTION(type, count, direction) \
	void uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_0(); \
	void uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_1(); \
	void uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_2(); \
	void uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_s0(); \
	void uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_s1(); \
	void uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_s2();
#define UML_DECLARE_SIMD_LANE_COUNT(type, count) \
	UML_DECLARE_SIMD_LANE_DIRECTION(type, count, store) \
	UML_DECLARE_SIMD_LANE_DIRECTION(type, count, load)
#define UML_DECLARE_SIMD_LANE(type) \
	UML_DECLARE_SIMD_LANE_COUNT(type, 1) \
	UML_DECLARE_SIMD_LANE_COUNT(type, 2) \
	UML_DECLARE_SIMD_LANE_COUNT(type, 3) \
	UML_DECLARE_SIMD_LANE_COUNT(type, 4)
UML_DECLARE_SIMD_LANE(b)
UML_DECLARE_SIMD_LANE(h)
UML_DECLARE_SIMD_LANE(s)
UML_DECLARE_SIMD_LANE(d)
#undef UML_DECLARE_SIMD_LANE
#undef UML_DECLARE_SIMD_LANE_COUNT
#undef UML_DECLARE_SIMD_LANE_DIRECTION
#define UML_DECLARE_SIMD_ORDERED_SCALAR(size, address) \
	void uml_arm64_gadget_simd_ordered_load_##size##_##address(); \
	void uml_arm64_gadget_simd_ordered_store_##size##_##address();
UML_DECLARE_SIMD_ORDERED_SCALAR(b, r)
UML_DECLARE_SIMD_ORDERED_SCALAR(b, s)
UML_DECLARE_SIMD_ORDERED_SCALAR(h, r)
UML_DECLARE_SIMD_ORDERED_SCALAR(h, s)
UML_DECLARE_SIMD_ORDERED_SCALAR(s, r)
UML_DECLARE_SIMD_ORDERED_SCALAR(s, s)
UML_DECLARE_SIMD_ORDERED_SCALAR(d, r)
UML_DECLARE_SIMD_ORDERED_SCALAR(d, s)
#undef UML_DECLARE_SIMD_ORDERED_SCALAR
#define UML_DECLARE_SIMD_ORDERED_Q(direction, address, implementation) \
	void uml_arm64_gadget_simd_ordered_##direction##_q_##address##_##implementation();
UML_DECLARE_SIMD_ORDERED_Q(load, r, fallback)
UML_DECLARE_SIMD_ORDERED_Q(load, s, fallback)
UML_DECLARE_SIMD_ORDERED_Q(load, r, native)
UML_DECLARE_SIMD_ORDERED_Q(load, s, native)
UML_DECLARE_SIMD_ORDERED_Q(store, r, fallback)
UML_DECLARE_SIMD_ORDERED_Q(store, s, fallback)
UML_DECLARE_SIMD_ORDERED_Q(store, r, native)
UML_DECLARE_SIMD_ORDERED_Q(store, s, native)
#undef UML_DECLARE_SIMD_ORDERED_Q
#define UML_DECLARE_SIMD_ORDERED_LANE(direction, lane, address) \
	void uml_arm64_gadget_simd_ordered_##direction##_lane##lane##_##address();
UML_DECLARE_SIMD_ORDERED_LANE(load, 0, r)
UML_DECLARE_SIMD_ORDERED_LANE(load, 0, s)
UML_DECLARE_SIMD_ORDERED_LANE(load, 1, r)
UML_DECLARE_SIMD_ORDERED_LANE(load, 1, s)
UML_DECLARE_SIMD_ORDERED_LANE(store, 0, r)
UML_DECLARE_SIMD_ORDERED_LANE(store, 0, s)
UML_DECLARE_SIMD_ORDERED_LANE(store, 1, r)
UML_DECLARE_SIMD_ORDERED_LANE(store, 1, s)
#undef UML_DECLARE_SIMD_ORDERED_LANE
void uml_arm64_gadget_ordered_writeback_store_w_r();
void uml_arm64_gadget_ordered_writeback_store_w_z();
void uml_arm64_gadget_ordered_writeback_store_x_r();
void uml_arm64_gadget_ordered_writeback_store_x_z();
void uml_arm64_gadget_ordered_writeback_load_w_r();
void uml_arm64_gadget_ordered_writeback_load_w_z();
void uml_arm64_gadget_ordered_writeback_load_w_suppress();
void uml_arm64_gadget_ordered_writeback_load_x_r();
void uml_arm64_gadget_ordered_writeback_load_x_z();
void uml_arm64_gadget_ordered_writeback_load_x_suppress();
void uml_arm64_gadget_ls64_load_r();
void uml_arm64_gadget_ls64_load_s();
void uml_arm64_gadget_ls64_store_r();
void uml_arm64_gadget_ls64_store_s();
void uml_arm64_gadget_ls64_st64bv_rr();
void uml_arm64_gadget_ls64_st64bv_rz();
void uml_arm64_gadget_ls64_st64bv_sr();
void uml_arm64_gadget_ls64_st64bv_sz();
void uml_arm64_gadget_ls64_st64bv0_rr();
void uml_arm64_gadget_ls64_st64bv0_rz();
void uml_arm64_gadget_ls64_st64bv0_sr();
void uml_arm64_gadget_ls64_st64bv0_sz();
void uml_arm64_gadget_mops_prologue_copy_forward();
void uml_arm64_gadget_mops_prologue_copy_general();
void uml_arm64_gadget_mops_prologue_set();
void uml_arm64_gadget_mops_epilogue_copy_forward();
void uml_arm64_gadget_mops_epilogue_copy_general();
void uml_arm64_gadget_mops_epilogue_set_r();
void uml_arm64_gadget_mops_epilogue_set_z();
void uml_arm64_gadget_mops_set_tag_prologue();
void uml_arm64_gadget_mops_set_tag_epilogue_r();
void uml_arm64_gadget_mops_set_tag_epilogue_z();
#define UML_DECLARE_MTE_MEMORY(name) void uml_arm64_gadget_mte_memory_##name();
UML_DECLARE_MTE_MEMORY(ldg_rr)
UML_DECLARE_MTE_MEMORY(ldg_rz)
UML_DECLARE_MTE_MEMORY(ldg_sr)
UML_DECLARE_MTE_MEMORY(ldg_sz)
#define UML_DECLARE_MTE_TAG_STORE_MODES(name) \
	UML_DECLARE_MTE_MEMORY(name##_post_r) \
	UML_DECLARE_MTE_MEMORY(name##_post_s) \
	UML_DECLARE_MTE_MEMORY(name##_offset_r) \
	UML_DECLARE_MTE_MEMORY(name##_offset_s) \
	UML_DECLARE_MTE_MEMORY(name##_pre_r) \
	UML_DECLARE_MTE_MEMORY(name##_pre_s)
UML_DECLARE_MTE_TAG_STORE_MODES(stg)
UML_DECLARE_MTE_TAG_STORE_MODES(st2g)
UML_DECLARE_MTE_TAG_STORE_MODES(stzg)
UML_DECLARE_MTE_TAG_STORE_MODES(stz2g)
#undef UML_DECLARE_MTE_TAG_STORE_MODES
#define UML_DECLARE_MTE_STGP_REGISTERS(mode, base) \
	UML_DECLARE_MTE_MEMORY(stgp_##mode##_##base##rr) \
	UML_DECLARE_MTE_MEMORY(stgp_##mode##_##base##rz) \
	UML_DECLARE_MTE_MEMORY(stgp_##mode##_##base##zr) \
	UML_DECLARE_MTE_MEMORY(stgp_##mode##_##base##zz)
#define UML_DECLARE_MTE_STGP_MODE(mode) \
	UML_DECLARE_MTE_STGP_REGISTERS(mode, r) \
	UML_DECLARE_MTE_STGP_REGISTERS(mode, s)
UML_DECLARE_MTE_STGP_MODE(post)
UML_DECLARE_MTE_STGP_MODE(offset)
UML_DECLARE_MTE_STGP_MODE(pre)
#undef UML_DECLARE_MTE_STGP_MODE
#undef UML_DECLARE_MTE_STGP_REGISTERS
#undef UML_DECLARE_MTE_MEMORY
#define UML_DECLARE_ORDERED_VARIANTS(kind, ordering, size) \
	void uml_arm64_gadget_ordered_##kind##_##ordering##_##size##_r(); \
	void uml_arm64_gadget_ordered_##kind##_##ordering##_##size##_z();
#define UML_DECLARE_ORDERED_SIZES(kind, ordering) \
	UML_DECLARE_ORDERED_VARIANTS(kind, ordering, b) \
	UML_DECLARE_ORDERED_VARIANTS(kind, ordering, h) \
	UML_DECLARE_ORDERED_VARIANTS(kind, ordering, w) \
	UML_DECLARE_ORDERED_VARIANTS(kind, ordering, x)
UML_DECLARE_ORDERED_SIZES(load, relaxed)
UML_DECLARE_ORDERED_SIZES(load, acquire)
UML_DECLARE_ORDERED_SIZES(store, limited)
UML_DECLARE_ORDERED_SIZES(store, release)
#undef UML_DECLARE_ORDERED_SIZES
#undef UML_DECLARE_ORDERED_VARIANTS
#define UML_DECLARE_RCPC_LOAD_VARIANTS(name) \
	void uml_arm64_gadget_rcpc_load_##name##_r(); \
	void uml_arm64_gadget_rcpc_load_##name##_z();
UML_DECLARE_RCPC_LOAD_VARIANTS(zero_b)
UML_DECLARE_RCPC_LOAD_VARIANTS(zero_h)
UML_DECLARE_RCPC_LOAD_VARIANTS(zero_w)
UML_DECLARE_RCPC_LOAD_VARIANTS(zero_x)
UML_DECLARE_RCPC_LOAD_VARIANTS(sb_w)
UML_DECLARE_RCPC_LOAD_VARIANTS(sb_x)
UML_DECLARE_RCPC_LOAD_VARIANTS(sh_w)
UML_DECLARE_RCPC_LOAD_VARIANTS(sh_x)
UML_DECLARE_RCPC_LOAD_VARIANTS(sw_x)
#undef UML_DECLARE_RCPC_LOAD_VARIANTS
#define UML_DECLARE_RCPC_STORE_VARIANTS(size) \
	void uml_arm64_gadget_rcpc_store_##size##_r(); \
	void uml_arm64_gadget_rcpc_store_##size##_z();
UML_DECLARE_RCPC_STORE_VARIANTS(b)
UML_DECLARE_RCPC_STORE_VARIANTS(h)
UML_DECLARE_RCPC_STORE_VARIANTS(w)
UML_DECLARE_RCPC_STORE_VARIANTS(x)
#undef UML_DECLARE_RCPC_STORE_VARIANTS
#define UML_DECLARE_ORDERED_PAIR_REGISTER_VARIANTS(kind, size, address) \
	void uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_rr(); \
	void uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_rz(); \
	void uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_zr(); \
	void uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_zz();
#define UML_DECLARE_ORDERED_PAIR_SIZE(kind, size) \
	UML_DECLARE_ORDERED_PAIR_REGISTER_VARIANTS(kind, size, no) \
	UML_DECLARE_ORDERED_PAIR_REGISTER_VARIANTS(kind, size, post)
UML_DECLARE_ORDERED_PAIR_SIZE(load, w)
UML_DECLARE_ORDERED_PAIR_SIZE(load, x)
#undef UML_DECLARE_ORDERED_PAIR_SIZE
#define UML_DECLARE_ORDERED_PAIR_SIZE(kind, size) \
	UML_DECLARE_ORDERED_PAIR_REGISTER_VARIANTS(kind, size, no) \
	UML_DECLARE_ORDERED_PAIR_REGISTER_VARIANTS(kind, size, pre)
UML_DECLARE_ORDERED_PAIR_SIZE(store, w)
UML_DECLARE_ORDERED_PAIR_SIZE(store, x)
#undef UML_DECLARE_ORDERED_PAIR_SIZE
#undef UML_DECLARE_ORDERED_PAIR_REGISTER_VARIANTS
#define UML_DECLARE_CONSISTENT_PAIR_VARIANTS(kind) \
	void uml_arm64_gadget_consistent_pair_##kind##_rr(); \
	void uml_arm64_gadget_consistent_pair_##kind##_rz(); \
	void uml_arm64_gadget_consistent_pair_##kind##_zr(); \
	void uml_arm64_gadget_consistent_pair_##kind##_zz();
UML_DECLARE_CONSISTENT_PAIR_VARIANTS(ldap)
UML_DECLARE_CONSISTENT_PAIR_VARIANTS(ldapp)
UML_DECLARE_CONSISTENT_PAIR_VARIANTS(store)
#undef UML_DECLARE_CONSISTENT_PAIR_VARIANTS
#define UML_DECLARE_COMPARE_SWAP_REGISTER_VARIANTS(ordering, size) \
	void uml_arm64_gadget_compare_swap_##ordering##_##size##_rr(); \
	void uml_arm64_gadget_compare_swap_##ordering##_##size##_rz(); \
	void uml_arm64_gadget_compare_swap_##ordering##_##size##_zr(); \
	void uml_arm64_gadget_compare_swap_##ordering##_##size##_zz();
#define UML_DECLARE_COMPARE_SWAP_SIZES(ordering) \
	UML_DECLARE_COMPARE_SWAP_REGISTER_VARIANTS(ordering, b) \
	UML_DECLARE_COMPARE_SWAP_REGISTER_VARIANTS(ordering, h) \
	UML_DECLARE_COMPARE_SWAP_REGISTER_VARIANTS(ordering, w) \
	UML_DECLARE_COMPARE_SWAP_REGISTER_VARIANTS(ordering, x)
UML_DECLARE_COMPARE_SWAP_SIZES(relaxed)
UML_DECLARE_COMPARE_SWAP_SIZES(release)
UML_DECLARE_COMPARE_SWAP_SIZES(acquire)
UML_DECLARE_COMPARE_SWAP_SIZES(acquire_release)
#undef UML_DECLARE_COMPARE_SWAP_SIZES
#undef UML_DECLARE_COMPARE_SWAP_REGISTER_VARIANTS
#define UML_DECLARE_RCW_SCALAR_FULL_VARIANTS(operation, soft, ordering) \
	void uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rr(); \
	void uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rz(); \
	void uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zr(); \
	void uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zz();
#define UML_DECLARE_RCW_SCALAR_LIVE_RESULT(operation, soft, ordering) \
	void uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rr(); \
	void uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zr();
#define UML_DECLARE_RCW_SCALAR_ORDERINGS(operation, soft) \
	UML_DECLARE_RCW_SCALAR_FULL_VARIANTS(operation, soft, relaxed) \
	UML_DECLARE_RCW_SCALAR_FULL_VARIANTS(operation, soft, release) \
	UML_DECLARE_RCW_SCALAR_LIVE_RESULT(operation, soft, acquire) \
	UML_DECLARE_RCW_SCALAR_LIVE_RESULT(operation, soft, acquire_release)
#define UML_DECLARE_RCW_SCALAR_SOFTNESS(operation) \
	UML_DECLARE_RCW_SCALAR_ORDERINGS(operation, hard) \
	UML_DECLARE_RCW_SCALAR_ORDERINGS(operation, soft)
UML_DECLARE_RCW_SCALAR_SOFTNESS(cas)
UML_DECLARE_RCW_SCALAR_SOFTNESS(bic)
UML_DECLARE_RCW_SCALAR_SOFTNESS(swp)
UML_DECLARE_RCW_SCALAR_SOFTNESS(orr)
#undef UML_DECLARE_RCW_SCALAR_SOFTNESS
#undef UML_DECLARE_RCW_SCALAR_ORDERINGS
#undef UML_DECLARE_RCW_SCALAR_LIVE_RESULT
#undef UML_DECLARE_RCW_SCALAR_FULL_VARIANTS
#define UML_DECLARE_RCW_PAIR_CAS_VARIANTS(soft, ordering) \
	void uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_rr(); \
	void uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_rz(); \
	void uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_zr(); \
	void uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_zz();
#define UML_DECLARE_RCW_PAIR_CAS_ORDERS(soft) \
	UML_DECLARE_RCW_PAIR_CAS_VARIANTS(soft, relaxed) \
	UML_DECLARE_RCW_PAIR_CAS_VARIANTS(soft, release) \
	UML_DECLARE_RCW_PAIR_CAS_VARIANTS(soft, acquire) \
	UML_DECLARE_RCW_PAIR_CAS_VARIANTS(soft, acquire_release)
UML_DECLARE_RCW_PAIR_CAS_ORDERS(hard)
UML_DECLARE_RCW_PAIR_CAS_ORDERS(soft)
#undef UML_DECLARE_RCW_PAIR_CAS_ORDERS
#undef UML_DECLARE_RCW_PAIR_CAS_VARIANTS
#define UML_DECLARE_RCW_PAIR_MODIFY_VARIANTS(operation, soft) \
	void uml_arm64_gadget_rcw_pair_##operation##_##soft##_relaxed(); \
	void uml_arm64_gadget_rcw_pair_##operation##_##soft##_release(); \
	void uml_arm64_gadget_rcw_pair_##operation##_##soft##_acquire(); \
	void uml_arm64_gadget_rcw_pair_##operation##_##soft##_acquire_release();
#define UML_DECLARE_RCW_PAIR_MODIFY_SOFTNESS(operation) \
	UML_DECLARE_RCW_PAIR_MODIFY_VARIANTS(operation, hard) \
	UML_DECLARE_RCW_PAIR_MODIFY_VARIANTS(operation, soft)
UML_DECLARE_RCW_PAIR_MODIFY_SOFTNESS(bic)
UML_DECLARE_RCW_PAIR_MODIFY_SOFTNESS(swp)
UML_DECLARE_RCW_PAIR_MODIFY_SOFTNESS(orr)
#undef UML_DECLARE_RCW_PAIR_MODIFY_SOFTNESS
#undef UML_DECLARE_RCW_PAIR_MODIFY_VARIANTS
#define UML_DECLARE_COMPARE_SWAP_PAIR_REGISTER_VARIANTS(ordering, size) \
	void uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_rr(); \
	void uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_rz(); \
	void uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_zr(); \
	void uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_zz();
#define UML_DECLARE_COMPARE_SWAP_PAIR_SIZES(ordering) \
	UML_DECLARE_COMPARE_SWAP_PAIR_REGISTER_VARIANTS(ordering, w) \
	UML_DECLARE_COMPARE_SWAP_PAIR_REGISTER_VARIANTS(ordering, x)
UML_DECLARE_COMPARE_SWAP_PAIR_SIZES(relaxed)
UML_DECLARE_COMPARE_SWAP_PAIR_SIZES(release)
UML_DECLARE_COMPARE_SWAP_PAIR_SIZES(acquire)
UML_DECLARE_COMPARE_SWAP_PAIR_SIZES(acquire_release)
#undef UML_DECLARE_COMPARE_SWAP_PAIR_SIZES
#undef UML_DECLARE_COMPARE_SWAP_PAIR_REGISTER_VARIANTS
#define UML_DECLARE_EXCLUSIVE_LOAD_VARIANTS(ordering, size) \
	void uml_arm64_gadget_exclusive_load_##ordering##_##size##_r(); \
	void uml_arm64_gadget_exclusive_load_##ordering##_##size##_z();
#define UML_DECLARE_EXCLUSIVE_LOAD_SIZES(ordering) \
	UML_DECLARE_EXCLUSIVE_LOAD_VARIANTS(ordering, b) \
	UML_DECLARE_EXCLUSIVE_LOAD_VARIANTS(ordering, h) \
	UML_DECLARE_EXCLUSIVE_LOAD_VARIANTS(ordering, w) \
	UML_DECLARE_EXCLUSIVE_LOAD_VARIANTS(ordering, x)
UML_DECLARE_EXCLUSIVE_LOAD_SIZES(relaxed)
UML_DECLARE_EXCLUSIVE_LOAD_SIZES(acquire)
#undef UML_DECLARE_EXCLUSIVE_LOAD_SIZES
#undef UML_DECLARE_EXCLUSIVE_LOAD_VARIANTS
#define UML_DECLARE_EXCLUSIVE_STORE_VARIANTS(ordering, size) \
	void uml_arm64_gadget_exclusive_store_##ordering##_##size##_rr(); \
	void uml_arm64_gadget_exclusive_store_##ordering##_##size##_rz(); \
	void uml_arm64_gadget_exclusive_store_##ordering##_##size##_zr(); \
	void uml_arm64_gadget_exclusive_store_##ordering##_##size##_zz();
#define UML_DECLARE_EXCLUSIVE_STORE_SIZES(ordering) \
	UML_DECLARE_EXCLUSIVE_STORE_VARIANTS(ordering, b) \
	UML_DECLARE_EXCLUSIVE_STORE_VARIANTS(ordering, h) \
	UML_DECLARE_EXCLUSIVE_STORE_VARIANTS(ordering, w) \
	UML_DECLARE_EXCLUSIVE_STORE_VARIANTS(ordering, x)
UML_DECLARE_EXCLUSIVE_STORE_SIZES(relaxed)
UML_DECLARE_EXCLUSIVE_STORE_SIZES(release)
#undef UML_DECLARE_EXCLUSIVE_STORE_SIZES
#undef UML_DECLARE_EXCLUSIVE_STORE_VARIANTS
#define UML_DECLARE_EXCLUSIVE_PAIR_LOAD_VARIANTS(ordering, size) \
	void uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_rr(); \
	void uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_rz(); \
	void uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_zr(); \
	void uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_zz();
#define UML_DECLARE_EXCLUSIVE_PAIR_LOAD_SIZES(ordering) \
	UML_DECLARE_EXCLUSIVE_PAIR_LOAD_VARIANTS(ordering, w) \
	UML_DECLARE_EXCLUSIVE_PAIR_LOAD_VARIANTS(ordering, x)
UML_DECLARE_EXCLUSIVE_PAIR_LOAD_SIZES(relaxed)
UML_DECLARE_EXCLUSIVE_PAIR_LOAD_SIZES(acquire)
#undef UML_DECLARE_EXCLUSIVE_PAIR_LOAD_SIZES
#undef UML_DECLARE_EXCLUSIVE_PAIR_LOAD_VARIANTS
#define UML_DECLARE_EXCLUSIVE_PAIR_STORE_VARIANTS(ordering, size) \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rrr(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rrz(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rzr(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rzz(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zrr(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zrz(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zzr(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zzz();
#define UML_DECLARE_EXCLUSIVE_PAIR_STORE_SIZES(ordering) \
	UML_DECLARE_EXCLUSIVE_PAIR_STORE_VARIANTS(ordering, w) \
	UML_DECLARE_EXCLUSIVE_PAIR_STORE_VARIANTS(ordering, x)
UML_DECLARE_EXCLUSIVE_PAIR_STORE_SIZES(relaxed)
UML_DECLARE_EXCLUSIVE_PAIR_STORE_SIZES(release)
#undef UML_DECLARE_EXCLUSIVE_PAIR_STORE_SIZES
#undef UML_DECLARE_EXCLUSIVE_PAIR_STORE_VARIANTS
#define UML_DECLARE_ATOMIC_REGISTER_VARIANTS(operation, ordering, size) \
	void uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_rr(); \
	void uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_rz(); \
	void uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_zr(); \
	void uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_zz();
#define UML_DECLARE_ATOMIC_SIZES(operation, ordering) \
	UML_DECLARE_ATOMIC_REGISTER_VARIANTS(operation, ordering, b) \
	UML_DECLARE_ATOMIC_REGISTER_VARIANTS(operation, ordering, h) \
	UML_DECLARE_ATOMIC_REGISTER_VARIANTS(operation, ordering, w) \
	UML_DECLARE_ATOMIC_REGISTER_VARIANTS(operation, ordering, x)
#define UML_DECLARE_ATOMIC_ORDERS(operation) \
	UML_DECLARE_ATOMIC_SIZES(operation, relaxed) \
	UML_DECLARE_ATOMIC_SIZES(operation, release) \
	UML_DECLARE_ATOMIC_SIZES(operation, acquire) \
	UML_DECLARE_ATOMIC_SIZES(operation, acquire_release)
UML_DECLARE_ATOMIC_ORDERS(ldadd)
UML_DECLARE_ATOMIC_ORDERS(ldclr)
UML_DECLARE_ATOMIC_ORDERS(ldeor)
UML_DECLARE_ATOMIC_ORDERS(ldset)
UML_DECLARE_ATOMIC_ORDERS(ldsmax)
UML_DECLARE_ATOMIC_ORDERS(ldsmin)
UML_DECLARE_ATOMIC_ORDERS(ldumax)
UML_DECLARE_ATOMIC_ORDERS(ldumin)
UML_DECLARE_ATOMIC_ORDERS(swp)
#undef UML_DECLARE_ATOMIC_ORDERS
#undef UML_DECLARE_ATOMIC_SIZES
#undef UML_DECLARE_ATOMIC_REGISTER_VARIANTS
#define UML_DECLARE_ATOMIC_FLOATING_VARIANTS(implementation, size, operation) \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_relaxed(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_relaxed_sp(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_release(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_release_sp(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire_sp(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire_release(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire_release_sp(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_relaxed(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_relaxed_sp(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_release(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_release_sp();
#define UML_DECLARE_ATOMIC_FLOATING_OPERATIONS(implementation, size) \
	UML_DECLARE_ATOMIC_FLOATING_VARIANTS(implementation, size, add) \
	UML_DECLARE_ATOMIC_FLOATING_VARIANTS(implementation, size, max) \
	UML_DECLARE_ATOMIC_FLOATING_VARIANTS(implementation, size, maxnm) \
	UML_DECLARE_ATOMIC_FLOATING_VARIANTS(implementation, size, min) \
	UML_DECLARE_ATOMIC_FLOATING_VARIANTS(implementation, size, minnm)
#define UML_DECLARE_ATOMIC_FLOATING_SIZES(implementation) \
	UML_DECLARE_ATOMIC_FLOATING_OPERATIONS(implementation, bf) \
	UML_DECLARE_ATOMIC_FLOATING_OPERATIONS(implementation, h) \
	UML_DECLARE_ATOMIC_FLOATING_OPERATIONS(implementation, s) \
	UML_DECLARE_ATOMIC_FLOATING_OPERATIONS(implementation, d)
UML_DECLARE_ATOMIC_FLOATING_SIZES(native)
UML_DECLARE_ATOMIC_FLOATING_SIZES(software)
#undef UML_DECLARE_ATOMIC_FLOATING_SIZES
#undef UML_DECLARE_ATOMIC_FLOATING_OPERATIONS
#undef UML_DECLARE_ATOMIC_FLOATING_VARIANTS
#define UML_DECLARE_ATOMIC_PAIR128_ORDER(operation, ordering) \
	void uml_arm64_gadget_atomic_pair128_##operation##_##ordering();
#define UML_DECLARE_ATOMIC_PAIR128_ORDERS(operation) \
	UML_DECLARE_ATOMIC_PAIR128_ORDER(operation, relaxed) \
	UML_DECLARE_ATOMIC_PAIR128_ORDER(operation, release) \
	UML_DECLARE_ATOMIC_PAIR128_ORDER(operation, acquire) \
	UML_DECLARE_ATOMIC_PAIR128_ORDER(operation, acquire_release)
UML_DECLARE_ATOMIC_PAIR128_ORDERS(clr)
UML_DECLARE_ATOMIC_PAIR128_ORDERS(set)
UML_DECLARE_ATOMIC_PAIR128_ORDERS(swp)
#undef UML_DECLARE_ATOMIC_PAIR128_ORDERS
#undef UML_DECLARE_ATOMIC_PAIR128_ORDER
#define UML_DECLARE_LITERAL_INTEGER(name) \
	void uml_arm64_gadget_##name##_r(); \
	void uml_arm64_gadget_##name##_z();
UML_DECLARE_LITERAL_INTEGER(literal_load32_fast)
UML_DECLARE_LITERAL_INTEGER(literal_load64_fast)
UML_DECLARE_LITERAL_INTEGER(literal_load32_signed_fast)
#undef UML_DECLARE_LITERAL_INTEGER
void uml_arm64_gadget_literal_load_vector32_fast();
void uml_arm64_gadget_literal_load_vector64_fast();
void uml_arm64_gadget_literal_load_vector128_fast();
void uml_arm64_gadget_svc();
void uml_arm64_gadget_interrupt();
void uml_arm64_gadget_fatal();
void uml_arm64_gadget_illegal();
void uml_arm64_gadget_breakpoint();
void uml_arm64_gadget_gcs_fault();
void uml_arm64_gadget_branch();
void uml_arm64_gadget_branch_linked();
void uml_arm64_gadget_branch_link();
void uml_arm64_gadget_branch_link_linked();
void uml_arm64_gadget_branch_register();
void uml_arm64_gadget_branch_link_register();
void uml_arm64_gadget_branch_register_cached();
void uml_arm64_gadget_branch_link_register_cached();
void uml_arm64_gadget_return_cached();
#define DECLARE_POINTER_AUTH_BRANCH(kind, key) \
	void uml_arm64_gadget_pauth_branch_##kind##_##key##_rr(); \
	void uml_arm64_gadget_pauth_branch_##kind##_##key##_rz(); \
	void uml_arm64_gadget_pauth_branch_##kind##_##key##_zr(); \
	void uml_arm64_gadget_pauth_branch_##kind##_##key##_zz();
DECLARE_POINTER_AUTH_BRANCH(bra, a)
DECLARE_POINTER_AUTH_BRANCH(bra, b)
DECLARE_POINTER_AUTH_BRANCH(blra, a)
DECLARE_POINTER_AUTH_BRANCH(blra, b)
DECLARE_POINTER_AUTH_BRANCH(blra_gcs, a)
DECLARE_POINTER_AUTH_BRANCH(blra_gcs, b)
#undef DECLARE_POINTER_AUTH_BRANCH
void uml_arm64_gadget_pauth_return_gcs_a();
void uml_arm64_gadget_pauth_return_gcs_b();
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
#define UML_DECLARE_LINKED_CONDITION(name) \
	void uml_arm64_gadget_branch_##name##_linked();
UML_DECLARE_LINKED_CONDITION(eq)
UML_DECLARE_LINKED_CONDITION(ne)
UML_DECLARE_LINKED_CONDITION(hs)
UML_DECLARE_LINKED_CONDITION(lo)
UML_DECLARE_LINKED_CONDITION(mi)
UML_DECLARE_LINKED_CONDITION(pl)
UML_DECLARE_LINKED_CONDITION(vs)
UML_DECLARE_LINKED_CONDITION(vc)
UML_DECLARE_LINKED_CONDITION(hi)
UML_DECLARE_LINKED_CONDITION(ls)
UML_DECLARE_LINKED_CONDITION(ge)
UML_DECLARE_LINKED_CONDITION(lt)
UML_DECLARE_LINKED_CONDITION(gt)
UML_DECLARE_LINKED_CONDITION(le)
#undef UML_DECLARE_LINKED_CONDITION
#define UML_DECLARE_CMPBR_I_SIZE(size) \
	void uml_arm64_gadget_cmpbr_i_##size##_sgt(); \
	void uml_arm64_gadget_cmpbr_i_##size##_slt(); \
	void uml_arm64_gadget_cmpbr_i_##size##_ugt(); \
	void uml_arm64_gadget_cmpbr_i_##size##_ult(); \
	void uml_arm64_gadget_cmpbr_i_##size##_eq(); \
	void uml_arm64_gadget_cmpbr_i_##size##_ne();
UML_DECLARE_CMPBR_I_SIZE(w)
UML_DECLARE_CMPBR_I_SIZE(x)
#undef UML_DECLARE_CMPBR_I_SIZE
#define UML_DECLARE_CMPBR_R_MODES(size, condition) \
	void uml_arm64_gadget_cmpbr_r_##size##_##condition##_rr(); \
	void uml_arm64_gadget_cmpbr_r_##size##_##condition##_rz(); \
	void uml_arm64_gadget_cmpbr_r_##size##_##condition##_zr();
#define UML_DECLARE_CMPBR_R_SIZE(size) \
	UML_DECLARE_CMPBR_R_MODES(size, sgt) \
	UML_DECLARE_CMPBR_R_MODES(size, sge) \
	UML_DECLARE_CMPBR_R_MODES(size, ugt) \
	UML_DECLARE_CMPBR_R_MODES(size, uge) \
	UML_DECLARE_CMPBR_R_MODES(size, eq) \
	UML_DECLARE_CMPBR_R_MODES(size, ne)
UML_DECLARE_CMPBR_R_SIZE(b)
UML_DECLARE_CMPBR_R_SIZE(h)
UML_DECLARE_CMPBR_R_SIZE(w)
UML_DECLARE_CMPBR_R_SIZE(x)
#undef UML_DECLARE_CMPBR_R_SIZE
#undef UML_DECLARE_CMPBR_R_MODES
void uml_arm64_gadget_branch_always();
void uml_arm64_gadget_branch_always_linked();
#define UML_DECLARE_COMPARE_BRANCH(width, polarity) \
	void uml_arm64_gadget_compare_branch_##width##_##polarity(); \
	void uml_arm64_gadget_compare_branch_##width##_##polarity##_linked();
UML_DECLARE_COMPARE_BRANCH(w, z)
UML_DECLARE_COMPARE_BRANCH(w, nz)
UML_DECLARE_COMPARE_BRANCH(x, z)
UML_DECLARE_COMPARE_BRANCH(x, nz)
#undef UML_DECLARE_COMPARE_BRANCH
#define UML_DECLARE_TEST_BRANCH(polarity) \
	void uml_arm64_gadget_test_branch_##polarity(); \
	void uml_arm64_gadget_test_branch_##polarity##_linked();
UML_DECLARE_TEST_BRANCH(z)
UML_DECLARE_TEST_BRANCH(nz)
#undef UML_DECLARE_TEST_BRANCH
void uml_arm64_gadget_link_exit();
}

using Gadget = void (*)();

[[nodiscard]] Gadget literal_load_gadget(
	bool vector, unsigned operation, bool discard) noexcept
{
	if (vector) {
		static constexpr std::array gadgets{
			uml_arm64_gadget_literal_load_vector32_fast,
			uml_arm64_gadget_literal_load_vector64_fast,
			uml_arm64_gadget_literal_load_vector128_fast};
		return operation < gadgets.size() ? gadgets[operation] : nullptr;
	}
	using Modes = std::array<Gadget, 2>;
	static constexpr std::array gadgets{
		Modes{uml_arm64_gadget_literal_load32_fast_r,
			uml_arm64_gadget_literal_load32_fast_z},
		Modes{uml_arm64_gadget_literal_load64_fast_r,
			uml_arm64_gadget_literal_load64_fast_z},
		Modes{uml_arm64_gadget_literal_load32_signed_fast_r,
			uml_arm64_gadget_literal_load32_signed_fast_z}};
	return operation < gadgets.size() ? gadgets[operation][discard] : nullptr;
}

[[nodiscard]] Gadget scalar_unsigned_memory_gadget(unsigned size,
	unsigned operation, bool base_sp, bool transfer_zero) noexcept
{
	using Registers = std::array<Gadget, 2>;
	using Bases = std::array<Registers, 2>;
#define UML_SCALAR_UNSIGNED_BASES(name) Bases{ \
	Registers{uml_arm64_gadget_##name##_r, \
		uml_arm64_gadget_##name##_z}, \
	Registers{uml_arm64_gadget_##name##_r_sp, \
		uml_arm64_gadget_##name##_z_sp}}
	static constexpr std::array stores{
		UML_SCALAR_UNSIGNED_BASES(store8_unsigned_fast),
		UML_SCALAR_UNSIGNED_BASES(store16_unsigned_fast),
		UML_SCALAR_UNSIGNED_BASES(store32_unsigned_fast),
		UML_SCALAR_UNSIGNED_BASES(store64_unsigned_fast)};
	static constexpr std::array loads{
		UML_SCALAR_UNSIGNED_BASES(load8_unsigned_fast),
		UML_SCALAR_UNSIGNED_BASES(load16_unsigned_fast),
		UML_SCALAR_UNSIGNED_BASES(load32_unsigned_fast),
		UML_SCALAR_UNSIGNED_BASES(load64_unsigned_fast)};
	static constexpr std::array signed_x{
		UML_SCALAR_UNSIGNED_BASES(load8_signed_x_fast),
		UML_SCALAR_UNSIGNED_BASES(load16_signed_x_fast),
		UML_SCALAR_UNSIGNED_BASES(load32_signed_x_fast),
		Bases{}};
	static constexpr std::array signed_w{
		UML_SCALAR_UNSIGNED_BASES(load8_signed_w_fast),
		UML_SCALAR_UNSIGNED_BASES(load16_signed_w_fast),
		Bases{}, Bases{}};
#undef UML_SCALAR_UNSIGNED_BASES
	const auto &gadgets = operation == 0U ? stores : operation == 1U ? loads :
		operation == 2U ? signed_x : signed_w;
	return size < 4U ? gadgets[size][base_sp][transfer_zero] : nullptr;
}

[[nodiscard]] Gadget vector_unsigned_memory_gadget(
	unsigned size, unsigned operation, bool base_sp) noexcept
{
	using Bases = std::array<Gadget, 2>;
#define UML_VECTOR_UNSIGNED_BASES(name) Bases{ \
	uml_arm64_gadget_vector_##name##_unsigned_fast, \
	uml_arm64_gadget_vector_##name##_unsigned_fast_sp}
	static constexpr std::array loads{
		UML_VECTOR_UNSIGNED_BASES(load8),
		UML_VECTOR_UNSIGNED_BASES(load16),
		UML_VECTOR_UNSIGNED_BASES(load32),
		UML_VECTOR_UNSIGNED_BASES(load64),
		UML_VECTOR_UNSIGNED_BASES(load128)};
	static constexpr std::array stores{
		UML_VECTOR_UNSIGNED_BASES(store8),
		UML_VECTOR_UNSIGNED_BASES(store16),
		UML_VECTOR_UNSIGNED_BASES(store32),
		UML_VECTOR_UNSIGNED_BASES(store64),
		UML_VECTOR_UNSIGNED_BASES(store128)};
#undef UML_VECTOR_UNSIGNED_BASES
	const unsigned width = operation >= 2U ? 4U : size;
	return width < loads.size() ?
		((operation & 1U) != 0U ? loads : stores)[width][base_sp] : nullptr;
}

using ScalarOtherRegisterChoices = std::array<Gadget, 2>;
using ScalarOtherBaseChoices = std::array<ScalarOtherRegisterChoices, 2>;
using ScalarOtherImmediateModes = std::array<ScalarOtherBaseChoices, 4>;
using ScalarOtherRegisterModes = std::array<ScalarOtherBaseChoices, 6>;

struct ScalarOtherGadgets final {
	ScalarOtherImmediateModes immediate;
	ScalarOtherRegisterModes register_offset;
};

#define UML_SCALAR_OTHER_REGISTER_CHOICES(name, mode) \
	ScalarOtherRegisterChoices{ \
	uml_arm64_gadget_scalar_other_##name##_##mode##_r, \
	uml_arm64_gadget_scalar_other_##name##_##mode##_z}
#define UML_SCALAR_OTHER_BASE_CHOICES(name, mode) ScalarOtherBaseChoices{ \
	UML_SCALAR_OTHER_REGISTER_CHOICES(name, mode), \
	ScalarOtherRegisterChoices{ \
		uml_arm64_gadget_scalar_other_##name##_##mode##_r_sp, \
		uml_arm64_gadget_scalar_other_##name##_##mode##_z_sp}}
#define UML_SCALAR_OTHER_REGISTERS(name) ScalarOtherRegisterModes{ \
	UML_SCALAR_OTHER_BASE_CHOICES(name, uxtw_0), \
	UML_SCALAR_OTHER_BASE_CHOICES(name, x_0), \
	UML_SCALAR_OTHER_BASE_CHOICES(name, sxtw_0), \
	UML_SCALAR_OTHER_BASE_CHOICES(name, uxtw_1), \
	UML_SCALAR_OTHER_BASE_CHOICES(name, x_1), \
	UML_SCALAR_OTHER_BASE_CHOICES(name, sxtw_1)}
#define UML_SCALAR_OTHER_STORE_GADGETS(name) ScalarOtherGadgets{ \
	ScalarOtherImmediateModes{ \
		UML_SCALAR_OTHER_BASE_CHOICES(name, offset), \
		UML_SCALAR_OTHER_BASE_CHOICES(name, pre), \
		UML_SCALAR_OTHER_BASE_CHOICES(name, post), \
		ScalarOtherBaseChoices{}}, \
	UML_SCALAR_OTHER_REGISTERS(name)}
#define UML_SCALAR_OTHER_LOAD_GADGETS(name) ScalarOtherGadgets{ \
	ScalarOtherImmediateModes{ \
		UML_SCALAR_OTHER_BASE_CHOICES(name, offset), \
		UML_SCALAR_OTHER_BASE_CHOICES(name, pre), \
		UML_SCALAR_OTHER_BASE_CHOICES(name, post), \
		ScalarOtherBaseChoices{ \
			ScalarOtherRegisterChoices{ \
				uml_arm64_gadget_scalar_other_##name##_post_suppress_r, \
				nullptr}, \
			ScalarOtherRegisterChoices{}}}, \
	UML_SCALAR_OTHER_REGISTERS(name)}

[[nodiscard]] const ScalarOtherGadgets *scalar_other_gadgets(
	unsigned size, unsigned operation) noexcept
{
	static constexpr auto store8 = UML_SCALAR_OTHER_STORE_GADGETS(store8);
	static constexpr auto load8u = UML_SCALAR_OTHER_LOAD_GADGETS(load8u);
	static constexpr auto load8sx = UML_SCALAR_OTHER_LOAD_GADGETS(load8sx);
	static constexpr auto load8sw = UML_SCALAR_OTHER_LOAD_GADGETS(load8sw);
	static constexpr auto store16 = UML_SCALAR_OTHER_STORE_GADGETS(store16);
	static constexpr auto load16u = UML_SCALAR_OTHER_LOAD_GADGETS(load16u);
	static constexpr auto load16sx = UML_SCALAR_OTHER_LOAD_GADGETS(load16sx);
	static constexpr auto load16sw = UML_SCALAR_OTHER_LOAD_GADGETS(load16sw);
	static constexpr auto store32 = UML_SCALAR_OTHER_STORE_GADGETS(store32);
	static constexpr auto load32u = UML_SCALAR_OTHER_LOAD_GADGETS(load32u);
	static constexpr auto load32sx = UML_SCALAR_OTHER_LOAD_GADGETS(load32sx);
	static constexpr auto store64 = UML_SCALAR_OTHER_STORE_GADGETS(store64);
	static constexpr auto load64u = UML_SCALAR_OTHER_LOAD_GADGETS(load64u);
	switch (size * 4U + operation) {
	case 0: return &store8;
	case 1: return &load8u;
	case 2: return &load8sx;
	case 3: return &load8sw;
	case 4: return &store16;
	case 5: return &load16u;
	case 6: return &load16sx;
	case 7: return &load16sw;
	case 8: return &store32;
	case 9: return &load32u;
	case 10: return &load32sx;
	case 12: return &store64;
	case 13: return &load64u;
	default: return nullptr;
	}
}

#undef UML_SCALAR_OTHER_LOAD_GADGETS
#undef UML_SCALAR_OTHER_STORE_GADGETS
#undef UML_SCALAR_OTHER_REGISTERS
#undef UML_SCALAR_OTHER_BASE_CHOICES
#undef UML_SCALAR_OTHER_REGISTER_CHOICES

using VectorOtherBaseModes = std::array<Gadget, 2>;
using VectorOtherImmediateModes = std::array<VectorOtherBaseModes, 3>;
using VectorOtherRegisterModes = std::array<VectorOtherBaseModes, 6>;

struct VectorOtherGadgets final {
	VectorOtherImmediateModes immediate;
	VectorOtherRegisterModes register_offset;
};

#define UML_VECTOR_OTHER_GADGETS(name) VectorOtherGadgets{ \
	VectorOtherImmediateModes{ \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_offset, \
			uml_arm64_gadget_vector_other_##name##_offset_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_pre, \
			uml_arm64_gadget_vector_other_##name##_pre_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_post, \
			uml_arm64_gadget_vector_other_##name##_post_sp}}, \
	VectorOtherRegisterModes{ \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_uxtw_0, \
			uml_arm64_gadget_vector_other_##name##_uxtw_0_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_x_0, \
			uml_arm64_gadget_vector_other_##name##_x_0_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_sxtw_0, \
			uml_arm64_gadget_vector_other_##name##_sxtw_0_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_uxtw_1, \
			uml_arm64_gadget_vector_other_##name##_uxtw_1_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_x_1, \
			uml_arm64_gadget_vector_other_##name##_x_1_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_sxtw_1, \
			uml_arm64_gadget_vector_other_##name##_sxtw_1_sp}}}

[[nodiscard]] const VectorOtherGadgets *vector_other_gadgets(
	unsigned size, unsigned operation) noexcept
{
	static constexpr auto store8 = UML_VECTOR_OTHER_GADGETS(store8);
	static constexpr auto load8 = UML_VECTOR_OTHER_GADGETS(load8);
	static constexpr auto store16 = UML_VECTOR_OTHER_GADGETS(store16);
	static constexpr auto load16 = UML_VECTOR_OTHER_GADGETS(load16);
	static constexpr auto store32 = UML_VECTOR_OTHER_GADGETS(store32);
	static constexpr auto load32 = UML_VECTOR_OTHER_GADGETS(load32);
	static constexpr auto store64 = UML_VECTOR_OTHER_GADGETS(store64);
	static constexpr auto load64 = UML_VECTOR_OTHER_GADGETS(load64);
	static constexpr auto store128 = UML_VECTOR_OTHER_GADGETS(store128);
	static constexpr auto load128 = UML_VECTOR_OTHER_GADGETS(load128);
	static constexpr std::array stores{
		&store8, &store16, &store32, &store64, &store128};
	static constexpr std::array loads{
		&load8, &load16, &load32, &load64, &load128};
	const unsigned width = operation >= 2U ? 4U : size;
	return width < loads.size() ?
		((operation & 1U) != 0U ? loads : stores)[width] : nullptr;
}

#undef UML_VECTOR_OTHER_GADGETS

using IntegerPairRegisterModes = std::array<Gadget, 4>;
using IntegerPairBaseModes = std::array<IntegerPairRegisterModes, 2>;
using IntegerPairAddressModes = std::array<IntegerPairBaseModes, 4>;

#define UML_INTEGER_PAIR_REGISTER_MODES(name, mode) \
	IntegerPairRegisterModes{ \
		uml_arm64_gadget_integer_pair_##name##_##mode##_rr, \
		uml_arm64_gadget_integer_pair_##name##_##mode##_rz, \
		uml_arm64_gadget_integer_pair_##name##_##mode##_zr, \
		uml_arm64_gadget_integer_pair_##name##_##mode##_zz}
#define UML_INTEGER_PAIR_SP_REGISTER_MODES(name, mode) \
	IntegerPairRegisterModes{ \
		uml_arm64_gadget_integer_pair_##name##_##mode##_rr_sp, \
		uml_arm64_gadget_integer_pair_##name##_##mode##_rz_sp, \
		uml_arm64_gadget_integer_pair_##name##_##mode##_zr_sp, \
		uml_arm64_gadget_integer_pair_##name##_##mode##_zz_sp}
#define UML_INTEGER_PAIR_BASE_MODES(name, mode) IntegerPairBaseModes{ \
	UML_INTEGER_PAIR_REGISTER_MODES(name, mode), \
	UML_INTEGER_PAIR_SP_REGISTER_MODES(name, mode)}
/* Address-mode index 3 is load-only. Reuse index 0 in store tables instead
 * of emitting a symbol the translator can never select. */
#define UML_INTEGER_PAIR_STORE_GADGETS(name) IntegerPairAddressModes{ \
	UML_INTEGER_PAIR_BASE_MODES(name, 0), \
	UML_INTEGER_PAIR_BASE_MODES(name, 1), \
	UML_INTEGER_PAIR_BASE_MODES(name, 2), \
	UML_INTEGER_PAIR_BASE_MODES(name, 0)}
#define UML_INTEGER_PAIR_LOAD_GADGETS(name) IntegerPairAddressModes{ \
	UML_INTEGER_PAIR_BASE_MODES(name, 0), \
	UML_INTEGER_PAIR_BASE_MODES(name, 1), \
	UML_INTEGER_PAIR_BASE_MODES(name, 2), \
	UML_INTEGER_PAIR_BASE_MODES(name, 3)}

[[nodiscard]] const IntegerPairAddressModes *integer_pair_gadgets(
	unsigned operation, bool load) noexcept
{
	static constexpr auto store32 = UML_INTEGER_PAIR_STORE_GADGETS(store32);
	static constexpr auto load32 = UML_INTEGER_PAIR_LOAD_GADGETS(load32);
	static constexpr auto load32s = UML_INTEGER_PAIR_LOAD_GADGETS(load32s);
	static constexpr auto store64 = UML_INTEGER_PAIR_STORE_GADGETS(store64);
	static constexpr auto load64 = UML_INTEGER_PAIR_LOAD_GADGETS(load64);
	if (operation == 1U)
		return load ? &load32s : nullptr;
	if (operation >= 2U)
		return load ? &load64 : &store64;
	return load ? &load32 : &store32;
}

#undef UML_INTEGER_PAIR_LOAD_GADGETS
#undef UML_INTEGER_PAIR_STORE_GADGETS
#undef UML_INTEGER_PAIR_BASE_MODES
#undef UML_INTEGER_PAIR_SP_REGISTER_MODES
#undef UML_INTEGER_PAIR_REGISTER_MODES

using VectorPairBaseModes = std::array<Gadget, 2>;
using VectorPairAddressModes = std::array<VectorPairBaseModes, 3>;

#define UML_VECTOR_PAIR_GADGETS(name) VectorPairAddressModes{ \
	VectorPairBaseModes{uml_arm64_gadget_vector_pair_##name##_0, \
		uml_arm64_gadget_vector_pair_##name##_0_sp}, \
	VectorPairBaseModes{uml_arm64_gadget_vector_pair_##name##_1, \
		uml_arm64_gadget_vector_pair_##name##_1_sp}, \
	VectorPairBaseModes{uml_arm64_gadget_vector_pair_##name##_2, \
		uml_arm64_gadget_vector_pair_##name##_2_sp}}

[[nodiscard]] const VectorPairAddressModes &vector_pair_gadgets(
	unsigned operation, bool load) noexcept
{
	static constexpr auto store32 = UML_VECTOR_PAIR_GADGETS(store32);
	static constexpr auto load32 = UML_VECTOR_PAIR_GADGETS(load32);
	static constexpr auto store64 = UML_VECTOR_PAIR_GADGETS(store64);
	static constexpr auto load64 = UML_VECTOR_PAIR_GADGETS(load64);
	static constexpr auto store128 = UML_VECTOR_PAIR_GADGETS(store128);
	static constexpr auto load128 = UML_VECTOR_PAIR_GADGETS(load128);
	if (operation == 0U)
		return load ? load32 : store32;
	if (operation == 1U)
		return load ? load64 : store64;
	return load ? load128 : store128;
}

#undef UML_VECTOR_PAIR_GADGETS

using SimdStructureAddressModes = std::array<Gadget, 3>;
using SimdStructureBases = std::array<SimdStructureAddressModes, 2>;
struct SimdStructureGadgets final {
	SimdStructureBases store;
	SimdStructureBases load;
};

#define UML_SIMD_STRUCTURE_DIRECTION(base, direction) \
	SimdStructureBases{ \
		SimdStructureAddressModes{ \
			uml_arm64_gadget_simd_structure_##base##_##direction##_0, \
			uml_arm64_gadget_simd_structure_##base##_##direction##_1, \
			uml_arm64_gadget_simd_structure_##base##_##direction##_2}, \
		SimdStructureAddressModes{ \
			uml_arm64_gadget_simd_structure_##base##_##direction##_s0, \
			uml_arm64_gadget_simd_structure_##base##_##direction##_s1, \
			uml_arm64_gadget_simd_structure_##base##_##direction##_s2}}
#define UML_SIMD_STRUCTURE_GADGETS(base) SimdStructureGadgets{ \
	UML_SIMD_STRUCTURE_DIRECTION(base, store), \
	UML_SIMD_STRUCTURE_DIRECTION(base, load)}

[[nodiscard]] const SimdStructureGadgets *simd_structure_gadgets(
	unsigned opcode, bool q, unsigned size) noexcept
{
	static constexpr std::array contiguous_d{
		UML_SIMD_STRUCTURE_GADGETS(contig_d_1),
		UML_SIMD_STRUCTURE_GADGETS(contig_d_2),
		UML_SIMD_STRUCTURE_GADGETS(contig_d_3),
		UML_SIMD_STRUCTURE_GADGETS(contig_d_4)};
	static constexpr std::array contiguous_q{
		UML_SIMD_STRUCTURE_GADGETS(contig_q_1),
		UML_SIMD_STRUCTURE_GADGETS(contig_q_2),
		UML_SIMD_STRUCTURE_GADGETS(contig_q_3),
		UML_SIMD_STRUCTURE_GADGETS(contig_q_4)};
#define UML_SIMD_STRUCTURE_INTERLEAVED_ARRAYS(count) \
	static constexpr std::array interleaved_d_##count{ \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_d_b_##count), \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_d_h_##count), \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_d_s_##count)}; \
	static constexpr std::array interleaved_q_##count{ \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_q_b_##count), \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_q_h_##count), \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_q_s_##count), \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_q_d_##count)}
	UML_SIMD_STRUCTURE_INTERLEAVED_ARRAYS(2);
	UML_SIMD_STRUCTURE_INTERLEAVED_ARRAYS(3);
	UML_SIMD_STRUCTURE_INTERLEAVED_ARRAYS(4);
#undef UML_SIMD_STRUCTURE_INTERLEAVED_ARRAYS
	if (opcode == 7U)
		return &(q ? contiguous_q : contiguous_d)[0];
	if (opcode == 10U)
		return &(q ? contiguous_q : contiguous_d)[1];
	if (opcode == 6U)
		return &(q ? contiguous_q : contiguous_d)[2];
	if (opcode == 2U)
		return &(q ? contiguous_q : contiguous_d)[3];
	if (opcode == 8U)
		return q ? &interleaved_q_2[size] : &interleaved_d_2[size];
	if (opcode == 4U)
		return q ? &interleaved_q_3[size] : &interleaved_d_3[size];
	if (opcode == 0U)
		return q ? &interleaved_q_4[size] : &interleaved_d_4[size];
	return nullptr;
}

#undef UML_SIMD_STRUCTURE_GADGETS
#undef UML_SIMD_STRUCTURE_DIRECTION

using SimdReplicateAddressModes = std::array<Gadget, 3>;
using SimdReplicateBases = std::array<SimdReplicateAddressModes, 2>;
using SimdReplicateCounts = std::array<SimdReplicateBases, 4>;

#define UML_SIMD_REPLICATE_MODES(type, count) SimdReplicateBases{ \
	SimdReplicateAddressModes{ \
		uml_arm64_gadget_simd_replicate_##type##_##count##_0, \
		uml_arm64_gadget_simd_replicate_##type##_##count##_1, \
		uml_arm64_gadget_simd_replicate_##type##_##count##_2}, \
	SimdReplicateAddressModes{ \
		uml_arm64_gadget_simd_replicate_##type##_##count##_s0, \
		uml_arm64_gadget_simd_replicate_##type##_##count##_s1, \
		uml_arm64_gadget_simd_replicate_##type##_##count##_s2}}
#define UML_SIMD_REPLICATE_GADGETS(type) SimdReplicateCounts{ \
	UML_SIMD_REPLICATE_MODES(type, 1), \
	UML_SIMD_REPLICATE_MODES(type, 2), \
	UML_SIMD_REPLICATE_MODES(type, 3), \
	UML_SIMD_REPLICATE_MODES(type, 4)}

[[nodiscard]] const SimdReplicateCounts &simd_replicate_gadgets(
	unsigned size, bool q) noexcept
{
	static constexpr auto b_d = UML_SIMD_REPLICATE_GADGETS(b_d);
	static constexpr auto b_q = UML_SIMD_REPLICATE_GADGETS(b_q);
	static constexpr auto h_d = UML_SIMD_REPLICATE_GADGETS(h_d);
	static constexpr auto h_q = UML_SIMD_REPLICATE_GADGETS(h_q);
	static constexpr auto s_d = UML_SIMD_REPLICATE_GADGETS(s_d);
	static constexpr auto s_q = UML_SIMD_REPLICATE_GADGETS(s_q);
	static constexpr auto d_d = UML_SIMD_REPLICATE_GADGETS(d_d);
	static constexpr auto d_q = UML_SIMD_REPLICATE_GADGETS(d_q);
	static constexpr std::array d{&b_d, &h_d, &s_d, &d_d};
	static constexpr std::array qv{&b_q, &h_q, &s_q, &d_q};
	return *(q ? qv : d)[size];
}

#undef UML_SIMD_REPLICATE_GADGETS
#undef UML_SIMD_REPLICATE_MODES

using SimdLaneAddressModes = std::array<Gadget, 3>;
using SimdLaneBases = std::array<SimdLaneAddressModes, 2>;
struct SimdLaneGadgets final {
	SimdLaneBases store;
	SimdLaneBases load;
};
using SimdLaneCounts = std::array<SimdLaneGadgets, 4>;

#define UML_SIMD_LANE_DIRECTION(type, count, direction) \
	SimdLaneBases{ \
		SimdLaneAddressModes{ \
			uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_0, \
			uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_1, \
			uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_2}, \
		SimdLaneAddressModes{ \
			uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_s0, \
			uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_s1, \
			uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_s2}}
#define UML_SIMD_LANE_COUNT(type, count) SimdLaneGadgets{ \
	UML_SIMD_LANE_DIRECTION(type, count, store), \
	UML_SIMD_LANE_DIRECTION(type, count, load)}
#define UML_SIMD_LANE_GADGETS(type) SimdLaneCounts{ \
	UML_SIMD_LANE_COUNT(type, 1), \
	UML_SIMD_LANE_COUNT(type, 2), \
	UML_SIMD_LANE_COUNT(type, 3), \
	UML_SIMD_LANE_COUNT(type, 4)}

[[nodiscard]] const SimdLaneCounts &simd_lane_gadgets(
	unsigned width_index) noexcept
{
	static constexpr auto b = UML_SIMD_LANE_GADGETS(b);
	static constexpr auto h = UML_SIMD_LANE_GADGETS(h);
	static constexpr auto s = UML_SIMD_LANE_GADGETS(s);
	static constexpr auto d = UML_SIMD_LANE_GADGETS(d);
	static constexpr std::array tables{&b, &h, &s, &d};
	return *tables[width_index];
}

#undef UML_SIMD_LANE_GADGETS
#undef UML_SIMD_LANE_COUNT
#undef UML_SIMD_LANE_DIRECTION

[[nodiscard]] bool host_has_sm3() noexcept
{
	return host_capabilities().has(ArmFeature::sm3);
}

[[nodiscard]] bool host_has_sm4() noexcept
{
	return host_capabilities().has(ArmFeature::sm4);
}

[[nodiscard]] bool host_has_sha1() noexcept
{
	return host_capabilities().has(ArmFeature::sha1);
}

[[nodiscard]] bool host_has_sha256() noexcept
{
	return host_capabilities().has(ArmFeature::sha256);
}

[[nodiscard]] bool host_has_sha512() noexcept
{
	return host_capabilities().has(ArmFeature::sha512);
}

[[nodiscard]] bool host_has_sha3() noexcept
{
	return host_capabilities().has(ArmFeature::sha3);
}

[[nodiscard]] bool host_has_aes() noexcept
{
	return host_capabilities().has(ArmFeature::advanced_simd) &&
		host_capabilities().has(ArmFeature::aes);
}

[[nodiscard]] bool host_has_pmull() noexcept
{
	return host_capabilities().has(ArmFeature::pmull);
}

[[nodiscard]] bool host_has_rdm() noexcept
{
	return host_capabilities().has(ArmFeature::rdm);
}

[[nodiscard]] bool host_has_dotprod() noexcept
{
	return host_capabilities().has(ArmFeature::dotprod);
}

[[nodiscard]] bool host_has_i8mm() noexcept
{
	return host_capabilities().has(ArmFeature::i8mm);
}

[[nodiscard]] bool host_has_lrcpc3() noexcept
{
	return host_capabilities().has(ArmFeature::lrcpc3);
}

[[nodiscard]] bool host_has_lut() noexcept
{
	return host_capabilities().has(ArmFeature::lut);
}

[[nodiscard]] bool host_has_frintts() noexcept
{
	return host_capabilities().has(ArmFeature::frintts);
}

[[nodiscard]] bool host_has_afp() noexcept
{
	return host_capabilities().afp;
}

[[nodiscard]] bool host_has_faminmax() noexcept
{
	return host_capabilities().has(ArmFeature::faminmax);
}

[[nodiscard]] bool host_has_fp8() noexcept
{
	return host_capabilities().has(ArmFeature::fp8);
}

[[nodiscard]] bool host_has_fp8dot2() noexcept
{
	return host_capabilities().has(ArmFeature::fp8dot2);
}

[[nodiscard]] bool host_has_fp8dot4() noexcept
{
	return host_capabilities().has(ArmFeature::fp8dot4);
}

[[nodiscard]] bool host_has_fp8fma() noexcept
{
	return host_capabilities().has(ArmFeature::fp8fma);
}

[[nodiscard]] bool host_has_f8f16mm() noexcept
{
	return host_capabilities().has(ArmFeature::f8f16mm);
}

[[nodiscard]] bool host_has_f8f32mm() noexcept
{
	return host_capabilities().has(ArmFeature::f8f32mm);
}

[[nodiscard]] bool host_has_bf16() noexcept
{
	return host_capabilities().has(ArmFeature::bf16);
}

[[nodiscard]] bool host_has_ebf16() noexcept
{
	return host_capabilities().has(ArmFeature::ebf16);
}

[[nodiscard]] bool host_has_fhm() noexcept
{
	return host_capabilities().has(ArmFeature::fhm);
}

[[nodiscard]] bool host_has_f16mm() noexcept
{
	return host_capabilities().has(ArmFeature::f16mm);
}

[[nodiscard]] bool host_has_f16f32mm() noexcept
{
	return host_capabilities().has(ArmFeature::f16f32mm);
}

[[nodiscard]] bool host_has_f16f32dot() noexcept
{
	return host_capabilities().has(ArmFeature::f16f32dot);
}

[[nodiscard]] bool host_has_lsfe() noexcept
{
	return host_capabilities().has(ArmFeature::lsfe);
}

using AtomicFloatingVariants = std::array<Gadget, 6>;
using AtomicFloatingBases = std::array<AtomicFloatingVariants, 2>;
using AtomicFloatingOperations = std::array<AtomicFloatingBases, 5>;
using AtomicFloatingGadgets = std::array<AtomicFloatingOperations, 4>;

#define UML_ATOMIC_FLOATING_VARIANTS(implementation, size, operation) \
	AtomicFloatingBases{ \
		AtomicFloatingVariants{ \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_relaxed, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_release, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire_release, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_relaxed, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_release}, \
		AtomicFloatingVariants{ \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_relaxed_sp, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_release_sp, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire_sp, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire_release_sp, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_relaxed_sp, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_release_sp}}
#define UML_ATOMIC_FLOATING_OPERATIONS(implementation, size) \
	AtomicFloatingOperations{ \
		UML_ATOMIC_FLOATING_VARIANTS(implementation, size, add), \
		UML_ATOMIC_FLOATING_VARIANTS(implementation, size, max), \
		UML_ATOMIC_FLOATING_VARIANTS(implementation, size, maxnm), \
		UML_ATOMIC_FLOATING_VARIANTS(implementation, size, min), \
		UML_ATOMIC_FLOATING_VARIANTS(implementation, size, minnm)}
#define UML_ATOMIC_FLOATING_GADGETS(implementation) AtomicFloatingGadgets{ \
	UML_ATOMIC_FLOATING_OPERATIONS(implementation, bf), \
	UML_ATOMIC_FLOATING_OPERATIONS(implementation, h), \
	UML_ATOMIC_FLOATING_OPERATIONS(implementation, s), \
	UML_ATOMIC_FLOATING_OPERATIONS(implementation, d)}

constexpr auto atomic_floating_native_gadgets =
	UML_ATOMIC_FLOATING_GADGETS(native);
constexpr auto atomic_floating_software_gadgets =
	UML_ATOMIC_FLOATING_GADGETS(software);

#undef UML_ATOMIC_FLOATING_GADGETS
#undef UML_ATOMIC_FLOATING_OPERATIONS
#undef UML_ATOMIC_FLOATING_VARIANTS

using AtomicRegisterGadgets = std::array<Gadget, 4>;
using AtomicSizeGadgets = std::array<AtomicRegisterGadgets, 4>;
using AtomicOrderGadgets = std::array<AtomicSizeGadgets, 4>;
using AtomicOperationGadgets = std::array<AtomicOrderGadgets, 9>;

#define UML_ATOMIC_REGISTER_GADGETS(operation, ordering, size) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_rr, \
		uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_rz, \
		uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_zr, \
		uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_zz}
#define UML_ATOMIC_SIZE_GADGETS(operation, ordering) \
	AtomicSizeGadgets{ \
		UML_ATOMIC_REGISTER_GADGETS(operation, ordering, b), \
		UML_ATOMIC_REGISTER_GADGETS(operation, ordering, h), \
		UML_ATOMIC_REGISTER_GADGETS(operation, ordering, w), \
		UML_ATOMIC_REGISTER_GADGETS(operation, ordering, x)}
#define UML_ATOMIC_ORDER_GADGETS(operation) \
	AtomicOrderGadgets{ \
		UML_ATOMIC_SIZE_GADGETS(operation, relaxed), \
		UML_ATOMIC_SIZE_GADGETS(operation, release), \
		UML_ATOMIC_SIZE_GADGETS(operation, acquire), \
		UML_ATOMIC_SIZE_GADGETS(operation, acquire_release)}

constexpr AtomicOperationGadgets atomic_rmw_gadgets{
	UML_ATOMIC_ORDER_GADGETS(ldadd),
	UML_ATOMIC_ORDER_GADGETS(ldclr),
	UML_ATOMIC_ORDER_GADGETS(ldeor),
	UML_ATOMIC_ORDER_GADGETS(ldset),
	UML_ATOMIC_ORDER_GADGETS(ldsmax),
	UML_ATOMIC_ORDER_GADGETS(ldsmin),
	UML_ATOMIC_ORDER_GADGETS(ldumax),
	UML_ATOMIC_ORDER_GADGETS(ldumin),
	UML_ATOMIC_ORDER_GADGETS(swp),
};

#undef UML_ATOMIC_ORDER_GADGETS
#undef UML_ATOMIC_SIZE_GADGETS
#undef UML_ATOMIC_REGISTER_GADGETS

using AtomicPair128OrderGadgets = std::array<Gadget, 4>;
using AtomicPair128Gadgets = std::array<AtomicPair128OrderGadgets, 3>;
#define UML_ATOMIC_PAIR128_GADGETS(operation) \
	AtomicPair128OrderGadgets{ \
		uml_arm64_gadget_atomic_pair128_##operation##_relaxed, \
		uml_arm64_gadget_atomic_pair128_##operation##_release, \
		uml_arm64_gadget_atomic_pair128_##operation##_acquire, \
		uml_arm64_gadget_atomic_pair128_##operation##_acquire_release}
constexpr AtomicPair128Gadgets atomic_pair128_gadgets{
	UML_ATOMIC_PAIR128_GADGETS(clr),
	UML_ATOMIC_PAIR128_GADGETS(set),
	UML_ATOMIC_PAIR128_GADGETS(swp),
};
#undef UML_ATOMIC_PAIR128_GADGETS

using CompareSwapGadgets = std::array<AtomicSizeGadgets, 4>;
using CompareSwapPairSizeGadgets = std::array<AtomicRegisterGadgets, 2>;
using CompareSwapPairGadgets = std::array<CompareSwapPairSizeGadgets, 4>;

#define UML_COMPARE_SWAP_REGISTER_GADGETS(ordering, size) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_compare_swap_##ordering##_##size##_rr, \
		uml_arm64_gadget_compare_swap_##ordering##_##size##_rz, \
		uml_arm64_gadget_compare_swap_##ordering##_##size##_zr, \
		uml_arm64_gadget_compare_swap_##ordering##_##size##_zz}
#define UML_COMPARE_SWAP_SIZE_GADGETS(ordering) \
	AtomicSizeGadgets{ \
		UML_COMPARE_SWAP_REGISTER_GADGETS(ordering, b), \
		UML_COMPARE_SWAP_REGISTER_GADGETS(ordering, h), \
		UML_COMPARE_SWAP_REGISTER_GADGETS(ordering, w), \
		UML_COMPARE_SWAP_REGISTER_GADGETS(ordering, x)}
constexpr CompareSwapGadgets compare_swap_gadgets{
	UML_COMPARE_SWAP_SIZE_GADGETS(relaxed),
	UML_COMPARE_SWAP_SIZE_GADGETS(release),
	UML_COMPARE_SWAP_SIZE_GADGETS(acquire),
	UML_COMPARE_SWAP_SIZE_GADGETS(acquire_release),
};
#undef UML_COMPARE_SWAP_SIZE_GADGETS
#undef UML_COMPARE_SWAP_REGISTER_GADGETS

using RcwScalarOrderingGadgets = std::array<AtomicRegisterGadgets, 4>;
using RcwScalarSoftGadgets = std::array<RcwScalarOrderingGadgets, 2>;
using RcwScalarGadgets = std::array<RcwScalarSoftGadgets, 4>;
#define UML_RCW_SCALAR_REGISTER_GADGETS(operation, soft, ordering) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rr, \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rz, \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zr, \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zz}
#define UML_RCW_SCALAR_ACQUIRE_GADGETS(operation, soft, ordering, suppressed) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rr, \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##suppressed##_rz, \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zr, \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##suppressed##_zz}
#define UML_RCW_SCALAR_ORDERING_GADGETS(operation, soft) \
	RcwScalarOrderingGadgets{ \
		UML_RCW_SCALAR_REGISTER_GADGETS(operation, soft, relaxed), \
		UML_RCW_SCALAR_REGISTER_GADGETS(operation, soft, release), \
		UML_RCW_SCALAR_ACQUIRE_GADGETS( \
			operation, soft, acquire, relaxed), \
		UML_RCW_SCALAR_ACQUIRE_GADGETS( \
			operation, soft, acquire_release, release)}
#define UML_RCW_SCALAR_SOFT_GADGETS(operation) RcwScalarSoftGadgets{ \
	UML_RCW_SCALAR_ORDERING_GADGETS(operation, hard), \
	UML_RCW_SCALAR_ORDERING_GADGETS(operation, soft)}
constexpr RcwScalarGadgets rcw_scalar_gadgets{
	UML_RCW_SCALAR_SOFT_GADGETS(cas),
	UML_RCW_SCALAR_SOFT_GADGETS(bic),
	UML_RCW_SCALAR_SOFT_GADGETS(swp),
	UML_RCW_SCALAR_SOFT_GADGETS(orr),
};
#undef UML_RCW_SCALAR_SOFT_GADGETS
#undef UML_RCW_SCALAR_ORDERING_GADGETS
#undef UML_RCW_SCALAR_ACQUIRE_GADGETS
#undef UML_RCW_SCALAR_REGISTER_GADGETS

using RcwPairCasOrderingGadgets = std::array<AtomicRegisterGadgets, 4>;
using RcwPairCasGadgets = std::array<RcwPairCasOrderingGadgets, 2>;
#define UML_RCW_PAIR_CAS_VARIANTS(soft, ordering) AtomicRegisterGadgets{ \
	uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_rr, \
	uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_rz, \
	uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_zr, \
	uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_zz}
#define UML_RCW_PAIR_CAS_ORDERINGS(soft) RcwPairCasOrderingGadgets{ \
	UML_RCW_PAIR_CAS_VARIANTS(soft, relaxed), \
	UML_RCW_PAIR_CAS_VARIANTS(soft, release), \
	UML_RCW_PAIR_CAS_VARIANTS(soft, acquire), \
	UML_RCW_PAIR_CAS_VARIANTS(soft, acquire_release)}
constexpr RcwPairCasGadgets rcw_pair_cas_gadgets{
	UML_RCW_PAIR_CAS_ORDERINGS(hard),
	UML_RCW_PAIR_CAS_ORDERINGS(soft),
};
#undef UML_RCW_PAIR_CAS_ORDERINGS
#undef UML_RCW_PAIR_CAS_VARIANTS

using RcwPairModifyOrderingGadgets = std::array<Gadget, 4>;
using RcwPairModifySoftGadgets =
	std::array<RcwPairModifyOrderingGadgets, 2>;
using RcwPairModifyGadgets = std::array<RcwPairModifySoftGadgets, 3>;
#define UML_RCW_PAIR_MODIFY_ORDERINGS(operation, soft) \
	RcwPairModifyOrderingGadgets{ \
		uml_arm64_gadget_rcw_pair_##operation##_##soft##_relaxed, \
		uml_arm64_gadget_rcw_pair_##operation##_##soft##_release, \
		uml_arm64_gadget_rcw_pair_##operation##_##soft##_acquire, \
		uml_arm64_gadget_rcw_pair_##operation##_##soft##_acquire_release}
#define UML_RCW_PAIR_MODIFY_SOFTNESS(operation) RcwPairModifySoftGadgets{ \
	UML_RCW_PAIR_MODIFY_ORDERINGS(operation, hard), \
	UML_RCW_PAIR_MODIFY_ORDERINGS(operation, soft)}
constexpr RcwPairModifyGadgets rcw_pair_modify_gadgets{
	UML_RCW_PAIR_MODIFY_SOFTNESS(bic),
	UML_RCW_PAIR_MODIFY_SOFTNESS(swp),
	UML_RCW_PAIR_MODIFY_SOFTNESS(orr),
};
#undef UML_RCW_PAIR_MODIFY_SOFTNESS
#undef UML_RCW_PAIR_MODIFY_ORDERINGS

#define UML_COMPARE_SWAP_PAIR_REGISTER_GADGETS(ordering, size) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_rr, \
		uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_rz, \
		uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_zr, \
		uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_zz}
#define UML_COMPARE_SWAP_PAIR_SIZE_GADGETS(ordering) \
	CompareSwapPairSizeGadgets{ \
		UML_COMPARE_SWAP_PAIR_REGISTER_GADGETS(ordering, w), \
		UML_COMPARE_SWAP_PAIR_REGISTER_GADGETS(ordering, x)}
constexpr CompareSwapPairGadgets compare_swap_pair_gadgets{
	UML_COMPARE_SWAP_PAIR_SIZE_GADGETS(relaxed),
	UML_COMPARE_SWAP_PAIR_SIZE_GADGETS(release),
	UML_COMPARE_SWAP_PAIR_SIZE_GADGETS(acquire),
	UML_COMPARE_SWAP_PAIR_SIZE_GADGETS(acquire_release),
};
#undef UML_COMPARE_SWAP_PAIR_SIZE_GADGETS
#undef UML_COMPARE_SWAP_PAIR_REGISTER_GADGETS

using ExclusiveLoadRegisterGadgets = std::array<Gadget, 2>;
using ExclusiveLoadSizeGadgets = std::array<ExclusiveLoadRegisterGadgets, 4>;
using ExclusiveLoadGadgets = std::array<ExclusiveLoadSizeGadgets, 2>;
using ExclusiveStoreGadgets = std::array<AtomicSizeGadgets, 2>;
using ExclusivePairLoadSizeGadgets =
	std::array<AtomicRegisterGadgets, 2>;
using ExclusivePairLoadGadgets =
	std::array<ExclusivePairLoadSizeGadgets, 2>;
using ExclusivePairStoreRegisterGadgets = std::array<Gadget, 8>;
using ExclusivePairStoreSizeGadgets =
	std::array<ExclusivePairStoreRegisterGadgets, 2>;
using ExclusivePairStoreGadgets =
	std::array<ExclusivePairStoreSizeGadgets, 2>;

#define UML_EXCLUSIVE_LOAD_REGISTER_GADGETS(ordering, size) \
	ExclusiveLoadRegisterGadgets{ \
		uml_arm64_gadget_exclusive_load_##ordering##_##size##_r, \
		uml_arm64_gadget_exclusive_load_##ordering##_##size##_z}
#define UML_EXCLUSIVE_LOAD_SIZE_GADGETS(ordering) \
	ExclusiveLoadSizeGadgets{ \
		UML_EXCLUSIVE_LOAD_REGISTER_GADGETS(ordering, b), \
		UML_EXCLUSIVE_LOAD_REGISTER_GADGETS(ordering, h), \
		UML_EXCLUSIVE_LOAD_REGISTER_GADGETS(ordering, w), \
		UML_EXCLUSIVE_LOAD_REGISTER_GADGETS(ordering, x)}
constexpr ExclusiveLoadGadgets exclusive_load_gadgets{
	UML_EXCLUSIVE_LOAD_SIZE_GADGETS(relaxed),
	UML_EXCLUSIVE_LOAD_SIZE_GADGETS(acquire),
};
#undef UML_EXCLUSIVE_LOAD_SIZE_GADGETS
#undef UML_EXCLUSIVE_LOAD_REGISTER_GADGETS

#define UML_EXCLUSIVE_STORE_REGISTER_GADGETS(ordering, size) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_exclusive_store_##ordering##_##size##_rr, \
		uml_arm64_gadget_exclusive_store_##ordering##_##size##_rz, \
		uml_arm64_gadget_exclusive_store_##ordering##_##size##_zr, \
		uml_arm64_gadget_exclusive_store_##ordering##_##size##_zz}
#define UML_EXCLUSIVE_STORE_SIZE_GADGETS(ordering) \
	AtomicSizeGadgets{ \
		UML_EXCLUSIVE_STORE_REGISTER_GADGETS(ordering, b), \
		UML_EXCLUSIVE_STORE_REGISTER_GADGETS(ordering, h), \
		UML_EXCLUSIVE_STORE_REGISTER_GADGETS(ordering, w), \
		UML_EXCLUSIVE_STORE_REGISTER_GADGETS(ordering, x)}
constexpr ExclusiveStoreGadgets exclusive_store_gadgets{
	UML_EXCLUSIVE_STORE_SIZE_GADGETS(relaxed),
	UML_EXCLUSIVE_STORE_SIZE_GADGETS(release),
};
#undef UML_EXCLUSIVE_STORE_SIZE_GADGETS
#undef UML_EXCLUSIVE_STORE_REGISTER_GADGETS

#define UML_EXCLUSIVE_PAIR_LOAD_REGISTER_GADGETS(ordering, size) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_rr, \
		uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_rz, \
		uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_zr, \
		uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_zz}
#define UML_EXCLUSIVE_PAIR_LOAD_SIZE_GADGETS(ordering) \
	ExclusivePairLoadSizeGadgets{ \
		UML_EXCLUSIVE_PAIR_LOAD_REGISTER_GADGETS(ordering, w), \
		UML_EXCLUSIVE_PAIR_LOAD_REGISTER_GADGETS(ordering, x)}
constexpr ExclusivePairLoadGadgets exclusive_pair_load_gadgets{
	UML_EXCLUSIVE_PAIR_LOAD_SIZE_GADGETS(relaxed),
	UML_EXCLUSIVE_PAIR_LOAD_SIZE_GADGETS(acquire),
};
#undef UML_EXCLUSIVE_PAIR_LOAD_SIZE_GADGETS
#undef UML_EXCLUSIVE_PAIR_LOAD_REGISTER_GADGETS

#define UML_EXCLUSIVE_PAIR_STORE_REGISTER_GADGETS(ordering, size) \
	ExclusivePairStoreRegisterGadgets{ \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rrr, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rrz, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rzr, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rzz, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zrr, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zrz, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zzr, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zzz}
#define UML_EXCLUSIVE_PAIR_STORE_SIZE_GADGETS(ordering) \
	ExclusivePairStoreSizeGadgets{ \
		UML_EXCLUSIVE_PAIR_STORE_REGISTER_GADGETS(ordering, w), \
		UML_EXCLUSIVE_PAIR_STORE_REGISTER_GADGETS(ordering, x)}
constexpr ExclusivePairStoreGadgets exclusive_pair_store_gadgets{
	UML_EXCLUSIVE_PAIR_STORE_SIZE_GADGETS(relaxed),
	UML_EXCLUSIVE_PAIR_STORE_SIZE_GADGETS(release),
};
#undef UML_EXCLUSIVE_PAIR_STORE_SIZE_GADGETS
#undef UML_EXCLUSIVE_PAIR_STORE_REGISTER_GADGETS

using OrderedLoadGadgets = ExclusiveLoadGadgets;
using OrderedStoreGadgets = ExclusiveLoadGadgets;

#define UML_ORDERED_REGISTER_GADGETS(kind, ordering, size) \
	ExclusiveLoadRegisterGadgets{ \
		uml_arm64_gadget_ordered_##kind##_##ordering##_##size##_r, \
		uml_arm64_gadget_ordered_##kind##_##ordering##_##size##_z}
#define UML_ORDERED_SIZE_GADGETS(kind, ordering) \
	ExclusiveLoadSizeGadgets{ \
		UML_ORDERED_REGISTER_GADGETS(kind, ordering, b), \
		UML_ORDERED_REGISTER_GADGETS(kind, ordering, h), \
		UML_ORDERED_REGISTER_GADGETS(kind, ordering, w), \
		UML_ORDERED_REGISTER_GADGETS(kind, ordering, x)}
constexpr OrderedLoadGadgets ordered_load_gadgets{
	UML_ORDERED_SIZE_GADGETS(load, relaxed),
	UML_ORDERED_SIZE_GADGETS(load, acquire),
};
constexpr OrderedStoreGadgets ordered_store_gadgets{
	UML_ORDERED_SIZE_GADGETS(store, limited),
	UML_ORDERED_SIZE_GADGETS(store, release),
};
#undef UML_ORDERED_SIZE_GADGETS
#undef UML_ORDERED_REGISTER_GADGETS

using RcpcRegisterGadgets = std::array<Gadget, 2>;
using RcpcLoadGadgets = std::array<RcpcRegisterGadgets, 9>;
using RcpcStoreGadgets = std::array<RcpcRegisterGadgets, 4>;
#define UML_RCPC_REGISTER_GADGETS(kind, name) \
	RcpcRegisterGadgets{ \
		uml_arm64_gadget_rcpc_##kind##_##name##_r, \
		uml_arm64_gadget_rcpc_##kind##_##name##_z}
constexpr RcpcLoadGadgets rcpc_load_gadgets{
	UML_RCPC_REGISTER_GADGETS(load, zero_b),
	UML_RCPC_REGISTER_GADGETS(load, zero_h),
	UML_RCPC_REGISTER_GADGETS(load, zero_w),
	UML_RCPC_REGISTER_GADGETS(load, zero_x),
	UML_RCPC_REGISTER_GADGETS(load, sb_w),
	UML_RCPC_REGISTER_GADGETS(load, sb_x),
	UML_RCPC_REGISTER_GADGETS(load, sh_w),
	UML_RCPC_REGISTER_GADGETS(load, sh_x),
	UML_RCPC_REGISTER_GADGETS(load, sw_x),
};
constexpr RcpcStoreGadgets rcpc_store_gadgets{
	UML_RCPC_REGISTER_GADGETS(store, b),
	UML_RCPC_REGISTER_GADGETS(store, h),
	UML_RCPC_REGISTER_GADGETS(store, w),
	UML_RCPC_REGISTER_GADGETS(store, x),
};
#undef UML_RCPC_REGISTER_GADGETS

using SimdOrderedAddressGadgets = std::array<Gadget, 2>;
using SimdOrderedScalarGadgets =
	std::array<SimdOrderedAddressGadgets, 4>;
#define UML_SIMD_ORDERED_ADDRESS_GADGETS(direction, size) \
	SimdOrderedAddressGadgets{ \
		uml_arm64_gadget_simd_ordered_##direction##_##size##_r, \
		uml_arm64_gadget_simd_ordered_##direction##_##size##_s}
constexpr SimdOrderedScalarGadgets simd_ordered_scalar_load_gadgets{
	UML_SIMD_ORDERED_ADDRESS_GADGETS(load, b),
	UML_SIMD_ORDERED_ADDRESS_GADGETS(load, h),
	UML_SIMD_ORDERED_ADDRESS_GADGETS(load, s),
	UML_SIMD_ORDERED_ADDRESS_GADGETS(load, d),
};
constexpr SimdOrderedScalarGadgets simd_ordered_scalar_store_gadgets{
	UML_SIMD_ORDERED_ADDRESS_GADGETS(store, b),
	UML_SIMD_ORDERED_ADDRESS_GADGETS(store, h),
	UML_SIMD_ORDERED_ADDRESS_GADGETS(store, s),
	UML_SIMD_ORDERED_ADDRESS_GADGETS(store, d),
};
#undef UML_SIMD_ORDERED_ADDRESS_GADGETS

using SimdOrderedImplementationGadgets =
	std::array<SimdOrderedAddressGadgets, 2>;
constexpr SimdOrderedImplementationGadgets simd_ordered_q_load_gadgets{
	SimdOrderedAddressGadgets{
		uml_arm64_gadget_simd_ordered_load_q_r_fallback,
		uml_arm64_gadget_simd_ordered_load_q_s_fallback},
	SimdOrderedAddressGadgets{
		uml_arm64_gadget_simd_ordered_load_q_r_native,
		uml_arm64_gadget_simd_ordered_load_q_s_native},
};
constexpr SimdOrderedImplementationGadgets simd_ordered_q_store_gadgets{
	SimdOrderedAddressGadgets{
		uml_arm64_gadget_simd_ordered_store_q_r_fallback,
		uml_arm64_gadget_simd_ordered_store_q_s_fallback},
	SimdOrderedAddressGadgets{
		uml_arm64_gadget_simd_ordered_store_q_r_native,
		uml_arm64_gadget_simd_ordered_store_q_s_native},
};

using SimdOrderedLaneGadgets =
	std::array<std::array<SimdOrderedAddressGadgets, 2>, 2>;
constexpr SimdOrderedLaneGadgets simd_ordered_lane_gadgets{
	std::array<SimdOrderedAddressGadgets, 2>{
		SimdOrderedAddressGadgets{
			uml_arm64_gadget_simd_ordered_store_lane0_r,
			uml_arm64_gadget_simd_ordered_store_lane0_s},
		SimdOrderedAddressGadgets{
			uml_arm64_gadget_simd_ordered_store_lane1_r,
			uml_arm64_gadget_simd_ordered_store_lane1_s}},
	std::array<SimdOrderedAddressGadgets, 2>{
		SimdOrderedAddressGadgets{
			uml_arm64_gadget_simd_ordered_load_lane0_r,
			uml_arm64_gadget_simd_ordered_load_lane0_s},
		SimdOrderedAddressGadgets{
			uml_arm64_gadget_simd_ordered_load_lane1_r,
			uml_arm64_gadget_simd_ordered_load_lane1_s}},
};

using OrderedPairRegisterGadgets = std::array<Gadget, 4>;
using OrderedPairAddressGadgets =
	std::array<OrderedPairRegisterGadgets, 2>;
using OrderedPairGadgets = std::array<OrderedPairAddressGadgets, 2>;
#define UML_ORDERED_PAIR_REGISTER_GADGETS(kind, size, address) \
	OrderedPairRegisterGadgets{ \
		uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_rr, \
		uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_rz, \
		uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_zr, \
		uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_zz}
constexpr OrderedPairGadgets ordered_pair_load_gadgets{
	OrderedPairAddressGadgets{
		UML_ORDERED_PAIR_REGISTER_GADGETS(load, w, no),
		UML_ORDERED_PAIR_REGISTER_GADGETS(load, w, post)},
	OrderedPairAddressGadgets{
		UML_ORDERED_PAIR_REGISTER_GADGETS(load, x, no),
		UML_ORDERED_PAIR_REGISTER_GADGETS(load, x, post)},
};
constexpr OrderedPairGadgets ordered_pair_store_gadgets{
	OrderedPairAddressGadgets{
		UML_ORDERED_PAIR_REGISTER_GADGETS(store, w, no),
		UML_ORDERED_PAIR_REGISTER_GADGETS(store, w, pre)},
	OrderedPairAddressGadgets{
		UML_ORDERED_PAIR_REGISTER_GADGETS(store, x, no),
		UML_ORDERED_PAIR_REGISTER_GADGETS(store, x, pre)},
};
#undef UML_ORDERED_PAIR_REGISTER_GADGETS

using ConsistentPairGadgets = std::array<Gadget, 4>;
#define UML_CONSISTENT_PAIR_GADGETS(kind) ConsistentPairGadgets{ \
	uml_arm64_gadget_consistent_pair_##kind##_rr, \
	uml_arm64_gadget_consistent_pair_##kind##_rz, \
	uml_arm64_gadget_consistent_pair_##kind##_zr, \
	uml_arm64_gadget_consistent_pair_##kind##_zz}
constexpr auto consistent_pair_ldap_gadgets =
	UML_CONSISTENT_PAIR_GADGETS(ldap);
constexpr auto consistent_pair_ldapp_gadgets =
	UML_CONSISTENT_PAIR_GADGETS(ldapp);
constexpr auto consistent_pair_store_gadgets =
	UML_CONSISTENT_PAIR_GADGETS(store);
#undef UML_CONSISTENT_PAIR_GADGETS

static_assert(offsetof(CpuState, registers) == 0);
static_assert(offsetof(CpuState, sp) == 248);
static_assert(offsetof(CpuState, pc) == 256);
static_assert(offsetof(CpuState, nzcv) == 264);
static_assert(offsetof(CpuState, vectors) == 272);
static_assert(offsetof(CpuState, fpcr) == 784);
static_assert(offsetof(CpuState, fpsr) == 788);
static_assert(offsetof(CpuState, tls) == 792);
static_assert(offsetof(CpuState, mte_enabled) == 800);
static_assert(offsetof(CpuState, mte_exclude) == 802);
static_assert(offsetof(CpuState, mte_tag_choices) == 804);
static_assert(offsetof(CpuState, mte_random_tags) == 1060);
static_assert(offsetof(CpuState, mte_seed) == 1062);
static_assert(offsetof(CpuState, mte_tag) == 1064);
static_assert(offsetof(CpuState, mte_random_state) == 1072);
static_assert(offsetof(CpuState, cpa_enabled) == 1080);
static_assert(offsetof(CpuState, cpa_multiply_enabled) == 1081);
static_assert(offsetof(CpuState, accdata) == 1084);
static_assert(offsetof(CpuState, pstate_ssbs) == 1088);
static_assert(offsetof(CpuState, pstate_dit) == 1089);
static_assert(offsetof(CpuState, pstate_tco) == 1090);
static_assert(offsetof(CpuState, pstate_sm) == 1091);
static_assert(offsetof(CpuState, pstate_za) == 1092);
static_assert(offsetof(CpuState, pstate_daif_access) == 1093);
static_assert(offsetof(CpuState, pstate_daif) == 1094);
static_assert(offsetof(CpuState, exclusive_address) == 1096);
static_assert(offsetof(CpuState, exclusive_key) == 1104);
static_assert(offsetof(CpuState, exclusive_value) == 1112);
static_assert(offsetof(CpuState, exclusive_value2) == 1120);
static_assert(offsetof(CpuState, exclusive_version) == 1128);
static_assert(offsetof(CpuState, exclusive_size) == 1136);
static_assert(offsetof(CpuState, deferred_vectors) == 1144);
static_assert(offsetof(CpuState, deferred_fpcr) == 1152);
static_assert(offsetof(CpuState, deferred_fpsr) == 1156);
static_assert(offsetof(CpuState, fp_loaded) == 1160);
static_assert(offsetof(CpuState, fp_dirty) == 1161);
static_assert(offsetof(CpuState, fpmr) == 1168);
static_assert(offsetof(CpuState, gadget_zero) == 1176);
static_assert(offsetof(CpuState, gadget_host_fpcr) == 1184);
static_assert(offsetof(CpuState, gadget_host_fpsr) == 1192);
static_assert(offsetof(CpuState, gadget_fp_active) == 1200);
static_assert(offsetof(CpuState, gadget_indirect_miss) == 1208);
static_assert(offsetof(CpuState, gadget_host_fpmr) == 1216);
static_assert(offsetof(CpuState, gadget_fpmr_active) == 1224);
static_assert(offsetof(CpuState, gcs_pointer) == 1232);
static_assert(offsetof(CpuState, gcs_implemented) == 1240);
static_assert(offsetof(CpuState, gcs_push_enabled) == 1241);
static_assert(offsetof(CpuState, gcs_store_enabled) == 1242);

struct GcsGadget final {
	Gadget entry{};
	std::uintptr_t operand{};
	bool has_operand{};
	bool always_traps{};
};

[[nodiscard, maybe_unused]] constexpr std::optional<GcsGadget>
gcs_gadget(std::uint32_t instruction) noexcept
{
	using uml::cpu::arm64::detail::GcsOperation;
	const auto decoded = uml::cpu::arm64::detail::decode_gcs(instruction);
	if (!decoded)
		return std::nullopt;
	if (uml::cpu::arm64::detail::
	    gcs_is_privileged_exception_operation(decoded.operation))
		return GcsGadget{uml_arm64_gadget_illegal, 0, false, true};

	const auto register_offset = [](unsigned index) constexpr {
		return static_cast<std::uintptr_t>(
			offsetof(CpuState, registers) + index * sizeof(std::uint64_t));
	};
	const bool zero = decoded.transfer == 31U;
	switch (decoded.operation) {
	case GcsOperation::pop_main:
		return GcsGadget{
			zero ? uml_arm64_gadget_gcs_popm_z :
				uml_arm64_gadget_gcs_popm_r,
			zero ? 0 : register_offset(decoded.transfer), !zero, false};
	case GcsOperation::push_main:
		return GcsGadget{
			zero ? uml_arm64_gadget_gcs_pushm_z :
				uml_arm64_gadget_gcs_pushm_r,
			zero ? 0 : register_offset(decoded.transfer), !zero, false};
	case GcsOperation::switch_stack_1:
		return GcsGadget{
			zero ? uml_arm64_gadget_gcs_ss1_z :
				uml_arm64_gadget_gcs_ss1_r,
			zero ? 0 : register_offset(decoded.transfer), !zero, false};
	case GcsOperation::switch_stack_2:
		return GcsGadget{
			zero ? uml_arm64_gadget_gcs_ss2_z :
				uml_arm64_gadget_gcs_ss2_r,
			zero ? 0 : register_offset(decoded.transfer), !zero, false};
	case GcsOperation::store:
	case GcsOperation::store_unprivileged: {
		const bool stack = decoded.base == 31U;
		const std::array<std::array<Gadget, 2>, 2> stores{{
			{uml_arm64_gadget_gcs_store_rr,
			 uml_arm64_gadget_gcs_store_rz},
			{uml_arm64_gadget_gcs_store_sr,
			 uml_arm64_gadget_gcs_store_sz},
		}};
		const auto base_offset = stack ? offsetof(CpuState, sp) :
			register_offset(decoded.base);
		const auto source_offset = zero ? 0 :
			register_offset(decoded.transfer);
		return GcsGadget{stores[stack][zero],
			base_offset | (source_offset << 16), true, false};
	}
	case GcsOperation::none:
	case GcsOperation::pop_exception_context:
	case GcsOperation::pop_exception:
	case GcsOperation::push_exception:
		break;
	}
	return std::nullopt;
}

/* Rows are PACIA, PACIB, PACDA, PACDB, AUTIA, AUTIB, AUTDA, AUTDB;
 * columns are register/SP modifier and architecturally-zero modifier. */
constexpr std::array<std::array<Gadget, 2>, 8> pointer_auth_gadgets{{
	{uml_arm64_gadget_pauth_pacia_r, uml_arm64_gadget_pauth_pacia_z},
	{uml_arm64_gadget_pauth_pacib_r, uml_arm64_gadget_pauth_pacib_z},
	{uml_arm64_gadget_pauth_pacda_r, uml_arm64_gadget_pauth_pacda_z},
	{uml_arm64_gadget_pauth_pacdb_r, uml_arm64_gadget_pauth_pacdb_z},
	{uml_arm64_gadget_pauth_autia_r, uml_arm64_gadget_pauth_autia_z},
	{uml_arm64_gadget_pauth_autib_r, uml_arm64_gadget_pauth_autib_z},
	{uml_arm64_gadget_pauth_autda_r, uml_arm64_gadget_pauth_autda_z},
	{uml_arm64_gadget_pauth_autdb_r, uml_arm64_gadget_pauth_autdb_z},
}};

struct PointerAuthOperation final {
	unsigned operation;
	unsigned destination;
	unsigned modifier;
	bool zero_modifier;
};

[[nodiscard]] constexpr std::optional<PointerAuthOperation>
decode_pointer_auth_hint(std::uint32_t instruction) noexcept
{
	if ((instruction & 0xffff'f01fU) != 0xd503'201fU)
		return std::nullopt;
	switch ((instruction >> 5) & 0x7fU) {
	case 8: return PointerAuthOperation{0, 17, 16, false}; // PACIA1716
	case 10: return PointerAuthOperation{1, 17, 16, false}; // PACIB1716
	case 12: return PointerAuthOperation{4, 17, 16, false}; // AUTIA1716
	case 14: return PointerAuthOperation{5, 17, 16, false}; // AUTIB1716
	case 24: return PointerAuthOperation{0, 30, 0, true}; // PACIAZ
	case 25: return PointerAuthOperation{0, 30, 31, false}; // PACIASP
	case 26: return PointerAuthOperation{1, 30, 0, true}; // PACIBZ
	case 27: return PointerAuthOperation{1, 30, 31, false}; // PACIBSP
	case 28: return PointerAuthOperation{4, 30, 0, true}; // AUTIAZ
	case 29: return PointerAuthOperation{4, 30, 31, false}; // AUTIASP
	case 30: return PointerAuthOperation{5, 30, 0, true}; // AUTIBZ
	case 31: return PointerAuthOperation{5, 30, 31, false}; // AUTIBSP
	default: return std::nullopt;
	}
}

[[nodiscard]] constexpr std::uintptr_t pointer_auth_operand(
	unsigned destination, unsigned modifier) noexcept
{
	const auto destination_offset = destination * sizeof(std::uint64_t);
	const auto modifier_offset = modifier < 31 ?
		modifier * sizeof(std::uint64_t) : offsetof(CpuState, sp);
	return destination_offset |
		(static_cast<std::uintptr_t>(modifier_offset) << 16);
}

[[nodiscard]] constexpr std::uintptr_t pointer_auth_generic_operand(
	unsigned destination, unsigned source, unsigned modifier) noexcept
{
	const auto destination_offset = destination * sizeof(std::uint64_t);
	const auto source_offset = source * sizeof(std::uint64_t);
	const auto modifier_offset = modifier < 31 ?
		modifier * sizeof(std::uint64_t) : offsetof(CpuState, sp);
	return destination_offset |
		(static_cast<std::uintptr_t>(source_offset) << 16) |
		(static_cast<std::uintptr_t>(modifier_offset) << 32);
}

[[nodiscard]] Gadget pointer_auth_branch_gadget(bool link, bool key_b,
	bool zero_modifier, bool zero_target) noexcept
{
	const unsigned variant = (static_cast<unsigned>(zero_target) << 1) |
		static_cast<unsigned>(zero_modifier);
	static constexpr std::array<Gadget, 4> bra_a{
		uml_arm64_gadget_pauth_branch_bra_a_rr,
		uml_arm64_gadget_pauth_branch_bra_a_rz,
		uml_arm64_gadget_pauth_branch_bra_a_zr,
		uml_arm64_gadget_pauth_branch_bra_a_zz};
	static constexpr std::array<Gadget, 4> bra_b{
		uml_arm64_gadget_pauth_branch_bra_b_rr,
		uml_arm64_gadget_pauth_branch_bra_b_rz,
		uml_arm64_gadget_pauth_branch_bra_b_zr,
		uml_arm64_gadget_pauth_branch_bra_b_zz};
	static constexpr std::array<Gadget, 4> blra_a{
		uml_arm64_gadget_pauth_branch_blra_a_rr,
		uml_arm64_gadget_pauth_branch_blra_a_rz,
		uml_arm64_gadget_pauth_branch_blra_a_zr,
		uml_arm64_gadget_pauth_branch_blra_a_zz};
	static constexpr std::array<Gadget, 4> blra_b{
		uml_arm64_gadget_pauth_branch_blra_b_rr,
		uml_arm64_gadget_pauth_branch_blra_b_rz,
		uml_arm64_gadget_pauth_branch_blra_b_zr,
		uml_arm64_gadget_pauth_branch_blra_b_zz};
	return link ? (key_b ? blra_b : blra_a)[variant] :
		(key_b ? bra_b : bra_a)[variant];
}

[[nodiscard]] Gadget pointer_auth_guarded_call_gadget(bool key_b,
	bool zero_modifier, bool zero_target) noexcept
{
	const unsigned variant = (static_cast<unsigned>(zero_target) << 1) |
		static_cast<unsigned>(zero_modifier);
	static constexpr std::array<Gadget, 4> key_a{
		uml_arm64_gadget_pauth_branch_blra_gcs_a_rr,
		uml_arm64_gadget_pauth_branch_blra_gcs_a_rz,
		uml_arm64_gadget_pauth_branch_blra_gcs_a_zr,
		uml_arm64_gadget_pauth_branch_blra_gcs_a_zz};
	static constexpr std::array<Gadget, 4> key_b_entries{
		uml_arm64_gadget_pauth_branch_blra_gcs_b_rr,
		uml_arm64_gadget_pauth_branch_blra_gcs_b_rz,
		uml_arm64_gadget_pauth_branch_blra_gcs_b_zr,
		uml_arm64_gadget_pauth_branch_blra_gcs_b_zz};
	return (key_b ? key_b_entries : key_a)[variant];
}

[[nodiscard]] Gadget pointer_auth_guarded_return_gadget(
	bool key_b) noexcept
{
	return key_b ? uml_arm64_gadget_pauth_return_gcs_b :
		uml_arm64_gadget_pauth_return_gcs_a;
}

[[nodiscard]] Gadget pointer_auth_load_gadget(bool key_b, bool writeback,
	bool base_sp, bool discard) noexcept
{
	static constexpr std::array<Gadget, 16> gadgets{
		uml_arm64_gadget_pauth_load_a_o_rr,
		uml_arm64_gadget_pauth_load_a_o_rz,
		uml_arm64_gadget_pauth_load_a_o_sr,
		uml_arm64_gadget_pauth_load_a_o_sz,
		uml_arm64_gadget_pauth_load_a_w_rr,
		uml_arm64_gadget_pauth_load_a_w_rz,
		uml_arm64_gadget_pauth_load_a_w_sr,
		uml_arm64_gadget_pauth_load_a_w_sz,
		uml_arm64_gadget_pauth_load_b_o_rr,
		uml_arm64_gadget_pauth_load_b_o_rz,
		uml_arm64_gadget_pauth_load_b_o_sr,
		uml_arm64_gadget_pauth_load_b_o_sz,
		uml_arm64_gadget_pauth_load_b_w_rr,
		uml_arm64_gadget_pauth_load_b_w_rz,
		uml_arm64_gadget_pauth_load_b_w_sr,
		uml_arm64_gadget_pauth_load_b_w_sz,
	};
	return gadgets[(key_b ? 8U : 0U) | (writeback ? 4U : 0U) |
		(base_sp ? 2U : 0U) | (discard ? 1U : 0U)];
}

[[nodiscard]] constexpr std::uintptr_t pointer_auth_load_operand(
	unsigned destination, unsigned base, std::int64_t displacement) noexcept
{
	const auto destination_offset = destination < 31 ?
		destination * sizeof(std::uint64_t) : 0U;
	const auto base_offset = base < 31 ?
		base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
	return destination_offset |
		(static_cast<std::uintptr_t>(base_offset) << 16) |
		(static_cast<std::uintptr_t>(
			static_cast<std::uint32_t>(displacement)) << 32);
}

[[nodiscard]] constexpr bool pointer_auth_lr_unavailable(
	std::uint32_t instruction) noexcept
{
	return (instruction & 0xffff'c000U) == 0xdac1'8000U ||
		(instruction & 0xffc0'0000U) == 0xf380'0000U ||
		(instruction & 0xffc0'0000U) == 0x5500'0000U ||
		((instruction & 0xffff'fbe0U) == 0xd65f'0be0U &&
		 (instruction & 31U) != 31U);
}

template<typename Allocator>
void emit(std::vector<std::uintptr_t, Allocator> &words, Gadget gadget)
{
	words.push_back(reinterpret_cast<std::uintptr_t>(gadget));
}

template<typename Allocator>
void emit(std::vector<std::uintptr_t, Allocator> &words,
	  std::uintptr_t operand)
{
	words.push_back(operand);
}

template<typename Allocator>
void emit_fatal(std::vector<std::uintptr_t, Allocator> &words,
	std::uint64_t address)
{
	emit(words, uml_arm64_gadget_fatal);
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

[[nodiscard]] Gadget conditional_branch_linked_gadget(
	unsigned condition) noexcept
{
	switch (condition) {
	case 0: return uml_arm64_gadget_branch_eq_linked;
	case 1: return uml_arm64_gadget_branch_ne_linked;
	case 2: return uml_arm64_gadget_branch_hs_linked;
	case 3: return uml_arm64_gadget_branch_lo_linked;
	case 4: return uml_arm64_gadget_branch_mi_linked;
	case 5: return uml_arm64_gadget_branch_pl_linked;
	case 6: return uml_arm64_gadget_branch_vs_linked;
	case 7: return uml_arm64_gadget_branch_vc_linked;
	case 8: return uml_arm64_gadget_branch_hi_linked;
	case 9: return uml_arm64_gadget_branch_ls_linked;
	case 10: return uml_arm64_gadget_branch_ge_linked;
	case 11: return uml_arm64_gadget_branch_lt_linked;
	case 12: return uml_arm64_gadget_branch_gt_linked;
	case 13: return uml_arm64_gadget_branch_le_linked;
	default: return uml_arm64_gadget_branch_always_linked;
	}
}

[[nodiscard]] Gadget compare_branch_gadget(bool wide, bool nonzero,
	bool linked) noexcept
{
	static constexpr std::array table{
		std::array{
			std::array{uml_arm64_gadget_compare_branch_w_z,
				uml_arm64_gadget_compare_branch_w_z_linked},
			std::array{uml_arm64_gadget_compare_branch_w_nz,
				uml_arm64_gadget_compare_branch_w_nz_linked}},
		std::array{
			std::array{uml_arm64_gadget_compare_branch_x_z,
				uml_arm64_gadget_compare_branch_x_z_linked},
			std::array{uml_arm64_gadget_compare_branch_x_nz,
				uml_arm64_gadget_compare_branch_x_nz_linked}},
	};
	return table[wide][nonzero][linked];
}

[[nodiscard]] Gadget test_branch_gadget(bool nonzero, bool linked) noexcept
{
	static constexpr std::array table{
		std::array{uml_arm64_gadget_test_branch_z,
			uml_arm64_gadget_test_branch_z_linked},
		std::array{uml_arm64_gadget_test_branch_nz,
			uml_arm64_gadget_test_branch_nz_linked},
	};
	return table[nonzero][linked];
}

#define UML_CMPBR_I_GADGETS(size) \
	std::array<Gadget, 6>{ \
		uml_arm64_gadget_cmpbr_i_##size##_sgt, \
		uml_arm64_gadget_cmpbr_i_##size##_slt, \
		uml_arm64_gadget_cmpbr_i_##size##_ugt, \
		uml_arm64_gadget_cmpbr_i_##size##_ult, \
		uml_arm64_gadget_cmpbr_i_##size##_eq, \
		uml_arm64_gadget_cmpbr_i_##size##_ne}

[[nodiscard]] Gadget cmpbr_immediate_gadget(bool is_64_bit,
	unsigned cc) noexcept
{
	static constexpr std::array table{
		UML_CMPBR_I_GADGETS(w), UML_CMPBR_I_GADGETS(x)};
	const unsigned condition = cc < 4 ? cc : cc - 2;
	return table[is_64_bit ? 1U : 0U][condition];
}

#undef UML_CMPBR_I_GADGETS

#define UML_CMPBR_R_MODES(size, condition) \
	std::array<Gadget, 3>{ \
		uml_arm64_gadget_cmpbr_r_##size##_##condition##_rr, \
		uml_arm64_gadget_cmpbr_r_##size##_##condition##_rz, \
		uml_arm64_gadget_cmpbr_r_##size##_##condition##_zr}
#define UML_CMPBR_R_GADGETS(size) \
	std::array<std::array<Gadget, 3>, 6>{ \
		UML_CMPBR_R_MODES(size, sgt), \
		UML_CMPBR_R_MODES(size, sge), \
		UML_CMPBR_R_MODES(size, ugt), \
		UML_CMPBR_R_MODES(size, uge), \
		UML_CMPBR_R_MODES(size, eq), \
		UML_CMPBR_R_MODES(size, ne)}

[[nodiscard]] Gadget cmpbr_register_gadget(unsigned width, unsigned cc,
	bool left_zero, bool right_zero) noexcept
{
	static constexpr std::array table{
		UML_CMPBR_R_GADGETS(b), UML_CMPBR_R_GADGETS(h),
		UML_CMPBR_R_GADGETS(w), UML_CMPBR_R_GADGETS(x)};
	const unsigned size = width == 8 ? 0U : width == 16 ? 1U :
		width == 32 ? 2U : 3U;
	const unsigned condition = cc < 4 ? cc : cc - 2;
	const unsigned mode = left_zero ? 2U : right_zero ? 1U : 0U;
	return table[size][condition][mode];
}

#undef UML_CMPBR_R_GADGETS
#undef UML_CMPBR_R_MODES

[[nodiscard]] constexpr bool cmpbr_immediate_zero_result(
	unsigned cc, unsigned immediate) noexcept
{
	switch (cc) {
	case 0: return false;
	case 1: return immediate != 0;
	case 2: return false;
	case 3: return immediate != 0;
	case 6: return immediate == 0;
	case 7: return immediate != 0;
	default: return false;
	}
}

[[nodiscard]] constexpr bool cmpbr_equal_register_result(unsigned cc) noexcept
{
	return cc == 1 || cc == 3 || cc == 6;
}

[[nodiscard]] Gadget ls64_gadget(unsigned operation, bool base_sp,
	bool status_discard) noexcept
{
	static constexpr std::array load{
		uml_arm64_gadget_ls64_load_r, uml_arm64_gadget_ls64_load_s};
	static constexpr std::array store{
		uml_arm64_gadget_ls64_store_r, uml_arm64_gadget_ls64_store_s};
	static constexpr std::array status{
		std::array{uml_arm64_gadget_ls64_st64bv_rr,
			uml_arm64_gadget_ls64_st64bv_rz},
		std::array{uml_arm64_gadget_ls64_st64bv_sr,
			uml_arm64_gadget_ls64_st64bv_sz}};
	static constexpr std::array status_accdata{
		std::array{uml_arm64_gadget_ls64_st64bv0_rr,
			uml_arm64_gadget_ls64_st64bv0_rz},
		std::array{uml_arm64_gadget_ls64_st64bv0_sr,
			uml_arm64_gadget_ls64_st64bv0_sz}};
	if (operation == 0)
		return load[base_sp];
	if (operation == 1)
		return store[base_sp];
	return (operation == 2 ? status : status_accdata)
		[base_sp][status_discard];
}

[[nodiscard]] constexpr std::uint16_t ls64_register_offset(
	unsigned index) noexcept
{
	static_assert(offsetof(CpuState, registers) == 0);
	return static_cast<std::uint16_t>(
		offsetof(CpuState, registers) + index * sizeof(std::uint64_t));
}

[[nodiscard]] constexpr std::uintptr_t ls64_operand(unsigned first,
	unsigned base, unsigned status, bool base_sp,
	bool status_discard) noexcept
{
	const auto base_offset =
		base_sp ? 0U : ls64_register_offset(base);
	const auto status_offset =
		status_discard ? 0U : ls64_register_offset(status);
	return static_cast<std::uintptr_t>(ls64_register_offset(first)) |
		(static_cast<std::uintptr_t>(base_offset) << 16) |
		(static_cast<std::uintptr_t>(status_offset) << 32);
}

[[nodiscard]] Gadget mops_gadget(
	unsigned family, unsigned stage, bool source_zero) noexcept
{
	static constexpr std::array prologue{
		uml_arm64_gadget_mops_prologue_copy_forward,
		uml_arm64_gadget_mops_prologue_copy_general,
		uml_arm64_gadget_mops_prologue_set};
	static constexpr std::array epilogue{
		uml_arm64_gadget_mops_epilogue_copy_forward,
		uml_arm64_gadget_mops_epilogue_copy_general};
	if (stage == 0)
		return prologue[family];
	if (stage == 1)
		return uml_arm64_gadget_nop;
	if (family == 2)
		return source_zero ? uml_arm64_gadget_mops_epilogue_set_z :
			uml_arm64_gadget_mops_epilogue_set_r;
	return epilogue[family];
}

[[nodiscard]] Gadget mops_set_tag_gadget(
	unsigned stage, bool source_zero) noexcept
{
	if (stage == 0)
		return uml_arm64_gadget_mops_set_tag_prologue;
	if (stage == 1)
		return uml_arm64_gadget_nop;
	return source_zero ? uml_arm64_gadget_mops_set_tag_epilogue_z :
		uml_arm64_gadget_mops_set_tag_epilogue_r;
}

struct SystemRegisterGadget final {
	Gadget gadget;
	bool operand;
};

struct PstateImmediateGadget final {
	Gadget gadget;
	bool valid;
	bool fault_address;
};

[[nodiscard]] constexpr PstateImmediateGadget pstate_boolean_gadget(
	Gadget zero, Gadget one, unsigned immediate) noexcept
{
	return {(immediate & 1U) != 0 ? one : zero, true, false};
}

constexpr std::array<Gadget, 16> pstate_daif_set_gadgets{
	uml_arm64_gadget_pstate_daif_set_0,
	uml_arm64_gadget_pstate_daif_set_1,
	uml_arm64_gadget_pstate_daif_set_2,
	uml_arm64_gadget_pstate_daif_set_3,
	uml_arm64_gadget_pstate_daif_set_4,
	uml_arm64_gadget_pstate_daif_set_5,
	uml_arm64_gadget_pstate_daif_set_6,
	uml_arm64_gadget_pstate_daif_set_7,
	uml_arm64_gadget_pstate_daif_set_8,
	uml_arm64_gadget_pstate_daif_set_9,
	uml_arm64_gadget_pstate_daif_set_10,
	uml_arm64_gadget_pstate_daif_set_11,
	uml_arm64_gadget_pstate_daif_set_12,
	uml_arm64_gadget_pstate_daif_set_13,
	uml_arm64_gadget_pstate_daif_set_14,
	uml_arm64_gadget_pstate_daif_set_15,
};

constexpr std::array<Gadget, 16> pstate_daif_clear_gadgets{
	uml_arm64_gadget_pstate_daif_clear_0,
	uml_arm64_gadget_pstate_daif_clear_1,
	uml_arm64_gadget_pstate_daif_clear_2,
	uml_arm64_gadget_pstate_daif_clear_3,
	uml_arm64_gadget_pstate_daif_clear_4,
	uml_arm64_gadget_pstate_daif_clear_5,
	uml_arm64_gadget_pstate_daif_clear_6,
	uml_arm64_gadget_pstate_daif_clear_7,
	uml_arm64_gadget_pstate_daif_clear_8,
	uml_arm64_gadget_pstate_daif_clear_9,
	uml_arm64_gadget_pstate_daif_clear_10,
	uml_arm64_gadget_pstate_daif_clear_11,
	uml_arm64_gadget_pstate_daif_clear_12,
	uml_arm64_gadget_pstate_daif_clear_13,
	uml_arm64_gadget_pstate_daif_clear_14,
	uml_arm64_gadget_pstate_daif_clear_15,
};

[[nodiscard]] constexpr PstateImmediateGadget pstate_immediate_gadget(
	std::uint32_t instruction) noexcept
{
	const unsigned op1 = (instruction >> 16) & 7U;
	const unsigned immediate = (instruction >> 8) & 15U;
	const unsigned op2 = (instruction >> 5) & 7U;
	if (op1 != 3U)
		return {uml_arm64_gadget_illegal, false, false};
	switch (op2) {
	case 1:
		return pstate_boolean_gadget(
			uml_arm64_gadget_pstate_ssbs_0,
			uml_arm64_gadget_pstate_ssbs_1, immediate);
	case 2:
		return pstate_boolean_gadget(
			uml_arm64_gadget_pstate_dit_0,
			uml_arm64_gadget_pstate_dit_1, immediate);
	case 3:
		if ((immediate & 8U) != 0)
			break;
		switch ((immediate >> 1) & 3U) {
		case 1:
			return pstate_boolean_gadget(
				uml_arm64_gadget_pstate_sm_0,
				uml_arm64_gadget_pstate_sm_1, immediate);
		case 2:
			return pstate_boolean_gadget(
				uml_arm64_gadget_pstate_za_0,
				uml_arm64_gadget_pstate_za_1, immediate);
		case 3:
			return pstate_boolean_gadget(
				uml_arm64_gadget_pstate_smza_0,
				uml_arm64_gadget_pstate_smza_1, immediate);
		default: break;
		}
		break;
	case 4:
		return pstate_boolean_gadget(
			uml_arm64_gadget_pstate_tco_0,
			uml_arm64_gadget_pstate_tco_1, immediate);
	case 6:
		return {pstate_daif_set_gadgets[immediate], true, true};
	case 7:
		return {pstate_daif_clear_gadgets[immediate], true, true};
	default: break;
	}
	return {uml_arm64_gadget_illegal, false, false};
}

[[nodiscard]] constexpr SystemRegisterGadget system_register_gadget(
	std::uint32_t instruction) noexcept
{
	const auto system = instruction & 0xffff'ffe0U;
	const bool zero = (instruction & 31U) == 31U;
	/* Discarded reads have no architectural side effects in this EL0 model. */
	if (zero && (instruction & 0x0020'0000U) != 0)
		return {uml_arm64_gadget_nop, false};
	if (system == 0xd53b'd040U)
		return {uml_arm64_gadget_sysreg_tls_read, true};
	if (system == 0xd51b'd040U)
		return {zero ? uml_arm64_gadget_sysreg_tls_write_z :
			uml_arm64_gadget_sysreg_tls_write_r, !zero};
	if (system == 0xd53b'd060U)
		return {uml_arm64_gadget_sysreg_zero_read, true};
	if (system == 0xd53b'00e0U)
		return {uml_arm64_gadget_sysreg_dczid_read, true};
	if (system == 0xd53b'0020U)
		return {uml_arm64_gadget_sysreg_ctr_read, true};
	if (system == 0xd53b'4400U)
		return {uml_arm64_gadget_sysreg_fpcr_read, true};
	if (system == 0xd51b'4400U)
		return {zero ? uml_arm64_gadget_sysreg_fpcr_write_z :
			uml_arm64_gadget_sysreg_fpcr_write_r, !zero};
	if (system == 0xd53b'4420U)
		return {uml_arm64_gadget_sysreg_fpsr_read, true};
	if (system == 0xd51b'4420U)
		return {zero ? uml_arm64_gadget_sysreg_fpsr_write_z :
			uml_arm64_gadget_sysreg_fpsr_write_r, !zero};
	if (system == 0xd53b'4440U)
		return {uml_arm64_gadget_sysreg_fpmr_read, true};
	if (system == 0xd51b'4440U)
		return {zero ? uml_arm64_gadget_sysreg_fpmr_write_z :
			uml_arm64_gadget_sysreg_fpmr_write_r, !zero};
	if (system == 0xd53b'4200U)
		return {uml_arm64_gadget_sysreg_nzcv_read, true};
	if (system == 0xd51b'4200U)
		return {zero ? uml_arm64_gadget_sysreg_nzcv_write_z :
			uml_arm64_gadget_sysreg_nzcv_write_r, !zero};
	if (system == 0xd53b'e000U)
		return {uml_arm64_gadget_sysreg_cntfrq_read, true};
	return {uml_arm64_gadget_sysreg_cntvct_read, true};
}

[[nodiscard]] Gadget mte_load_tag_gadget(
	bool base_sp, bool destination_zero) noexcept
{
	static constexpr std::array gadgets{
		std::array{uml_arm64_gadget_mte_memory_ldg_rr,
			uml_arm64_gadget_mte_memory_ldg_rz},
		std::array{uml_arm64_gadget_mte_memory_ldg_sr,
			uml_arm64_gadget_mte_memory_ldg_sz}};
	return gadgets[base_sp][destination_zero];
}

[[nodiscard]] Gadget mte_tag_store_gadget(
	unsigned operation, unsigned mode, bool base_sp) noexcept
{
	static constexpr std::array gadgets{
		std::array{
			std::array{uml_arm64_gadget_mte_memory_stg_post_r,
				uml_arm64_gadget_mte_memory_stg_post_s},
			std::array{uml_arm64_gadget_mte_memory_stg_offset_r,
				uml_arm64_gadget_mte_memory_stg_offset_s},
			std::array{uml_arm64_gadget_mte_memory_stg_pre_r,
				uml_arm64_gadget_mte_memory_stg_pre_s}},
		std::array{
			std::array{uml_arm64_gadget_mte_memory_stzg_post_r,
				uml_arm64_gadget_mte_memory_stzg_post_s},
			std::array{uml_arm64_gadget_mte_memory_stzg_offset_r,
				uml_arm64_gadget_mte_memory_stzg_offset_s},
			std::array{uml_arm64_gadget_mte_memory_stzg_pre_r,
				uml_arm64_gadget_mte_memory_stzg_pre_s}},
		std::array{
			std::array{uml_arm64_gadget_mte_memory_st2g_post_r,
				uml_arm64_gadget_mte_memory_st2g_post_s},
			std::array{uml_arm64_gadget_mte_memory_st2g_offset_r,
				uml_arm64_gadget_mte_memory_st2g_offset_s},
			std::array{uml_arm64_gadget_mte_memory_st2g_pre_r,
				uml_arm64_gadget_mte_memory_st2g_pre_s}},
		std::array{
			std::array{uml_arm64_gadget_mte_memory_stz2g_post_r,
				uml_arm64_gadget_mte_memory_stz2g_post_s},
			std::array{uml_arm64_gadget_mte_memory_stz2g_offset_r,
				uml_arm64_gadget_mte_memory_stz2g_offset_s},
			std::array{uml_arm64_gadget_mte_memory_stz2g_pre_r,
				uml_arm64_gadget_mte_memory_stz2g_pre_s}}};
	return gadgets[operation][mode][base_sp];
}

[[nodiscard]] Gadget mte_store_pair_tag_gadget(unsigned mode, bool base_sp,
	bool first_zero, bool second_zero) noexcept
{
	static constexpr std::array gadgets{
		std::array{
			std::array{uml_arm64_gadget_mte_memory_stgp_post_rrr,
				uml_arm64_gadget_mte_memory_stgp_post_rrz,
				uml_arm64_gadget_mte_memory_stgp_post_rzr,
				uml_arm64_gadget_mte_memory_stgp_post_rzz},
			std::array{uml_arm64_gadget_mte_memory_stgp_post_srr,
				uml_arm64_gadget_mte_memory_stgp_post_srz,
				uml_arm64_gadget_mte_memory_stgp_post_szr,
				uml_arm64_gadget_mte_memory_stgp_post_szz}},
		std::array{
			std::array{uml_arm64_gadget_mte_memory_stgp_offset_rrr,
				uml_arm64_gadget_mte_memory_stgp_offset_rrz,
				uml_arm64_gadget_mte_memory_stgp_offset_rzr,
				uml_arm64_gadget_mte_memory_stgp_offset_rzz},
			std::array{uml_arm64_gadget_mte_memory_stgp_offset_srr,
				uml_arm64_gadget_mte_memory_stgp_offset_srz,
				uml_arm64_gadget_mte_memory_stgp_offset_szr,
				uml_arm64_gadget_mte_memory_stgp_offset_szz}},
		std::array{
			std::array{uml_arm64_gadget_mte_memory_stgp_pre_rrr,
				uml_arm64_gadget_mte_memory_stgp_pre_rrz,
				uml_arm64_gadget_mte_memory_stgp_pre_rzr,
				uml_arm64_gadget_mte_memory_stgp_pre_rzz},
			std::array{uml_arm64_gadget_mte_memory_stgp_pre_srr,
				uml_arm64_gadget_mte_memory_stgp_pre_srz,
				uml_arm64_gadget_mte_memory_stgp_pre_szr,
				uml_arm64_gadget_mte_memory_stgp_pre_szz}}};
	const unsigned registers = (first_zero ? 2U : 0U) |
		(second_zero ? 1U : 0U);
	return gadgets[mode][base_sp][registers];
}

[[nodiscard]] constexpr std::uint16_t mte_cpu_slot_offset(
	unsigned index, bool stack_pointer) noexcept
{
	static_assert(offsetof(CpuState, registers) == 0);
	static_assert(offsetof(CpuState, sp) == 248);
	return static_cast<std::uint16_t>(index < 31 ? index * 8U :
		stack_pointer ? offsetof(CpuState, sp) : 0U);
}

[[nodiscard]] constexpr std::uintptr_t mte_memory_operand(unsigned base,
	unsigned first, unsigned second, std::int16_t immediate,
	bool first_stack_pointer) noexcept
{
	return static_cast<std::uintptr_t>(mte_cpu_slot_offset(base, true)) |
		(static_cast<std::uintptr_t>(
			mte_cpu_slot_offset(first, first_stack_pointer)) << 16) |
		(static_cast<std::uintptr_t>(mte_cpu_slot_offset(second, false)) << 32) |
		(static_cast<std::uintptr_t>(static_cast<std::uint16_t>(immediate))
			<< 48);
}

#define UML_CONDITIONAL_COMPARE_FAST_ROW(name) \
	std::array<Gadget, 8>{ \
		uml_arm64_gadget_conditional_compare_fast_##name##_w_cmn_r, \
		uml_arm64_gadget_conditional_compare_fast_##name##_w_cmn_i, \
		uml_arm64_gadget_conditional_compare_fast_##name##_x_cmn_r, \
		uml_arm64_gadget_conditional_compare_fast_##name##_x_cmn_i, \
		uml_arm64_gadget_conditional_compare_fast_##name##_w_cmp_r, \
		uml_arm64_gadget_conditional_compare_fast_##name##_w_cmp_i, \
		uml_arm64_gadget_conditional_compare_fast_##name##_x_cmp_r, \
		uml_arm64_gadget_conditional_compare_fast_##name##_x_cmp_i}

[[nodiscard]] Gadget conditional_compare_gadget(unsigned condition,
	bool subtract, bool is_64_bit, bool immediate) noexcept
{
	static constexpr std::array table{
		UML_CONDITIONAL_COMPARE_FAST_ROW(eq),
		UML_CONDITIONAL_COMPARE_FAST_ROW(ne),
		UML_CONDITIONAL_COMPARE_FAST_ROW(hs),
		UML_CONDITIONAL_COMPARE_FAST_ROW(lo),
		UML_CONDITIONAL_COMPARE_FAST_ROW(mi),
		UML_CONDITIONAL_COMPARE_FAST_ROW(pl),
		UML_CONDITIONAL_COMPARE_FAST_ROW(vs),
		UML_CONDITIONAL_COMPARE_FAST_ROW(vc),
		UML_CONDITIONAL_COMPARE_FAST_ROW(hi),
		UML_CONDITIONAL_COMPARE_FAST_ROW(ls),
		UML_CONDITIONAL_COMPARE_FAST_ROW(ge),
		UML_CONDITIONAL_COMPARE_FAST_ROW(lt),
		UML_CONDITIONAL_COMPARE_FAST_ROW(gt),
		UML_CONDITIONAL_COMPARE_FAST_ROW(le),
		UML_CONDITIONAL_COMPARE_FAST_ROW(always)};
	return table[condition < 14U ? condition : 14U]
		[subtract * 4U + is_64_bit * 2U + immediate];
}

#undef UML_CONDITIONAL_COMPARE_FAST_ROW

#define UML_CONDITIONAL_SELECT_FAST_ROW(name) \
	std::array<Gadget, 8>{ \
		uml_arm64_gadget_conditional_select_fast_##name##_w_sel, \
		uml_arm64_gadget_conditional_select_fast_##name##_x_sel, \
		uml_arm64_gadget_conditional_select_fast_##name##_w_inc, \
		uml_arm64_gadget_conditional_select_fast_##name##_x_inc, \
		uml_arm64_gadget_conditional_select_fast_##name##_w_inv, \
		uml_arm64_gadget_conditional_select_fast_##name##_x_inv, \
		uml_arm64_gadget_conditional_select_fast_##name##_w_neg, \
		uml_arm64_gadget_conditional_select_fast_##name##_x_neg}

[[nodiscard]] Gadget conditional_select_gadget(unsigned condition,
	unsigned operation, bool is_64_bit) noexcept
{
	static constexpr std::array table{
		UML_CONDITIONAL_SELECT_FAST_ROW(eq),
		UML_CONDITIONAL_SELECT_FAST_ROW(ne),
		UML_CONDITIONAL_SELECT_FAST_ROW(hs),
		UML_CONDITIONAL_SELECT_FAST_ROW(lo),
		UML_CONDITIONAL_SELECT_FAST_ROW(mi),
		UML_CONDITIONAL_SELECT_FAST_ROW(pl),
		UML_CONDITIONAL_SELECT_FAST_ROW(vs),
		UML_CONDITIONAL_SELECT_FAST_ROW(vc),
		UML_CONDITIONAL_SELECT_FAST_ROW(hi),
		UML_CONDITIONAL_SELECT_FAST_ROW(ls),
		UML_CONDITIONAL_SELECT_FAST_ROW(ge),
		UML_CONDITIONAL_SELECT_FAST_ROW(lt),
		UML_CONDITIONAL_SELECT_FAST_ROW(gt),
		UML_CONDITIONAL_SELECT_FAST_ROW(le)};
	if (condition >= 14U)
		return is_64_bit ?
			uml_arm64_gadget_conditional_select_fast_always_x :
			uml_arm64_gadget_conditional_select_fast_always_w;
	return table[condition][operation * 2U + is_64_bit];
}

#undef UML_CONDITIONAL_SELECT_FAST_ROW

[[nodiscard]] Gadget data_memory_barrier_gadget(
	bool synchronization, unsigned option) noexcept
{
	static constexpr std::array<Gadget, 16> dmb{
		uml_arm64_gadget_dmb_0, uml_arm64_gadget_dmb_1,
		uml_arm64_gadget_dmb_2, uml_arm64_gadget_dmb_3,
		uml_arm64_gadget_dmb_4, uml_arm64_gadget_dmb_5,
		uml_arm64_gadget_dmb_6, uml_arm64_gadget_dmb_7,
		uml_arm64_gadget_dmb_8, uml_arm64_gadget_dmb_9,
		uml_arm64_gadget_dmb_10, uml_arm64_gadget_dmb_11,
		uml_arm64_gadget_dmb_12, uml_arm64_gadget_dmb_13,
		uml_arm64_gadget_dmb_14, uml_arm64_gadget_dmb_15};
	static constexpr std::array<Gadget, 16> dsb{
		uml_arm64_gadget_dsb_0, uml_arm64_gadget_dsb_1,
		uml_arm64_gadget_dsb_2, uml_arm64_gadget_dsb_3,
		uml_arm64_gadget_dsb_4, uml_arm64_gadget_dsb_5,
		uml_arm64_gadget_dsb_6, uml_arm64_gadget_dsb_7,
		uml_arm64_gadget_dsb_8, uml_arm64_gadget_dsb_9,
		uml_arm64_gadget_dsb_10, uml_arm64_gadget_dsb_11,
		uml_arm64_gadget_dsb_12, uml_arm64_gadget_dsb_13,
		uml_arm64_gadget_dsb_14, uml_arm64_gadget_dsb_15};
	return (synchronization ? dsb : dmb)[option & 15U];
}

[[nodiscard]] Gadget memory_barrier_gadget(std::uint32_t instruction) noexcept
{
	if (instruction == 0xd503'30ffU)
		return host_capabilities().has(ArmFeature::sb) ?
			uml_arm64_gadget_speculation_barrier :
			uml_arm64_gadget_speculation_barrier_fallback;
	if ((instruction & 0xffff'f3ffU) == 0xd503'323fU) {
		if (host_capabilities().has(ArmFeature::xs)) {
			static constexpr std::array<Gadget, 4> native{
				uml_arm64_gadget_dsb_nxs_0,
				uml_arm64_gadget_dsb_nxs_1,
				uml_arm64_gadget_dsb_nxs_2,
				uml_arm64_gadget_dsb_nxs_3};
			return native[(instruction >> 10) & 3U];
		}
		/* A full DSB is stronger than its nXS counterpart. Preserve the
		 * requested shareability scope on hosts without FEAT_XS. */
		static constexpr std::array<unsigned, 4> options{3, 7, 11, 15};
		return data_memory_barrier_gadget(
			true, options[(instruction >> 10) & 3U]);
	}
	if ((instruction & 0xffff'f0ffU) == 0xd503'30dfU)
		return uml_arm64_gadget_isb;
	return data_memory_barrier_gadget(
		(instruction & 0x60U) == 0, (instruction >> 8) & 15U);
}

[[nodiscard]] Gadget architectural_hint_gadget(
	std::uint32_t instruction) noexcept
{
	const unsigned immediate = (instruction >> 5) & 0x7fU;
	if (immediate == 1U)
		return uml_arm64_gadget_yield;
	if (immediate == 6U)
		return uml_arm64_gadget_dgh;
	if (immediate == 20U)
		return uml_arm64_gadget_csdb;
	if (immediate == 22U)
		return uml_arm64_gadget_clrbhb;
	/* The remaining allocated hints have no guest-visible effect in the
	 * currently exposed architecture: guarded pages, GCS, RAS delivery,
	 * profiling, trace, and a blocking event register are not advertised.
	 * Their architectural feature-absent or unguarded behavior is NOP. Keep
	 * that policy at translation time so the valid hot path has no decoder.
	 */
	return uml_arm64_gadget_nop;
}

#define UML_INTEGER_MIN_MAX_FAST_ROW(name) \
	std::array<Gadget, 4>{ \
		uml_arm64_gadget_integer_##name##_fast_w_r, \
		uml_arm64_gadget_integer_##name##_fast_w_i, \
		uml_arm64_gadget_integer_##name##_fast_x_r, \
		uml_arm64_gadget_integer_##name##_fast_x_i}

[[nodiscard]] Gadget integer_min_max_gadget(unsigned operation,
	bool is_64_bit, bool immediate) noexcept
{
	static constexpr std::array table{
		UML_INTEGER_MIN_MAX_FAST_ROW(smax),
		UML_INTEGER_MIN_MAX_FAST_ROW(umax),
		UML_INTEGER_MIN_MAX_FAST_ROW(smin),
		UML_INTEGER_MIN_MAX_FAST_ROW(umin)};
	return table[operation][is_64_bit * 2U + immediate];
}

#undef UML_INTEGER_MIN_MAX_FAST_ROW

#define UML_SELECT_ADD_SUB_IMMEDIATE_MODES(prefix) \
	if (source_sp) \
		return destination_special ? \
			uml_arm64_gadget_add_sub_immediate_##prefix##_ss : \
			uml_arm64_gadget_add_sub_immediate_##prefix##_sr; \
	return destination_special ? \
		uml_arm64_gadget_add_sub_immediate_##prefix##_rs : \
		uml_arm64_gadget_add_sub_immediate_##prefix##_rr

[[nodiscard]] Gadget add_sub_immediate_gadget(bool is_64_bit, bool subtract,
	bool set_flags, bool source_sp, bool destination_special) noexcept
{
	if (is_64_bit) {
		if (subtract) {
			if (set_flags) {
				UML_SELECT_ADD_SUB_IMMEDIATE_MODES(x_sub_s);
			}
			UML_SELECT_ADD_SUB_IMMEDIATE_MODES(x_sub_n);
		}
		if (set_flags) {
			UML_SELECT_ADD_SUB_IMMEDIATE_MODES(x_add_s);
		}
		UML_SELECT_ADD_SUB_IMMEDIATE_MODES(x_add_n);
	}
	if (subtract) {
		if (set_flags) {
			UML_SELECT_ADD_SUB_IMMEDIATE_MODES(w_sub_s);
		}
		UML_SELECT_ADD_SUB_IMMEDIATE_MODES(w_sub_n);
	}
	if (set_flags) {
		UML_SELECT_ADD_SUB_IMMEDIATE_MODES(w_add_s);
	}
	UML_SELECT_ADD_SUB_IMMEDIATE_MODES(w_add_n);
}

#undef UML_SELECT_ADD_SUB_IMMEDIATE_MODES

#define UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix) \
	return destination_discard ? \
		uml_arm64_gadget_add_sub_register_##prefix##_d : \
		uml_arm64_gadget_add_sub_register_##prefix##_r
#define UML_SELECT_ADD_SUB_SHIFTED_SOURCES(prefix) \
	if (left_zero) { \
		if (right_zero) { \
			UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix##_zz); \
		} \
		UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix##_zr); \
	} \
	if (right_zero) { \
		UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix##_rz); \
	} \
	UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix##_rr)
#define UML_SELECT_ADD_SUB_SHIFTED_TRANSFORM(prefix) \
	switch (transform) { \
	case 0: UML_SELECT_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_n); \
	case 1: UML_SELECT_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_l); \
	case 2: UML_SELECT_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_r); \
	default: UML_SELECT_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_a); \
	}
#define UML_SELECT_ADD_SUB_EXTENDED_SOURCE(prefix) \
	if (right_zero) { \
		UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix##_z); \
	} \
	UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix##_r)
#define UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, number) \
	if (extension == number) { \
		if (shift_nonzero) { \
			UML_SELECT_ADD_SUB_EXTENDED_SOURCE(prefix##_ex_e##number##s); \
		} \
		UML_SELECT_ADD_SUB_EXTENDED_SOURCE(prefix##_ex_e##number##n); \
	}
#define UML_SELECT_ADD_SUB_EXTENDED_TRANSFORM(prefix) \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 0); \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 1); \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 2); \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 3); \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 4); \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 5); \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 6); \
	if (shift_nonzero) { \
		UML_SELECT_ADD_SUB_EXTENDED_SOURCE(prefix##_ex_e7s); \
	} \
	UML_SELECT_ADD_SUB_EXTENDED_SOURCE(prefix##_ex_e7n)
#define UML_SELECT_ADD_SUB_REGISTER_FORM(prefix) \
	if (extended) { \
		UML_SELECT_ADD_SUB_EXTENDED_TRANSFORM(prefix); \
	} \
	UML_SELECT_ADD_SUB_SHIFTED_TRANSFORM(prefix)

[[nodiscard]] Gadget add_sub_register_gadget(bool is_64_bit, bool subtract,
	bool set_flags, bool extended, unsigned transform, unsigned extension,
	bool shift_nonzero, bool left_zero, bool right_zero,
	bool destination_discard) noexcept
{
	if (is_64_bit) {
		if (subtract) {
			if (set_flags) {
				UML_SELECT_ADD_SUB_REGISTER_FORM(x_sub_s);
			}
			UML_SELECT_ADD_SUB_REGISTER_FORM(x_sub_n);
		}
		if (set_flags) {
			UML_SELECT_ADD_SUB_REGISTER_FORM(x_add_s);
		}
		UML_SELECT_ADD_SUB_REGISTER_FORM(x_add_n);
	}
	if (subtract) {
		if (set_flags) {
			UML_SELECT_ADD_SUB_REGISTER_FORM(w_sub_s);
		}
		UML_SELECT_ADD_SUB_REGISTER_FORM(w_sub_n);
	}
	if (set_flags) {
		UML_SELECT_ADD_SUB_REGISTER_FORM(w_add_s);
	}
	UML_SELECT_ADD_SUB_REGISTER_FORM(w_add_n);
}

#undef UML_SELECT_ADD_SUB_REGISTER_FORM
#undef UML_SELECT_ADD_SUB_EXTENDED_TRANSFORM
#undef UML_SELECT_ADD_SUB_EXTENDED_ONE
#undef UML_SELECT_ADD_SUB_EXTENDED_SOURCE
#undef UML_SELECT_ADD_SUB_SHIFTED_TRANSFORM
#undef UML_SELECT_ADD_SUB_SHIFTED_SOURCES
#undef UML_SELECT_ADD_SUB_REGISTER_DESTINATION

#define UML_SELECT_ADD_SUB_CARRY_FLAGS(prefix) \
	return destination_zero ? \
		uml_arm64_gadget_add_sub_carry_##prefix##_z : \
		uml_arm64_gadget_add_sub_carry_##prefix##_r

#define UML_SELECT_ADD_SUB_CARRY_SOURCES(prefix, result) \
	if (left_zero) { \
		if (right_zero) { \
			result(prefix##_zz); \
		} \
		result(prefix##_zr); \
	} \
	if (right_zero) { \
		result(prefix##_rz); \
	} \
	if (same_source) { \
		result(prefix##_eq); \
	} \
	result(prefix##_rr)

#define UML_SELECT_ADD_SUB_CARRY_RESULT(prefix) \
	return uml_arm64_gadget_add_sub_carry_##prefix##_r

[[nodiscard]] Gadget add_sub_carry_gadget(bool is_64_bit, bool subtract,
	bool set_flags, bool left_zero, bool right_zero,
	bool same_source, bool destination_zero) noexcept
{
	if (destination_zero && !set_flags)
		return uml_arm64_gadget_nop;
	if (is_64_bit) {
		if (subtract) {
			if (set_flags) {
				UML_SELECT_ADD_SUB_CARRY_SOURCES(x_sbc_s,
					UML_SELECT_ADD_SUB_CARRY_FLAGS);
			}
			UML_SELECT_ADD_SUB_CARRY_SOURCES(x_sbc_n,
				UML_SELECT_ADD_SUB_CARRY_RESULT);
		}
		if (set_flags) {
			UML_SELECT_ADD_SUB_CARRY_SOURCES(x_adc_s,
				UML_SELECT_ADD_SUB_CARRY_FLAGS);
		}
		UML_SELECT_ADD_SUB_CARRY_SOURCES(x_adc_n,
			UML_SELECT_ADD_SUB_CARRY_RESULT);
	}
	if (subtract) {
		if (set_flags) {
			UML_SELECT_ADD_SUB_CARRY_SOURCES(w_sbc_s,
				UML_SELECT_ADD_SUB_CARRY_FLAGS);
		}
		UML_SELECT_ADD_SUB_CARRY_SOURCES(w_sbc_n,
			UML_SELECT_ADD_SUB_CARRY_RESULT);
	}
	if (set_flags) {
		UML_SELECT_ADD_SUB_CARRY_SOURCES(w_adc_s,
			UML_SELECT_ADD_SUB_CARRY_FLAGS);
	}
	UML_SELECT_ADD_SUB_CARRY_SOURCES(w_adc_n,
		UML_SELECT_ADD_SUB_CARRY_RESULT);
}

#undef UML_SELECT_ADD_SUB_CARRY_RESULT
#undef UML_SELECT_ADD_SUB_CARRY_SOURCES
#undef UML_SELECT_ADD_SUB_CARRY_FLAGS

#define UML_SELECT_BINARY_SOURCES(kind, prefix) \
	if (left_zero) { \
		if (right_zero) { \
			return uml_arm64_gadget_##kind##_##prefix##_zz_r; \
		} \
		return uml_arm64_gadget_##kind##_##prefix##_zr_r; \
	} \
	if (right_zero) { \
		return uml_arm64_gadget_##kind##_##prefix##_rz_r; \
	} \
	if (same_source) { \
		return uml_arm64_gadget_##kind##_##prefix##_eq_r; \
	} \
	return uml_arm64_gadget_##kind##_##prefix##_rr_r

[[nodiscard]] Gadget variable_shift_gadget(bool is_64_bit,
	unsigned operation, bool left_zero, bool right_zero,
	bool same_source) noexcept
{
	if (is_64_bit) {
		switch (operation) {
		case 0: UML_SELECT_BINARY_SOURCES(variable_shift, x_lsl);
		case 1: UML_SELECT_BINARY_SOURCES(variable_shift, x_lsr);
		case 2: UML_SELECT_BINARY_SOURCES(variable_shift, x_asr);
		default: UML_SELECT_BINARY_SOURCES(variable_shift, x_ror);
		}
	}
	switch (operation) {
	case 0: UML_SELECT_BINARY_SOURCES(variable_shift, w_lsl);
	case 1: UML_SELECT_BINARY_SOURCES(variable_shift, w_lsr);
	case 2: UML_SELECT_BINARY_SOURCES(variable_shift, w_asr);
	default: UML_SELECT_BINARY_SOURCES(variable_shift, w_ror);
	}
}

[[nodiscard]] Gadget divide_gadget(bool is_64_bit, bool is_signed,
	bool left_zero, bool right_zero, bool same_source) noexcept
{
	if (is_64_bit) {
		if (is_signed) {
			UML_SELECT_BINARY_SOURCES(divide, x_s);
		}
		UML_SELECT_BINARY_SOURCES(divide, x_u);
	}
	if (is_signed) {
		UML_SELECT_BINARY_SOURCES(divide, w_s);
	}
	UML_SELECT_BINARY_SOURCES(divide, w_u);
}

#undef UML_SELECT_BINARY_SOURCES

#define UML_SELECT_LOGICAL_IMMEDIATE_MODES(prefix) \
	if (source_zero) \
		return destination_special ? \
			uml_arm64_gadget_logical_immediate_##prefix##_zs : \
			uml_arm64_gadget_logical_immediate_##prefix##_zr; \
	return destination_special ? \
		uml_arm64_gadget_logical_immediate_##prefix##_rs : \
		uml_arm64_gadget_logical_immediate_##prefix##_rr

[[nodiscard]] Gadget logical_immediate_gadget(bool is_64_bit,
	unsigned operation, bool source_zero, bool destination_special) noexcept
{
	if (is_64_bit) {
		switch (operation) {
		case 0: UML_SELECT_LOGICAL_IMMEDIATE_MODES(x_and);
		case 1: UML_SELECT_LOGICAL_IMMEDIATE_MODES(x_orr);
		case 2: UML_SELECT_LOGICAL_IMMEDIATE_MODES(x_eor);
		default: UML_SELECT_LOGICAL_IMMEDIATE_MODES(x_ands);
		}
	}
	switch (operation) {
	case 0: UML_SELECT_LOGICAL_IMMEDIATE_MODES(w_and);
	case 1: UML_SELECT_LOGICAL_IMMEDIATE_MODES(w_orr);
	case 2: UML_SELECT_LOGICAL_IMMEDIATE_MODES(w_eor);
	default: UML_SELECT_LOGICAL_IMMEDIATE_MODES(w_ands);
	}
}

#undef UML_SELECT_LOGICAL_IMMEDIATE_MODES

#define UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES(prefix) \
	if (left_zero) { \
		if (right_zero) \
			return uml_arm64_gadget_logical_shifted_##prefix##_zz_r; \
		return uml_arm64_gadget_logical_shifted_##prefix##_zr_r; \
	} \
	if (right_zero) \
		return uml_arm64_gadget_logical_shifted_##prefix##_rz_r; \
	return uml_arm64_gadget_logical_shifted_##prefix##_rr_r
#define UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(prefix) \
	switch (shift_kind) { \
	case 0: UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_n); \
	case 1: UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_l); \
	case 2: UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_r); \
	case 3: UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_a); \
	default: UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_o); \
	}
#define UML_SELECT_LOGICAL_SHIFTED_FLAG_DESTINATION(prefix) \
	return destination_zero ? \
		uml_arm64_gadget_logical_shifted_##prefix##_z : \
		uml_arm64_gadget_logical_shifted_##prefix##_r
#define UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES(prefix) \
	if (left_zero) { \
		if (right_zero) { \
			UML_SELECT_LOGICAL_SHIFTED_FLAG_DESTINATION(prefix##_zz); \
		} \
		UML_SELECT_LOGICAL_SHIFTED_FLAG_DESTINATION(prefix##_zr); \
	} \
	if (right_zero) { \
		UML_SELECT_LOGICAL_SHIFTED_FLAG_DESTINATION(prefix##_rz); \
	} \
	UML_SELECT_LOGICAL_SHIFTED_FLAG_DESTINATION(prefix##_rr)
#define UML_SELECT_LOGICAL_SHIFTED_FLAG_SHIFT(prefix) \
	switch (shift_kind) { \
	case 0: UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_n); \
	case 1: UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_l); \
	case 2: UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_r); \
	case 3: UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_a); \
	default: UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_o); \
	}

[[nodiscard]] Gadget logical_shifted_gadget(bool is_64_bit,
	unsigned operation, bool invert, unsigned shift_kind, bool left_zero,
	bool right_zero, bool destination_zero) noexcept
{
	if (is_64_bit) {
		switch (operation) {
		case 0:
			if (invert) {
				UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(x_and_n);
			}
			UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(x_and_p);
		case 1:
			if (invert) {
				UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(x_orr_n);
			}
			UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(x_orr_p);
		case 2:
			if (invert) {
				UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(x_eor_n);
			}
			UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(x_eor_p);
		default:
			if (invert) {
				UML_SELECT_LOGICAL_SHIFTED_FLAG_SHIFT(x_ands_n);
			}
			UML_SELECT_LOGICAL_SHIFTED_FLAG_SHIFT(x_ands_p);
		}
	}
	switch (operation) {
	case 0:
		if (invert) {
			UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(w_and_n);
		}
		UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(w_and_p);
	case 1:
		if (invert) {
			UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(w_orr_n);
		}
		UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(w_orr_p);
	case 2:
		if (invert) {
			UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(w_eor_n);
		}
		UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(w_eor_p);
	default:
		if (invert) {
			UML_SELECT_LOGICAL_SHIFTED_FLAG_SHIFT(w_ands_n);
		}
		UML_SELECT_LOGICAL_SHIFTED_FLAG_SHIFT(w_ands_p);
	}
}

#undef UML_SELECT_LOGICAL_SHIFTED_FLAG_SHIFT
#undef UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES
#undef UML_SELECT_LOGICAL_SHIFTED_FLAG_DESTINATION
#undef UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT
#undef UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES

#define UML_SELECT_MULTIPLY_RESULT(prefix) \
	return uml_arm64_gadget_multiply_add_##prefix##_r
#define UML_SELECT_MULTIPLY_ADDEND(prefix) \
	if (addend_zero) { \
		UML_SELECT_MULTIPLY_RESULT(prefix##_z); \
	} \
	UML_SELECT_MULTIPLY_RESULT(prefix##_r)
#define UML_SELECT_MULTIPLY_SOURCES(prefix) \
	if (left_zero || right_zero) { \
		UML_SELECT_MULTIPLY_ADDEND(prefix##_z); \
	} \
	UML_SELECT_MULTIPLY_ADDEND(prefix##_p)
#define UML_SELECT_MULTIPLY_HIGH_SOURCES(prefix) \
	if (left_zero || right_zero) { \
		UML_SELECT_MULTIPLY_RESULT(prefix##_z_z); \
	} \
	UML_SELECT_MULTIPLY_RESULT(prefix##_p_z)

[[nodiscard]] Gadget multiply_add_gadget(unsigned operation, bool subtract,
	bool is_64_bit, bool left_zero, bool right_zero,
	bool addend_zero) noexcept
{
	switch (operation) {
	case 0:
		if (is_64_bit) {
			if (subtract) {
				UML_SELECT_MULTIPLY_SOURCES(x_msub);
			}
			UML_SELECT_MULTIPLY_SOURCES(x_madd);
		}
		if (subtract) {
			UML_SELECT_MULTIPLY_SOURCES(w_msub);
		}
		UML_SELECT_MULTIPLY_SOURCES(w_madd);
	case 1:
		if (subtract) {
			UML_SELECT_MULTIPLY_SOURCES(smsubl);
		}
		UML_SELECT_MULTIPLY_SOURCES(smaddl);
	case 2:
		UML_SELECT_MULTIPLY_HIGH_SOURCES(smulh);
	case 5:
		if (subtract) {
			UML_SELECT_MULTIPLY_SOURCES(umsubl);
		}
		UML_SELECT_MULTIPLY_SOURCES(umaddl);
	default:
		UML_SELECT_MULTIPLY_HIGH_SOURCES(umulh);
	}
}

#undef UML_SELECT_MULTIPLY_HIGH_SOURCES
#undef UML_SELECT_MULTIPLY_SOURCES
#undef UML_SELECT_MULTIPLY_ADDEND
#undef UML_SELECT_MULTIPLY_RESULT

#define UML_SELECT_BITFIELD_SOURCES(prefix) \
	if (rotate_nonzero) \
		return source_zero ? uml_arm64_gadget_bitfield_##prefix##_r_z : \
			uml_arm64_gadget_bitfield_##prefix##_r_r; \
	return source_zero ? uml_arm64_gadget_bitfield_##prefix##_n_z : \
		uml_arm64_gadget_bitfield_##prefix##_n_r

[[nodiscard]] Gadget bitfield_gadget(bool is_64_bit, unsigned operation,
	bool rotate_nonzero, bool source_zero) noexcept
{
	if (is_64_bit) {
		switch (operation) {
		case 0: UML_SELECT_BITFIELD_SOURCES(x_s);
		case 1: UML_SELECT_BITFIELD_SOURCES(x_b);
		default: UML_SELECT_BITFIELD_SOURCES(x_u);
		}
	}
	switch (operation) {
	case 0: UML_SELECT_BITFIELD_SOURCES(w_s);
	case 1: UML_SELECT_BITFIELD_SOURCES(w_b);
	default: UML_SELECT_BITFIELD_SOURCES(w_u);
	}
}

#undef UML_SELECT_BITFIELD_SOURCES

#define UML_SELECT_EXTRACT_DESTINATION(prefix) \
	return destination_zero ? uml_arm64_gadget_extract_register_##prefix##_z : \
		uml_arm64_gadget_extract_register_##prefix##_r
#define UML_SELECT_EXTRACT_SOURCES(prefix) \
	if (left_zero) { \
		if (right_zero) { \
			UML_SELECT_EXTRACT_DESTINATION(prefix##_zz); \
		} \
		UML_SELECT_EXTRACT_DESTINATION(prefix##_zr); \
	} \
	if (right_zero) { \
		UML_SELECT_EXTRACT_DESTINATION(prefix##_rz); \
	} \
	UML_SELECT_EXTRACT_DESTINATION(prefix##_rr)

[[nodiscard]] Gadget extract_register_gadget(bool is_64_bit,
	bool shift_nonzero, bool left_zero, bool right_zero,
	bool destination_zero) noexcept
{
	if (is_64_bit) {
		if (shift_nonzero) {
			UML_SELECT_EXTRACT_SOURCES(x_s);
		}
		UML_SELECT_EXTRACT_SOURCES(x_n);
	}
	if (shift_nonzero) {
		UML_SELECT_EXTRACT_SOURCES(w_s);
	}
	UML_SELECT_EXTRACT_SOURCES(w_n);
}

#undef UML_SELECT_EXTRACT_SOURCES
#undef UML_SELECT_EXTRACT_DESTINATION

#define UML_CRC_GADGET(prefix, suffix) \
	(native ? uml_arm64_gadget_crc32_##prefix##_##suffix : \
		uml_arm64_gadget_crc32_software_##prefix##_##suffix)
#define UML_SELECT_CRC_SOURCES(prefix) \
	if (left_zero) { \
		if (right_zero) { \
			return UML_CRC_GADGET(prefix, zz); \
		} \
		return UML_CRC_GADGET(prefix, zr); \
	} \
	if (right_zero) { \
		return UML_CRC_GADGET(prefix, rz); \
	} \
	return same_source ? UML_CRC_GADGET(prefix, same) : \
		UML_CRC_GADGET(prefix, rr)

[[nodiscard]] Gadget crc32_gadget(unsigned operation, bool left_zero,
	bool right_zero, bool same_source, bool native) noexcept
{
	switch (operation) {
	case 0: UML_SELECT_CRC_SOURCES(b);
	case 1: UML_SELECT_CRC_SOURCES(h);
	case 2: UML_SELECT_CRC_SOURCES(w);
	case 3: UML_SELECT_CRC_SOURCES(x);
	case 4: UML_SELECT_CRC_SOURCES(cb);
	case 5: UML_SELECT_CRC_SOURCES(ch);
	case 6: UML_SELECT_CRC_SOURCES(cw);
	default: UML_SELECT_CRC_SOURCES(cx);
	}
}

#undef UML_SELECT_CRC_SOURCES
#undef UML_CRC_GADGET

[[nodiscard]] Gadget bit_reverse_count_gadget(bool is_64_bit,
	unsigned operation) noexcept
{
	const bool native = operation >= 6U && host_capabilities().cssc;
	if (is_64_bit) {
		switch (operation) {
		case 0: return uml_arm64_gadget_bit_reverse_count_x_rbit;
		case 1: return uml_arm64_gadget_bit_reverse_count_x_rev16;
		case 2: return uml_arm64_gadget_bit_reverse_count_x_rev32;
		case 3: return uml_arm64_gadget_bit_reverse_count_x_rev;
		case 4: return uml_arm64_gadget_bit_reverse_count_x_clz;
		case 5: return uml_arm64_gadget_bit_reverse_count_x_cls;
		case 6: return native ?
			uml_arm64_gadget_bit_reverse_count_x_ctz_native :
			uml_arm64_gadget_bit_reverse_count_x_ctz;
		case 7: return native ?
			uml_arm64_gadget_bit_reverse_count_x_cnt_native :
			uml_arm64_gadget_bit_reverse_count_x_cnt;
		default: return native ?
			uml_arm64_gadget_bit_reverse_count_x_abs_native :
			uml_arm64_gadget_bit_reverse_count_x_abs;
		}
	}
	switch (operation) {
	case 0: return uml_arm64_gadget_bit_reverse_count_w_rbit;
	case 1: return uml_arm64_gadget_bit_reverse_count_w_rev16;
	case 2: return uml_arm64_gadget_bit_reverse_count_w_rev;
	case 4: return uml_arm64_gadget_bit_reverse_count_w_clz;
	case 5: return uml_arm64_gadget_bit_reverse_count_w_cls;
	case 6: return native ?
		uml_arm64_gadget_bit_reverse_count_w_ctz_native :
		uml_arm64_gadget_bit_reverse_count_w_ctz;
	case 7: return native ?
		uml_arm64_gadget_bit_reverse_count_w_cnt_native :
		uml_arm64_gadget_bit_reverse_count_w_cnt;
	default: return native ?
		uml_arm64_gadget_bit_reverse_count_w_abs_native :
		uml_arm64_gadget_bit_reverse_count_w_abs;
	}
}

#define UML_MTE_POINTER_GADGETS(operation) \
	std::array<Gadget, 8>{ \
		uml_arm64_gadget_mte_##operation##_rrr, \
		uml_arm64_gadget_mte_##operation##_rrz, \
		uml_arm64_gadget_mte_##operation##_rsr, \
		uml_arm64_gadget_mte_##operation##_rsz, \
		uml_arm64_gadget_mte_##operation##_srr, \
		uml_arm64_gadget_mte_##operation##_srz, \
		uml_arm64_gadget_mte_##operation##_ssr, \
		uml_arm64_gadget_mte_##operation##_ssz}

[[nodiscard]] Gadget mte_pointer_gadget(unsigned operation, bool first_special,
	bool second_special, bool destination_zero) noexcept
{
	static constexpr std::array table{
		UML_MTE_POINTER_GADGETS(gmi),
		UML_MTE_POINTER_GADGETS(subp),
		UML_MTE_POINTER_GADGETS(subps),
	};
	const unsigned mode = (first_special ? 4U : 0U) |
		(second_special ? 2U : 0U) | (destination_zero ? 1U : 0U);
	return table[operation][mode];
}

#undef UML_MTE_POINTER_GADGETS

[[nodiscard]] Gadget mte_tag_add_sub_gadget(bool subtract, bool source_sp,
	bool destination_sp) noexcept
{
	static constexpr std::array add{
		uml_arm64_gadget_mte_addg_rr, uml_arm64_gadget_mte_addg_rs,
		uml_arm64_gadget_mte_addg_sr, uml_arm64_gadget_mte_addg_ss};
	static constexpr std::array sub{
		uml_arm64_gadget_mte_subg_rr, uml_arm64_gadget_mte_subg_rs,
		uml_arm64_gadget_mte_subg_sr, uml_arm64_gadget_mte_subg_ss};
	const unsigned mode = (source_sp ? 2U : 0U) |
		(destination_sp ? 1U : 0U);
	return (subtract ? sub : add)[mode];
}

[[nodiscard]] Gadget mte_irg_gadget(bool source_sp, bool mask_zero,
	bool destination_sp) noexcept
{
	static constexpr std::array table{
		uml_arm64_gadget_mte_irg_rrr, uml_arm64_gadget_mte_irg_rrs,
		uml_arm64_gadget_mte_irg_rzr, uml_arm64_gadget_mte_irg_rzs,
		uml_arm64_gadget_mte_irg_srr, uml_arm64_gadget_mte_irg_srs,
		uml_arm64_gadget_mte_irg_szr, uml_arm64_gadget_mte_irg_szs};
	const unsigned mode = (source_sp ? 4U : 0U) |
		(mask_zero ? 2U : 0U) | (destination_sp ? 1U : 0U);
	return table[mode];
}

[[nodiscard]] constexpr auto make_byte_popcount_table() noexcept
{
	std::array<std::uint8_t, 256> table{};
	for (unsigned value = 0; value < table.size(); ++value)
		table[value] = static_cast<std::uint8_t>(std::popcount(value));
	return table;
}

constexpr auto byte_popcount_table = make_byte_popcount_table();

using CpaModeGadgets = std::array<Gadget, 8>;
using CpaShiftGadgets = std::array<CpaModeGadgets, 8>;

#define UML_CPA_MODES(operation, shift) \
	CpaModeGadgets{ \
		uml_arm64_gadget_cpa_##operation##_##shift##_rrr, \
		uml_arm64_gadget_cpa_##operation##_##shift##_rrs, \
		uml_arm64_gadget_cpa_##operation##_##shift##_rzr, \
		uml_arm64_gadget_cpa_##operation##_##shift##_rzs, \
		uml_arm64_gadget_cpa_##operation##_##shift##_srr, \
		uml_arm64_gadget_cpa_##operation##_##shift##_srs, \
		uml_arm64_gadget_cpa_##operation##_##shift##_szr, \
		uml_arm64_gadget_cpa_##operation##_##shift##_szs}
#define UML_CPA_SHIFTS(operation) \
	CpaShiftGadgets{ \
		UML_CPA_MODES(operation, 0), UML_CPA_MODES(operation, 1), \
		UML_CPA_MODES(operation, 2), UML_CPA_MODES(operation, 3), \
		UML_CPA_MODES(operation, 4), UML_CPA_MODES(operation, 5), \
		UML_CPA_MODES(operation, 6), UML_CPA_MODES(operation, 7)}

[[nodiscard]] Gadget cpa_pointer_gadget(bool subtract, unsigned shift,
	bool base_sp, bool offset_zero, bool destination_sp) noexcept
{
	static constexpr std::array table{
		UML_CPA_SHIFTS(addpt), UML_CPA_SHIFTS(subpt)};
	const unsigned mode = (base_sp ? 4U : 0U) |
		(offset_zero ? 2U : 0U) | (destination_sp ? 1U : 0U);
	return table[subtract ? 1U : 0U][shift][mode];
}

#undef UML_CPA_SHIFTS
#undef UML_CPA_MODES

#define UML_CPA_MULTIPLY_MODES(operation) \
	std::array<Gadget, 16>{ \
		uml_arm64_gadget_cpa_##operation##_rrrr, \
		uml_arm64_gadget_cpa_##operation##_rrrz, \
		uml_arm64_gadget_cpa_##operation##_rrzr, \
		uml_arm64_gadget_cpa_##operation##_rrzz, \
		uml_arm64_gadget_cpa_##operation##_rzrr, \
		uml_arm64_gadget_cpa_##operation##_rzrz, \
		uml_arm64_gadget_cpa_##operation##_rzzr, \
		uml_arm64_gadget_cpa_##operation##_rzzz, \
		uml_arm64_gadget_cpa_##operation##_zrrr, \
		uml_arm64_gadget_cpa_##operation##_zrrz, \
		uml_arm64_gadget_cpa_##operation##_zrzr, \
		uml_arm64_gadget_cpa_##operation##_zrzz, \
		uml_arm64_gadget_cpa_##operation##_zzrr, \
		uml_arm64_gadget_cpa_##operation##_zzrz, \
		uml_arm64_gadget_cpa_##operation##_zzzr, \
		uml_arm64_gadget_cpa_##operation##_zzzz}

[[nodiscard]] Gadget cpa_multiply_gadget(bool subtract, bool left_zero,
	bool right_zero, bool addend_zero, bool destination_zero) noexcept
{
	static constexpr std::array table{
		UML_CPA_MULTIPLY_MODES(maddpt),
		UML_CPA_MULTIPLY_MODES(msubpt)};
	const unsigned mode = (left_zero ? 8U : 0U) |
		(right_zero ? 4U : 0U) | (addend_zero ? 2U : 0U) |
		(destination_zero ? 1U : 0U);
	return table[subtract ? 1U : 0U][mode];
}

#undef UML_CPA_MULTIPLY_MODES

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

using SimdThreeSameGadgets = std::array<Gadget, 11>;

#define UML_SIMD_THREE_SAME_V6(name) SimdThreeSameGadgets{ \
	uml_arm64_gadget_simd_three_same_##name##_8b, \
	uml_arm64_gadget_simd_three_same_##name##_4h, \
	uml_arm64_gadget_simd_three_same_##name##_2s, \
	uml_arm64_gadget_simd_three_same_##name##_16b, \
	uml_arm64_gadget_simd_three_same_##name##_8h, \
	uml_arm64_gadget_simd_three_same_##name##_4s, \
	nullptr, nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_THREE_SAME_V7(name) SimdThreeSameGadgets{ \
	uml_arm64_gadget_simd_three_same_##name##_8b, \
	uml_arm64_gadget_simd_three_same_##name##_4h, \
	uml_arm64_gadget_simd_three_same_##name##_2s, \
	uml_arm64_gadget_simd_three_same_##name##_16b, \
	uml_arm64_gadget_simd_three_same_##name##_8h, \
	uml_arm64_gadget_simd_three_same_##name##_4s, \
	uml_arm64_gadget_simd_three_same_##name##_2d, \
	nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_THREE_SAME_V7_S4(name) SimdThreeSameGadgets{ \
	uml_arm64_gadget_simd_three_same_##name##_8b, \
	uml_arm64_gadget_simd_three_same_##name##_4h, \
	uml_arm64_gadget_simd_three_same_##name##_2s, \
	uml_arm64_gadget_simd_three_same_##name##_16b, \
	uml_arm64_gadget_simd_three_same_##name##_8h, \
	uml_arm64_gadget_simd_three_same_##name##_4s, \
	uml_arm64_gadget_simd_three_same_##name##_2d, \
	uml_arm64_gadget_simd_three_same_##name##_b, \
	uml_arm64_gadget_simd_three_same_##name##_h, \
	uml_arm64_gadget_simd_three_same_##name##_s, \
	uml_arm64_gadget_simd_three_same_##name##_d}
#define UML_SIMD_THREE_SAME_V7_SD(name) SimdThreeSameGadgets{ \
	uml_arm64_gadget_simd_three_same_##name##_8b, \
	uml_arm64_gadget_simd_three_same_##name##_4h, \
	uml_arm64_gadget_simd_three_same_##name##_2s, \
	uml_arm64_gadget_simd_three_same_##name##_16b, \
	uml_arm64_gadget_simd_three_same_##name##_8h, \
	uml_arm64_gadget_simd_three_same_##name##_4s, \
	uml_arm64_gadget_simd_three_same_##name##_2d, \
	nullptr, nullptr, nullptr, \
	uml_arm64_gadget_simd_three_same_##name##_d}
#define UML_SIMD_THREE_SAME_VB(name) SimdThreeSameGadgets{ \
	uml_arm64_gadget_simd_three_same_##name##_8b, nullptr, nullptr, \
	uml_arm64_gadget_simd_three_same_##name##_16b, nullptr, nullptr, nullptr, \
	nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_THREE_SAME_HS(name) SimdThreeSameGadgets{ \
	nullptr, uml_arm64_gadget_simd_three_same_##name##_4h, \
	uml_arm64_gadget_simd_three_same_##name##_2s, nullptr, \
	uml_arm64_gadget_simd_three_same_##name##_8h, \
	uml_arm64_gadget_simd_three_same_##name##_4s, nullptr, nullptr, \
	uml_arm64_gadget_simd_three_same_##name##_h, \
	uml_arm64_gadget_simd_three_same_##name##_s, nullptr}

[[nodiscard]] Gadget simd_three_same_gadget(unsigned opcode,
	bool unsigned_group, bool scalar, bool q, unsigned size) noexcept
{
	if ((!scalar && !q && size == 3U) || size > 3U)
		return nullptr;
	const unsigned shape = scalar ? 7U + size : q ? 3U + size : size;

	static constexpr auto shadd = UML_SIMD_THREE_SAME_V6(shadd);
	static constexpr auto uhadd = UML_SIMD_THREE_SAME_V6(uhadd);
	static constexpr auto sqadd = UML_SIMD_THREE_SAME_V7_S4(sqadd);
	static constexpr auto uqadd = UML_SIMD_THREE_SAME_V7_S4(uqadd);
	static constexpr auto srhadd = UML_SIMD_THREE_SAME_V6(srhadd);
	static constexpr auto urhadd = UML_SIMD_THREE_SAME_V6(urhadd);
	static constexpr auto shsub = UML_SIMD_THREE_SAME_V6(shsub);
	static constexpr auto uhsub = UML_SIMD_THREE_SAME_V6(uhsub);
	static constexpr auto sqsub = UML_SIMD_THREE_SAME_V7_S4(sqsub);
	static constexpr auto uqsub = UML_SIMD_THREE_SAME_V7_S4(uqsub);
	static constexpr auto cmgt = UML_SIMD_THREE_SAME_V7_SD(cmgt);
	static constexpr auto cmhi = UML_SIMD_THREE_SAME_V7_SD(cmhi);
	static constexpr auto cmge = UML_SIMD_THREE_SAME_V7_SD(cmge);
	static constexpr auto cmhs = UML_SIMD_THREE_SAME_V7_SD(cmhs);
	static constexpr auto sshl = UML_SIMD_THREE_SAME_V7_SD(sshl);
	static constexpr auto ushl = UML_SIMD_THREE_SAME_V7_SD(ushl);
	static constexpr auto sqshl = UML_SIMD_THREE_SAME_V7_S4(sqshl);
	static constexpr auto uqshl = UML_SIMD_THREE_SAME_V7_S4(uqshl);
	static constexpr auto srshl = UML_SIMD_THREE_SAME_V7_SD(srshl);
	static constexpr auto urshl = UML_SIMD_THREE_SAME_V7_SD(urshl);
	static constexpr auto sqrshl = UML_SIMD_THREE_SAME_V7_S4(sqrshl);
	static constexpr auto uqrshl = UML_SIMD_THREE_SAME_V7_S4(uqrshl);
	static constexpr auto smax = UML_SIMD_THREE_SAME_V6(smax);
	static constexpr auto umax = UML_SIMD_THREE_SAME_V6(umax);
	static constexpr auto smin = UML_SIMD_THREE_SAME_V6(smin);
	static constexpr auto umin = UML_SIMD_THREE_SAME_V6(umin);
	static constexpr auto sabd = UML_SIMD_THREE_SAME_V6(sabd);
	static constexpr auto uabd = UML_SIMD_THREE_SAME_V6(uabd);
	static constexpr auto saba = UML_SIMD_THREE_SAME_V6(saba);
	static constexpr auto uaba = UML_SIMD_THREE_SAME_V6(uaba);
	static constexpr auto add = UML_SIMD_THREE_SAME_V7_SD(add);
	static constexpr auto sub = UML_SIMD_THREE_SAME_V7_SD(sub);
	static constexpr auto cmtst = UML_SIMD_THREE_SAME_V7_SD(cmtst);
	static constexpr auto cmeq = UML_SIMD_THREE_SAME_V7_SD(cmeq);
	static constexpr auto mla = UML_SIMD_THREE_SAME_V6(mla);
	static constexpr auto mls = UML_SIMD_THREE_SAME_V6(mls);
	static constexpr auto mul = UML_SIMD_THREE_SAME_V6(mul);
	static constexpr auto pmul = UML_SIMD_THREE_SAME_VB(pmul);
	static constexpr auto smaxp = UML_SIMD_THREE_SAME_V6(smaxp);
	static constexpr auto umaxp = UML_SIMD_THREE_SAME_V6(umaxp);
	static constexpr auto sminp = UML_SIMD_THREE_SAME_V6(sminp);
	static constexpr auto uminp = UML_SIMD_THREE_SAME_V6(uminp);
	static constexpr auto sqdmulh = UML_SIMD_THREE_SAME_HS(sqdmulh);
	static constexpr auto sqrdmulh = UML_SIMD_THREE_SAME_HS(sqrdmulh);
	static constexpr auto addp = UML_SIMD_THREE_SAME_V7(addp);

	switch (opcode) {
	case 0: return (unsigned_group ? uhadd : shadd)[shape];
	case 1: return (unsigned_group ? uqadd : sqadd)[shape];
	case 2: return (unsigned_group ? urhadd : srhadd)[shape];
	case 4: return (unsigned_group ? uhsub : shsub)[shape];
	case 5: return (unsigned_group ? uqsub : sqsub)[shape];
	case 6: return (unsigned_group ? cmhi : cmgt)[shape];
	case 7: return (unsigned_group ? cmhs : cmge)[shape];
	case 8: return (unsigned_group ? ushl : sshl)[shape];
	case 9: return (unsigned_group ? uqshl : sqshl)[shape];
	case 10: return (unsigned_group ? urshl : srshl)[shape];
	case 11: return (unsigned_group ? uqrshl : sqrshl)[shape];
	case 12: return (unsigned_group ? umax : smax)[shape];
	case 13: return (unsigned_group ? umin : smin)[shape];
	case 14: return (unsigned_group ? uabd : sabd)[shape];
	case 15: return (unsigned_group ? uaba : saba)[shape];
	case 16: return (unsigned_group ? sub : add)[shape];
	case 17: return (unsigned_group ? cmeq : cmtst)[shape];
	case 18: return (unsigned_group ? mls : mla)[shape];
	case 19: return (unsigned_group ? pmul : mul)[shape];
	case 20: return (unsigned_group ? umaxp : smaxp)[shape];
	case 21: return (unsigned_group ? uminp : sminp)[shape];
	case 22: return (unsigned_group ? sqrdmulh : sqdmulh)[shape];
	case 23: return unsigned_group ? nullptr : addp[shape];
	default: return nullptr;
	}
}

#undef UML_SIMD_THREE_SAME_HS
#undef UML_SIMD_THREE_SAME_VB
#undef UML_SIMD_THREE_SAME_V7_SD
#undef UML_SIMD_THREE_SAME_V7_S4
#undef UML_SIMD_THREE_SAME_V7
#undef UML_SIMD_THREE_SAME_V6

[[nodiscard]] Gadget simd_rdm_three_same_gadget(bool subtract, bool scalar,
	bool q, unsigned size) noexcept
{
	if (size != 1U && size != 2U)
		return nullptr;
#define UML_SIMD_RDM_GADGETS(name) std::array<Gadget, 6>{ \
	uml_arm64_gadget_simd_three_same_##name##_4h, \
	uml_arm64_gadget_simd_three_same_##name##_2s, \
	uml_arm64_gadget_simd_three_same_##name##_8h, \
	uml_arm64_gadget_simd_three_same_##name##_4s, \
	uml_arm64_gadget_simd_three_same_##name##_h, \
	uml_arm64_gadget_simd_three_same_##name##_s}
	static constexpr auto add = UML_SIMD_RDM_GADGETS(sqrdmlah);
	static constexpr auto sub = UML_SIMD_RDM_GADGETS(sqrdmlsh);
#undef UML_SIMD_RDM_GADGETS
	const unsigned shape = scalar ? size + 3U :
		(q ? size + 1U : size - 1U);
	return (subtract ? sub : add)[shape];
}

enum class SimdDotSignedness {
	signed_signed,
	unsigned_unsigned,
	signed_unsigned,
	unsigned_signed,
};

[[nodiscard]] Gadget simd_dot_vector_gadget(
	SimdDotSignedness signedness, bool q, bool force_software) noexcept
{
	static constexpr std::array<Gadget, 2> signed_signed{
		uml_arm64_gadget_simd_dot_vector_sdot_2s,
		uml_arm64_gadget_simd_dot_vector_sdot_4s};
	static constexpr std::array<Gadget, 2> signed_signed_software{
		uml_arm64_gadget_simd_dot_vector_sdot_2s_soft,
		uml_arm64_gadget_simd_dot_vector_sdot_4s_soft};
	static constexpr std::array<Gadget, 2> unsigned_unsigned{
		uml_arm64_gadget_simd_dot_vector_udot_2s,
		uml_arm64_gadget_simd_dot_vector_udot_4s};
	static constexpr std::array<Gadget, 2> unsigned_unsigned_software{
		uml_arm64_gadget_simd_dot_vector_udot_2s_soft,
		uml_arm64_gadget_simd_dot_vector_udot_4s_soft};
	static constexpr std::array<Gadget, 2> unsigned_signed_native{
		uml_arm64_gadget_simd_dot_vector_usdot_2s,
		uml_arm64_gadget_simd_dot_vector_usdot_4s};
	static constexpr std::array<Gadget, 2> unsigned_signed_software{
		uml_arm64_gadget_simd_dot_vector_usdot_2s_soft,
		uml_arm64_gadget_simd_dot_vector_usdot_4s_soft};
	switch (signedness) {
	case SimdDotSignedness::signed_signed:
		return (!force_software && host_has_dotprod() ? signed_signed :
			signed_signed_software)[q];
	case SimdDotSignedness::unsigned_unsigned:
		return (!force_software && host_has_dotprod() ? unsigned_unsigned :
			unsigned_unsigned_software)[q];
	case SimdDotSignedness::unsigned_signed:
		return (!force_software && host_has_i8mm() ?
			unsigned_signed_native :
			unsigned_signed_software)[q];
	case SimdDotSignedness::signed_unsigned: return nullptr;
	}
	return nullptr;
}

#define UML_SIMD_DOT_INDEXED_ARRAY(name) std::array<Gadget, 8>{ \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_0, \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_1, \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_2, \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_3, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_0, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_1, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_2, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_3}
#define UML_SIMD_DOT_INDEXED_SOFT_ARRAY(name) std::array<Gadget, 8>{ \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_0_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_1_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_2_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_3_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_0_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_1_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_2_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_3_soft}

[[nodiscard]] Gadget simd_dot_indexed_gadget(
	SimdDotSignedness signedness, bool q, unsigned index,
	bool force_software) noexcept
{
	static constexpr auto signed_signed = UML_SIMD_DOT_INDEXED_ARRAY(sdot);
	static constexpr auto unsigned_unsigned = UML_SIMD_DOT_INDEXED_ARRAY(udot);
	static constexpr auto signed_signed_software =
		UML_SIMD_DOT_INDEXED_SOFT_ARRAY(sdot);
	static constexpr auto unsigned_unsigned_software =
		UML_SIMD_DOT_INDEXED_SOFT_ARRAY(udot);
	static constexpr auto signed_unsigned_native =
		UML_SIMD_DOT_INDEXED_ARRAY(sudot);
	static constexpr auto unsigned_signed_native =
		UML_SIMD_DOT_INDEXED_ARRAY(usdot);
	static constexpr auto signed_unsigned_software =
		UML_SIMD_DOT_INDEXED_SOFT_ARRAY(sudot);
	static constexpr auto unsigned_signed_software =
		UML_SIMD_DOT_INDEXED_SOFT_ARRAY(usdot);
	const unsigned shape = (q ? 4U : 0U) + index;
	switch (signedness) {
	case SimdDotSignedness::signed_signed:
		return (!force_software && host_has_dotprod() ? signed_signed :
			signed_signed_software)[shape];
	case SimdDotSignedness::unsigned_unsigned:
		return (!force_software && host_has_dotprod() ? unsigned_unsigned :
			unsigned_unsigned_software)[shape];
	case SimdDotSignedness::signed_unsigned:
		return (!force_software && host_has_i8mm() ?
			signed_unsigned_native :
			signed_unsigned_software)[shape];
	case SimdDotSignedness::unsigned_signed:
		return (!force_software && host_has_i8mm() ?
			unsigned_signed_native :
			unsigned_signed_software)[shape];
	}
	return nullptr;
}

#undef UML_SIMD_DOT_INDEXED_SOFT_ARRAY
#undef UML_SIMD_DOT_INDEXED_ARRAY

[[nodiscard]] Gadget simd_matrix_gadget(
	SimdDotSignedness signedness, bool force_software) noexcept
{
	static constexpr std::array<Gadget, 3> native{
		uml_arm64_gadget_simd_matrix_smmla,
		uml_arm64_gadget_simd_matrix_ummla,
		uml_arm64_gadget_simd_matrix_usmmla};
	static constexpr std::array<Gadget, 3> software{
		uml_arm64_gadget_simd_matrix_smmla_soft,
		uml_arm64_gadget_simd_matrix_ummla_soft,
		uml_arm64_gadget_simd_matrix_usmmla_soft};
	unsigned index;
	switch (signedness) {
	case SimdDotSignedness::signed_signed: index = 0U; break;
	case SimdDotSignedness::unsigned_unsigned: index = 1U; break;
	case SimdDotSignedness::unsigned_signed: index = 2U; break;
	case SimdDotSignedness::signed_unsigned: return nullptr;
	}
	return (!force_software && host_has_i8mm() ?
		native : software)[index];
}

using SimdWideningGadgets = std::array<Gadget, 6>;

#define UML_SIMD_WIDENING_V6(name) SimdWideningGadgets{ \
	uml_arm64_gadget_simd_widening_##name##_8b, \
	uml_arm64_gadget_simd_widening_##name##_4h, \
	uml_arm64_gadget_simd_widening_##name##_2s, \
	uml_arm64_gadget_simd_widening_##name##_16b, \
	uml_arm64_gadget_simd_widening_##name##_8h, \
	uml_arm64_gadget_simd_widening_##name##_4s}
#define UML_SIMD_WIDENING_SAT(name) SimdWideningGadgets{ \
	nullptr, uml_arm64_gadget_simd_widening_##name##_4h, \
	uml_arm64_gadget_simd_widening_##name##_2s, nullptr, \
	uml_arm64_gadget_simd_widening_##name##_8h, \
	uml_arm64_gadget_simd_widening_##name##_4s}
#define UML_SIMD_WIDENING_SCALAR(name) std::array<Gadget, 3>{ \
	nullptr, uml_arm64_gadget_simd_widening_##name##_h, \
	uml_arm64_gadget_simd_widening_##name##_s}

[[nodiscard]] Gadget simd_widening_gadget(unsigned operation,
	bool unsigned_values, bool scalar, bool upper, unsigned size) noexcept
{
	if (size > 2U)
		return nullptr;
	static constexpr auto saddl = UML_SIMD_WIDENING_V6(saddl);
	static constexpr auto uaddl = UML_SIMD_WIDENING_V6(uaddl);
	static constexpr auto saddw = UML_SIMD_WIDENING_V6(saddw);
	static constexpr auto uaddw = UML_SIMD_WIDENING_V6(uaddw);
	static constexpr auto ssubl = UML_SIMD_WIDENING_V6(ssubl);
	static constexpr auto usubl = UML_SIMD_WIDENING_V6(usubl);
	static constexpr auto ssubw = UML_SIMD_WIDENING_V6(ssubw);
	static constexpr auto usubw = UML_SIMD_WIDENING_V6(usubw);
	static constexpr auto sabal = UML_SIMD_WIDENING_V6(sabal);
	static constexpr auto uabal = UML_SIMD_WIDENING_V6(uabal);
	static constexpr auto sabdl = UML_SIMD_WIDENING_V6(sabdl);
	static constexpr auto uabdl = UML_SIMD_WIDENING_V6(uabdl);
	static constexpr auto smlal = UML_SIMD_WIDENING_V6(smlal);
	static constexpr auto umlal = UML_SIMD_WIDENING_V6(umlal);
	static constexpr auto smlsl = UML_SIMD_WIDENING_V6(smlsl);
	static constexpr auto umlsl = UML_SIMD_WIDENING_V6(umlsl);
	static constexpr auto smull = UML_SIMD_WIDENING_V6(smull);
	static constexpr auto umull = UML_SIMD_WIDENING_V6(umull);
	static constexpr auto sqdmlal = UML_SIMD_WIDENING_SAT(sqdmlal);
	static constexpr auto sqdmlsl = UML_SIMD_WIDENING_SAT(sqdmlsl);
	static constexpr auto sqdmull = UML_SIMD_WIDENING_SAT(sqdmull);
	static constexpr auto sqdmlal_scalar = UML_SIMD_WIDENING_SCALAR(sqdmlal);
	static constexpr auto sqdmlsl_scalar = UML_SIMD_WIDENING_SCALAR(sqdmlsl);
	static constexpr auto sqdmull_scalar = UML_SIMD_WIDENING_SCALAR(sqdmull);

	if (scalar) {
		if (unsigned_values)
			return nullptr;
		switch (operation) {
		case 9: return sqdmlal_scalar[size];
		case 11: return sqdmlsl_scalar[size];
		case 13: return sqdmull_scalar[size];
		default: return nullptr;
		}
	}
	const unsigned shape = size + (upper ? 3U : 0U);
	switch (operation) {
	case 0: return (unsigned_values ? uaddl : saddl)[shape];
	case 1: return (unsigned_values ? uaddw : saddw)[shape];
	case 2: return (unsigned_values ? usubl : ssubl)[shape];
	case 3: return (unsigned_values ? usubw : ssubw)[shape];
	case 5: return (unsigned_values ? uabal : sabal)[shape];
	case 7: return (unsigned_values ? uabdl : sabdl)[shape];
	case 8: return (unsigned_values ? umlal : smlal)[shape];
	case 9: return unsigned_values ? nullptr : sqdmlal[shape];
	case 10: return (unsigned_values ? umlsl : smlsl)[shape];
	case 11: return unsigned_values ? nullptr : sqdmlsl[shape];
	case 12: return (unsigned_values ? umull : smull)[shape];
	case 13: return unsigned_values ? nullptr : sqdmull[shape];
	default: return nullptr;
	}
}

#undef UML_SIMD_WIDENING_SCALAR
#undef UML_SIMD_WIDENING_SAT
#undef UML_SIMD_WIDENING_V6

#define UML_SIMD_HIGH_NARROW(name) SimdWideningGadgets{ \
	uml_arm64_gadget_simd_widening_##name##_8h, \
	uml_arm64_gadget_simd_widening_##name##_4s, \
	uml_arm64_gadget_simd_widening_##name##_2d, \
	uml_arm64_gadget_simd_widening_##name##_8h2, \
	uml_arm64_gadget_simd_widening_##name##_4s2, \
	uml_arm64_gadget_simd_widening_##name##_2d2}

[[nodiscard]] Gadget simd_high_narrow_gadget(bool subtract, bool rounding,
	bool upper, unsigned size) noexcept
{
	if (size > 2U)
		return nullptr;
	static constexpr auto addhn = UML_SIMD_HIGH_NARROW(addhn);
	static constexpr auto raddhn = UML_SIMD_HIGH_NARROW(raddhn);
	static constexpr auto subhn = UML_SIMD_HIGH_NARROW(subhn);
	static constexpr auto rsubhn = UML_SIMD_HIGH_NARROW(rsubhn);
	const unsigned shape = size + (upper ? 3U : 0U);
	if (subtract)
		return (rounding ? rsubhn : subhn)[shape];
	return (rounding ? raddhn : addhn)[shape];
}

#undef UML_SIMD_HIGH_NARROW

using SimdUnaryGadgets = std::array<Gadget, 11>;

[[nodiscard]] constexpr std::uintptr_t simd_unary_operand(
	unsigned destination, unsigned source) noexcept
{
	const auto vector_offset = [](unsigned index) {
		return offsetof(CpuState, vectors) +
			index * sizeof(VectorRegister);
	};
	static_assert(offsetof(CpuState, vectors) +
		31U * sizeof(VectorRegister) <= 0xffffU);
	return vector_offset(destination) |
		(static_cast<std::uintptr_t>(vector_offset(source)) << 16U);
}

[[nodiscard]] constexpr std::uintptr_t simd_ternary_operand(
	unsigned destination, unsigned first, unsigned second,
	unsigned descriptor = 0U) noexcept
{
	const auto vector_offset = [](unsigned index) {
		return offsetof(CpuState, vectors) +
			index * sizeof(VectorRegister);
	};
	static_assert(offsetof(CpuState, vectors) +
		31U * sizeof(VectorRegister) <= 0xffffU);
	return vector_offset(destination) |
		(static_cast<std::uintptr_t>(vector_offset(first)) << 16U) |
		(static_cast<std::uintptr_t>(vector_offset(second)) << 32U) |
		(static_cast<std::uintptr_t>(descriptor) << 48U);
}

#define UML_SIMD_UNARY_V6(name) SimdUnaryGadgets{ \
	uml_arm64_gadget_simd_unary_##name##_8b, \
	uml_arm64_gadget_simd_unary_##name##_4h, \
	uml_arm64_gadget_simd_unary_##name##_2s, \
	uml_arm64_gadget_simd_unary_##name##_16b, \
	uml_arm64_gadget_simd_unary_##name##_8h, \
	uml_arm64_gadget_simd_unary_##name##_4s, \
	nullptr, nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_UNARY_V7(name) SimdUnaryGadgets{ \
	uml_arm64_gadget_simd_unary_##name##_8b, \
	uml_arm64_gadget_simd_unary_##name##_4h, \
	uml_arm64_gadget_simd_unary_##name##_2s, \
	uml_arm64_gadget_simd_unary_##name##_16b, \
	uml_arm64_gadget_simd_unary_##name##_8h, \
	uml_arm64_gadget_simd_unary_##name##_4s, \
	uml_arm64_gadget_simd_unary_##name##_2d, \
	nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_UNARY_V7_S4(name) SimdUnaryGadgets{ \
	uml_arm64_gadget_simd_unary_##name##_8b, \
	uml_arm64_gadget_simd_unary_##name##_4h, \
	uml_arm64_gadget_simd_unary_##name##_2s, \
	uml_arm64_gadget_simd_unary_##name##_16b, \
	uml_arm64_gadget_simd_unary_##name##_8h, \
	uml_arm64_gadget_simd_unary_##name##_4s, \
	uml_arm64_gadget_simd_unary_##name##_2d, \
	uml_arm64_gadget_simd_unary_##name##_b, \
	uml_arm64_gadget_simd_unary_##name##_h, \
	uml_arm64_gadget_simd_unary_##name##_s, \
	uml_arm64_gadget_simd_unary_##name##_d}
#define UML_SIMD_UNARY_V7_SD(name) SimdUnaryGadgets{ \
	uml_arm64_gadget_simd_unary_##name##_8b, \
	uml_arm64_gadget_simd_unary_##name##_4h, \
	uml_arm64_gadget_simd_unary_##name##_2s, \
	uml_arm64_gadget_simd_unary_##name##_16b, \
	uml_arm64_gadget_simd_unary_##name##_8h, \
	uml_arm64_gadget_simd_unary_##name##_4s, \
	uml_arm64_gadget_simd_unary_##name##_2d, \
	nullptr, nullptr, nullptr, \
	uml_arm64_gadget_simd_unary_##name##_d}

[[nodiscard]] Gadget simd_integer_unary_gadget(unsigned opcode,
	bool unsigned_group, bool scalar, bool q, unsigned size) noexcept
{
	if ((!scalar && !q && size == 3U) || size > 3U)
		return nullptr;
	const unsigned shape = scalar ? 7U + size : q ? 3U + size : size;
	static constexpr auto saddlp = UML_SIMD_UNARY_V6(saddlp);
	static constexpr auto uaddlp = UML_SIMD_UNARY_V6(uaddlp);
	static constexpr auto sadalp = UML_SIMD_UNARY_V6(sadalp);
	static constexpr auto uadalp = UML_SIMD_UNARY_V6(uadalp);
	static constexpr auto suqadd = UML_SIMD_UNARY_V7_S4(suqadd);
	static constexpr auto usqadd = UML_SIMD_UNARY_V7_S4(usqadd);
	static constexpr auto cls = UML_SIMD_UNARY_V6(cls);
	static constexpr auto clz = UML_SIMD_UNARY_V6(clz);
	static constexpr auto sqabs = UML_SIMD_UNARY_V7_S4(sqabs);
	static constexpr auto sqneg = UML_SIMD_UNARY_V7_S4(sqneg);
	static constexpr auto abs = UML_SIMD_UNARY_V7_SD(abs);
	static constexpr auto neg = UML_SIMD_UNARY_V7_SD(neg);
	static constexpr SimdWideningGadgets xtn{
		uml_arm64_gadget_simd_unary_xtn_8h,
		uml_arm64_gadget_simd_unary_xtn_4s,
		uml_arm64_gadget_simd_unary_xtn_2d,
		uml_arm64_gadget_simd_unary_xtn_8h2,
		uml_arm64_gadget_simd_unary_xtn_4s2,
		uml_arm64_gadget_simd_unary_xtn_2d2};
#define UML_SIMD_SAT_NARROW(name) SimdWideningGadgets{ \
	uml_arm64_gadget_simd_unary_##name##_8h, \
	uml_arm64_gadget_simd_unary_##name##_4s, \
	uml_arm64_gadget_simd_unary_##name##_2d, \
	uml_arm64_gadget_simd_unary_##name##_8h2, \
	uml_arm64_gadget_simd_unary_##name##_4s2, \
	uml_arm64_gadget_simd_unary_##name##_2d2}
	static constexpr auto sqxtn = UML_SIMD_SAT_NARROW(sqxtn);
	static constexpr auto uqxtn = UML_SIMD_SAT_NARROW(uqxtn);
	static constexpr auto sqxtun = UML_SIMD_SAT_NARROW(sqxtun);
	static constexpr std::array<Gadget, 3> sqxtn_scalar{
		uml_arm64_gadget_simd_unary_sqxtn_b,
		uml_arm64_gadget_simd_unary_sqxtn_h,
		uml_arm64_gadget_simd_unary_sqxtn_s};
	static constexpr std::array<Gadget, 3> uqxtn_scalar{
		uml_arm64_gadget_simd_unary_uqxtn_b,
		uml_arm64_gadget_simd_unary_uqxtn_h,
		uml_arm64_gadget_simd_unary_uqxtn_s};
	static constexpr std::array<Gadget, 3> sqxtun_scalar{
		uml_arm64_gadget_simd_unary_sqxtun_b,
		uml_arm64_gadget_simd_unary_sqxtun_h,
		uml_arm64_gadget_simd_unary_sqxtun_s};
	static constexpr auto shll = UML_SIMD_UNARY_V6(shll);
#undef UML_SIMD_SAT_NARROW

	switch (opcode) {
	case 2: return scalar ? nullptr : (unsigned_group ? uaddlp : saddlp)[shape];
	case 3: return (unsigned_group ? usqadd : suqadd)[shape];
	case 4: return scalar ? nullptr : (unsigned_group ? clz : cls)[shape];
	case 6: return scalar ? nullptr : (unsigned_group ? uadalp : sadalp)[shape];
	case 7: return (unsigned_group ? sqneg : sqabs)[shape];
	case 11: return (unsigned_group ? neg : abs)[shape];
	case 18:
		if (size > 2U)
			return nullptr;
		if (!unsigned_group)
			return scalar ? nullptr : xtn[size + (q ? 3U : 0U)];
		return scalar ? sqxtun_scalar[size] :
			sqxtun[size + (q ? 3U : 0U)];
	case 19:
		return !scalar && unsigned_group ? shll[shape] : nullptr;
	case 20:
		if (size > 2U)
			return nullptr;
		if (scalar)
			return (unsigned_group ? uqxtn_scalar : sqxtn_scalar)[size];
		return (unsigned_group ? uqxtn : sqxtn)[size + (q ? 3U : 0U)];
	default: return nullptr;
	}
}

[[nodiscard]] Gadget simd_compare_zero_native_gadget(unsigned opcode,
	bool alternate, bool scalar, bool q, unsigned size) noexcept
{
	if ((!scalar && !q && size == 3U) || (scalar && size != 3U))
		return nullptr;
	const unsigned shape = scalar ? 10U : q ? 3U + size : size;
	static constexpr auto cmgt = UML_SIMD_UNARY_V7_SD(cmgt0);
	static constexpr auto cmge = UML_SIMD_UNARY_V7_SD(cmge0);
	static constexpr auto cmeq = UML_SIMD_UNARY_V7_SD(cmeq0);
	static constexpr auto cmle = UML_SIMD_UNARY_V7_SD(cmle0);
	static constexpr auto cmlt = UML_SIMD_UNARY_V7_SD(cmlt0);
	if (opcode == 8U)
		return (alternate ? cmge : cmgt)[shape];
	if (opcode == 9U)
		return (alternate ? cmle : cmeq)[shape];
	return opcode == 10U && !alternate ? cmlt[shape] : nullptr;
}

[[nodiscard]] Gadget simd_floating_compare_gadget(
	const detail::SimdFloatingCompareDecode &decoded) noexcept
{
#define UML_SIMD_FLOATING_COMPARE_GADGETS(name) \
	std::array<Gadget, 8>{ \
		uml_arm64_gadget_simd_fcmp_##name##_h, \
		uml_arm64_gadget_simd_fcmp_##name##_s, \
		uml_arm64_gadget_simd_fcmp_##name##_d, \
		uml_arm64_gadget_simd_fcmp_##name##_4h, \
		uml_arm64_gadget_simd_fcmp_##name##_8h, \
		uml_arm64_gadget_simd_fcmp_##name##_2s, \
		uml_arm64_gadget_simd_fcmp_##name##_4s, \
		uml_arm64_gadget_simd_fcmp_##name##_2d}
	static constexpr auto equal = UML_SIMD_FLOATING_COMPARE_GADGETS(eq);
	static constexpr auto greater_equal = UML_SIMD_FLOATING_COMPARE_GADGETS(ge);
	static constexpr auto greater = UML_SIMD_FLOATING_COMPARE_GADGETS(gt);
	static constexpr auto absolute_greater_equal =
		UML_SIMD_FLOATING_COMPARE_GADGETS(facge);
	static constexpr auto absolute_greater =
		UML_SIMD_FLOATING_COMPARE_GADGETS(facgt);
	static constexpr auto equal_zero = UML_SIMD_FLOATING_COMPARE_GADGETS(eq0);
	static constexpr auto greater_equal_zero =
		UML_SIMD_FLOATING_COMPARE_GADGETS(ge0);
	static constexpr auto greater_zero = UML_SIMD_FLOATING_COMPARE_GADGETS(gt0);
	static constexpr auto less_equal_zero = UML_SIMD_FLOATING_COMPARE_GADGETS(le0);
	static constexpr auto less_zero = UML_SIMD_FLOATING_COMPARE_GADGETS(lt0);
#undef UML_SIMD_FLOATING_COMPARE_GADGETS
	using detail::SimdFloatingCompareOperation;
	if (decoded.operation == SimdFloatingCompareOperation::equal)
		return (decoded.zero ? equal_zero : equal)[decoded.shape];
	if (decoded.operation == SimdFloatingCompareOperation::greater_equal)
		return (decoded.zero ? greater_equal_zero : greater_equal)[decoded.shape];
	if (decoded.operation == SimdFloatingCompareOperation::greater)
		return (decoded.zero ? greater_zero : greater)[decoded.shape];
	if (decoded.operation == SimdFloatingCompareOperation::absolute_greater_equal)
		return absolute_greater_equal[decoded.shape];
	if (decoded.operation == SimdFloatingCompareOperation::absolute_greater)
		return absolute_greater[decoded.shape];
	if (decoded.operation == SimdFloatingCompareOperation::less_equal)
		return less_equal_zero[decoded.shape];
	return less_zero[decoded.shape];
}

[[nodiscard]] Gadget simd_floating_precision_vector_gadget(
	const detail::SimdFloatingPrecisionVectorDecode &decoded) noexcept
{
	if (decoded.round_odd)
		return decoded.scalar ? uml_arm64_gadget_simd_fcvtxn_scalar :
			decoded.upper ? uml_arm64_gadget_simd_fcvtxn2_vector :
				uml_arm64_gadget_simd_fcvtxn_vector;
	static constexpr std::array lower{
		uml_arm64_gadget_simd_fcvtl_hs,
		uml_arm64_gadget_simd_fcvtl_sd,
		uml_arm64_gadget_simd_fcvtn_sh,
		uml_arm64_gadget_simd_fcvtn_ds,
	};
	static constexpr std::array upper{
		uml_arm64_gadget_simd_fcvtl2_hs,
		uml_arm64_gadget_simd_fcvtl2_sd,
		uml_arm64_gadget_simd_fcvtn2_sh,
		uml_arm64_gadget_simd_fcvtn2_ds,
	};
	return (decoded.upper ? upper : lower)[decoded.shape];
}

[[nodiscard]] Gadget floating_to_integer_gadget(
	const detail::FloatingToIntegerDecode &decoded, bool discard) noexcept
{
	if (decoded.source_width == 16U && !host_capabilities().fp16)
		return uml_arm64_gadget_floating_to_integer_software_fallback;
	const auto fixed_table_entry = [](Gadget begin, Gadget end,
		unsigned count, unsigned index) noexcept {
		const auto first = reinterpret_cast<std::uintptr_t>(begin);
		const auto limit = reinterpret_cast<std::uintptr_t>(end);
		return reinterpret_cast<Gadget>(
			first + (limit - first) / count * index);
	};
#define UML_FIXED_TABLE(name) \
	std::array<Gadget, 2>{uml_arm64_gadget_##name, \
		uml_arm64_gadget_##name##_end}
#define UML_FLOATING_TO_GPR_FIXED(operation, suffix) \
	std::array<std::array<Gadget, 2>, 6>{ \
		UML_FIXED_TABLE(floating_to_gpr_fixed_##operation##_w_h##suffix), \
		UML_FIXED_TABLE(floating_to_gpr_fixed_##operation##_w_s##suffix), \
		UML_FIXED_TABLE(floating_to_gpr_fixed_##operation##_w_d##suffix), \
		UML_FIXED_TABLE(floating_to_gpr_fixed_##operation##_x_h##suffix), \
		UML_FIXED_TABLE(floating_to_gpr_fixed_##operation##_x_s##suffix), \
		UML_FIXED_TABLE(floating_to_gpr_fixed_##operation##_x_d##suffix)}
#define UML_SIMD_FLOATING_TO_INTEGER_FIXED(operation) \
	std::array<std::array<Gadget, 2>, 8>{ \
		UML_FIXED_TABLE(simd_fixed_##operation##_h), \
		UML_FIXED_TABLE(simd_fixed_##operation##_s), \
		UML_FIXED_TABLE(simd_fixed_##operation##_d), \
		UML_FIXED_TABLE(simd_fixed_##operation##_4h), \
		UML_FIXED_TABLE(simd_fixed_##operation##_8h), \
		UML_FIXED_TABLE(simd_fixed_##operation##_2s), \
		UML_FIXED_TABLE(simd_fixed_##operation##_4s), \
		UML_FIXED_TABLE(simd_fixed_##operation##_2d)}
	static constexpr auto signed_fixed =
		UML_FLOATING_TO_GPR_FIXED(fcvtzs, );
	static constexpr auto unsigned_fixed =
		UML_FLOATING_TO_GPR_FIXED(fcvtzu, );
	static constexpr auto signed_fixed_discard =
		UML_FLOATING_TO_GPR_FIXED(fcvtzs, _discard);
	static constexpr auto unsigned_fixed_discard =
		UML_FLOATING_TO_GPR_FIXED(fcvtzu, _discard);
	static constexpr auto signed_simd_fixed =
		UML_SIMD_FLOATING_TO_INTEGER_FIXED(fcvtzs);
	static constexpr auto unsigned_simd_fixed =
		UML_SIMD_FLOATING_TO_INTEGER_FIXED(fcvtzu);
#undef UML_SIMD_FLOATING_TO_INTEGER_FIXED
#undef UML_FLOATING_TO_GPR_FIXED
#undef UML_FIXED_TABLE
	if (decoded.fixed_point) {
		const unsigned offset = decoded.fractional_bits - 1U;
		if (!decoded.destination_gpr) {
			const auto &table =
				(decoded.unsigned_result ? unsigned_simd_fixed :
					signed_simd_fixed)[decoded.shape];
			return fixed_table_entry(table[0], table[1],
				decoded.integer_width, offset);
		}
		const unsigned index = (decoded.integer_width == 64U ? 3U : 0U) +
			decoded.shape;
		const auto &tables = discard ?
			(decoded.unsigned_result ? unsigned_fixed_discard :
				signed_fixed_discard) :
			(decoded.unsigned_result ? unsigned_fixed : signed_fixed);
		const auto &table = tables[index];
		return fixed_table_entry(table[0], table[1],
			decoded.integer_width, offset);
	}
#define UML_FLOATING_TO_GPR(operation, suffix) \
	std::array<Gadget, 6>{ \
		uml_arm64_gadget_floating_to_gpr_##operation##_w_h##suffix, \
		uml_arm64_gadget_floating_to_gpr_##operation##_w_s##suffix, \
		uml_arm64_gadget_floating_to_gpr_##operation##_w_d##suffix, \
		uml_arm64_gadget_floating_to_gpr_##operation##_x_h##suffix, \
		uml_arm64_gadget_floating_to_gpr_##operation##_x_s##suffix, \
		uml_arm64_gadget_floating_to_gpr_##operation##_x_d##suffix}
	static constexpr std::array signed_gpr{
		UML_FLOATING_TO_GPR(fcvtns, ), UML_FLOATING_TO_GPR(fcvtps, ),
		UML_FLOATING_TO_GPR(fcvtms, ), UML_FLOATING_TO_GPR(fcvtzs, ),
		UML_FLOATING_TO_GPR(fcvtas, )};
	static constexpr std::array unsigned_gpr{
		UML_FLOATING_TO_GPR(fcvtnu, ), UML_FLOATING_TO_GPR(fcvtpu, ),
		UML_FLOATING_TO_GPR(fcvtmu, ), UML_FLOATING_TO_GPR(fcvtzu, ),
		UML_FLOATING_TO_GPR(fcvtau, )};
	static constexpr std::array signed_discard{
		UML_FLOATING_TO_GPR(fcvtns, _discard),
		UML_FLOATING_TO_GPR(fcvtps, _discard),
		UML_FLOATING_TO_GPR(fcvtms, _discard),
		UML_FLOATING_TO_GPR(fcvtzs, _discard),
		UML_FLOATING_TO_GPR(fcvtas, _discard)};
	static constexpr std::array unsigned_discard{
		UML_FLOATING_TO_GPR(fcvtnu, _discard),
		UML_FLOATING_TO_GPR(fcvtpu, _discard),
		UML_FLOATING_TO_GPR(fcvtmu, _discard),
		UML_FLOATING_TO_GPR(fcvtzu, _discard),
		UML_FLOATING_TO_GPR(fcvtau, _discard)};
#undef UML_FLOATING_TO_GPR
#define UML_SIMD_FLOATING_TO_INTEGER(operation) \
	std::array<Gadget, 8>{ \
		uml_arm64_gadget_simd_##operation##_h, \
		uml_arm64_gadget_simd_##operation##_s, \
		uml_arm64_gadget_simd_##operation##_d, \
		uml_arm64_gadget_simd_##operation##_4h, \
		uml_arm64_gadget_simd_##operation##_8h, \
		uml_arm64_gadget_simd_##operation##_2s, \
		uml_arm64_gadget_simd_##operation##_4s, \
		uml_arm64_gadget_simd_##operation##_2d}
	static constexpr std::array signed_simd{
		UML_SIMD_FLOATING_TO_INTEGER(fcvtns),
		UML_SIMD_FLOATING_TO_INTEGER(fcvtps),
		UML_SIMD_FLOATING_TO_INTEGER(fcvtms),
		UML_SIMD_FLOATING_TO_INTEGER(fcvtzs),
		UML_SIMD_FLOATING_TO_INTEGER(fcvtas)};
	static constexpr std::array unsigned_simd{
		UML_SIMD_FLOATING_TO_INTEGER(fcvtnu),
		UML_SIMD_FLOATING_TO_INTEGER(fcvtpu),
		UML_SIMD_FLOATING_TO_INTEGER(fcvtmu),
		UML_SIMD_FLOATING_TO_INTEGER(fcvtzu),
		UML_SIMD_FLOATING_TO_INTEGER(fcvtau)};
#undef UML_SIMD_FLOATING_TO_INTEGER
#define UML_FLOATING_TO_SIMD_INTEGER(operation) \
	std::array<Gadget, 4>{ \
		uml_arm64_gadget_floating_to_simd_##operation##_w_h, \
		uml_arm64_gadget_floating_to_simd_##operation##_x_h, \
		uml_arm64_gadget_floating_to_simd_##operation##_x_s, \
		uml_arm64_gadget_floating_to_simd_##operation##_w_d}
	static constexpr std::array signed_fpr{
		UML_FLOATING_TO_SIMD_INTEGER(fcvtns),
		UML_FLOATING_TO_SIMD_INTEGER(fcvtps),
		UML_FLOATING_TO_SIMD_INTEGER(fcvtms),
		UML_FLOATING_TO_SIMD_INTEGER(fcvtzs),
		UML_FLOATING_TO_SIMD_INTEGER(fcvtas)};
	static constexpr std::array unsigned_fpr{
		UML_FLOATING_TO_SIMD_INTEGER(fcvtnu),
		UML_FLOATING_TO_SIMD_INTEGER(fcvtpu),
		UML_FLOATING_TO_SIMD_INTEGER(fcvtmu),
		UML_FLOATING_TO_SIMD_INTEGER(fcvtzu),
		UML_FLOATING_TO_SIMD_INTEGER(fcvtau)};
#undef UML_FLOATING_TO_SIMD_INTEGER
	const unsigned rounding = static_cast<unsigned>(decoded.rounding);
	if (!decoded.destination_gpr) {
		if (decoded.source_width != decoded.integer_width) {
			const unsigned index = decoded.source_width == 16U ?
				(decoded.integer_width == 64U ? 1U : 0U) :
				decoded.source_width == 32U ? 2U : 3U;
			return (decoded.unsigned_result ? unsigned_fpr : signed_fpr)
				[rounding][index];
		}
		return (decoded.unsigned_result ? unsigned_simd : signed_simd)
			[rounding][decoded.shape];
	}
	const unsigned index = (decoded.integer_width == 64U ? 3U : 0U) +
		decoded.shape;
	if (discard)
		return (decoded.unsigned_result ? unsigned_discard :
			signed_discard)[rounding][index];
	return (decoded.unsigned_result ? unsigned_gpr : signed_gpr)
		[rounding][index];
}

[[nodiscard]] Gadget integer_to_floating_gadget(
	const detail::IntegerToFloatingDecode &decoded, bool zero_source) noexcept
{
	const bool cross_width = !decoded.source_gpr &&
		decoded.integer_width != decoded.destination_width;
	const auto &host = host_capabilities();
	if ((cross_width && !host.has(ArmFeature::fprcvt)) ||
	    (!cross_width && decoded.destination_width == 16U && !host.fp16))
		return uml_arm64_gadget_integer_to_floating_software_fallback;
	const auto fixed_table_entry = [](Gadget begin, Gadget end,
		unsigned count, unsigned index) noexcept {
		const auto first = reinterpret_cast<std::uintptr_t>(begin);
		const auto limit = reinterpret_cast<std::uintptr_t>(end);
		return reinterpret_cast<Gadget>(
			first + (limit - first) / count * index);
	};
#define UML_INTEGER_FLOAT_FIXED_TABLE(name) \
	std::array<Gadget, 2>{uml_arm64_gadget_##name, \
		uml_arm64_gadget_##name##_end}
#define UML_GPR_TO_FLOATING_FIXED(operation, zero) \
	std::array<std::array<Gadget, 2>, 6>{ \
		UML_INTEGER_FLOAT_FIXED_TABLE( \
			gpr_to_floating_fixed_##operation##_w_h##zero), \
		UML_INTEGER_FLOAT_FIXED_TABLE( \
			gpr_to_floating_fixed_##operation##_w_s##zero), \
		UML_INTEGER_FLOAT_FIXED_TABLE( \
			gpr_to_floating_fixed_##operation##_w_d##zero), \
		UML_INTEGER_FLOAT_FIXED_TABLE( \
			gpr_to_floating_fixed_##operation##_x_h##zero), \
		UML_INTEGER_FLOAT_FIXED_TABLE( \
			gpr_to_floating_fixed_##operation##_x_s##zero), \
		UML_INTEGER_FLOAT_FIXED_TABLE( \
			gpr_to_floating_fixed_##operation##_x_d##zero)}
#define UML_SIMD_INTEGER_TO_FLOATING_FIXED(operation) \
	std::array<std::array<Gadget, 2>, 8>{ \
		UML_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_h), \
		UML_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_s), \
		UML_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_d), \
		UML_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_4h), \
		UML_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_8h), \
		UML_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_2s), \
		UML_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_4s), \
		UML_INTEGER_FLOAT_FIXED_TABLE(simd_fixed_##operation##_2d)}
	static constexpr auto signed_gpr_fixed =
		UML_GPR_TO_FLOATING_FIXED(scvtf, 0);
	static constexpr auto unsigned_gpr_fixed =
		UML_GPR_TO_FLOATING_FIXED(ucvtf, 0);
	static constexpr auto signed_zero_fixed =
		UML_GPR_TO_FLOATING_FIXED(scvtf, 1);
	static constexpr auto unsigned_zero_fixed =
		UML_GPR_TO_FLOATING_FIXED(ucvtf, 1);
	static constexpr auto signed_simd_fixed =
		UML_SIMD_INTEGER_TO_FLOATING_FIXED(scvtf);
	static constexpr auto unsigned_simd_fixed =
		UML_SIMD_INTEGER_TO_FLOATING_FIXED(ucvtf);
#undef UML_SIMD_INTEGER_TO_FLOATING_FIXED
#undef UML_GPR_TO_FLOATING_FIXED
#undef UML_INTEGER_FLOAT_FIXED_TABLE
	if (decoded.fixed_point) {
		const unsigned offset = decoded.fractional_bits - 1U;
		if (!decoded.source_gpr) {
			const auto &table =
				(decoded.unsigned_source ? unsigned_simd_fixed :
					signed_simd_fixed)[decoded.shape];
			return fixed_table_entry(table[0], table[1],
				decoded.integer_width, offset);
		}
		const unsigned index = (decoded.integer_width == 64U ? 3U : 0U) +
			decoded.shape;
		const auto &tables = zero_source ?
			(decoded.unsigned_source ? unsigned_zero_fixed :
				signed_zero_fixed) :
			(decoded.unsigned_source ? unsigned_gpr_fixed :
				signed_gpr_fixed);
		const auto &table = tables[index];
		return fixed_table_entry(table[0], table[1],
			decoded.integer_width, offset);
	}
#define UML_GPR_TO_FLOATING(operation, zero) \
	std::array<Gadget, 6>{ \
		uml_arm64_gadget_gpr_to_floating_##operation##_w_h##zero, \
		uml_arm64_gadget_gpr_to_floating_##operation##_w_s##zero, \
		uml_arm64_gadget_gpr_to_floating_##operation##_w_d##zero, \
		uml_arm64_gadget_gpr_to_floating_##operation##_x_h##zero, \
		uml_arm64_gadget_gpr_to_floating_##operation##_x_s##zero, \
		uml_arm64_gadget_gpr_to_floating_##operation##_x_d##zero}
	static constexpr auto signed_gpr = UML_GPR_TO_FLOATING(scvtf, 0);
	static constexpr auto unsigned_gpr = UML_GPR_TO_FLOATING(ucvtf, 0);
	static constexpr auto signed_zero = UML_GPR_TO_FLOATING(scvtf, 1);
	static constexpr auto unsigned_zero = UML_GPR_TO_FLOATING(ucvtf, 1);
#undef UML_GPR_TO_FLOATING
#define UML_SIMD_INTEGER_TO_FLOATING(operation) \
	std::array<Gadget, 8>{ \
		uml_arm64_gadget_simd_##operation##_h, \
		uml_arm64_gadget_simd_##operation##_s, \
		uml_arm64_gadget_simd_##operation##_d, \
		uml_arm64_gadget_simd_##operation##_4h, \
		uml_arm64_gadget_simd_##operation##_8h, \
		uml_arm64_gadget_simd_##operation##_2s, \
		uml_arm64_gadget_simd_##operation##_4s, \
		uml_arm64_gadget_simd_##operation##_2d}
	static constexpr auto signed_simd = UML_SIMD_INTEGER_TO_FLOATING(scvtf);
	static constexpr auto unsigned_simd = UML_SIMD_INTEGER_TO_FLOATING(ucvtf);
#undef UML_SIMD_INTEGER_TO_FLOATING
#define UML_SIMD_CROSS_INTEGER_TO_FLOATING(operation) \
	std::array<Gadget, 4>{ \
		uml_arm64_gadget_simd_cross_##operation##_h_w, \
		uml_arm64_gadget_simd_cross_##operation##_h_x, \
		uml_arm64_gadget_simd_cross_##operation##_s_x, \
		uml_arm64_gadget_simd_cross_##operation##_d_w}
	static constexpr auto signed_cross =
		UML_SIMD_CROSS_INTEGER_TO_FLOATING(scvtf);
	static constexpr auto unsigned_cross =
		UML_SIMD_CROSS_INTEGER_TO_FLOATING(ucvtf);
#undef UML_SIMD_CROSS_INTEGER_TO_FLOATING
	if (decoded.source_gpr) {
		const unsigned index = (decoded.integer_width == 64U ? 3U : 0U) +
			decoded.shape;
		if (zero_source)
			return (decoded.unsigned_source ? unsigned_zero :
				signed_zero)[index];
		return (decoded.unsigned_source ? unsigned_gpr : signed_gpr)[index];
	}
	if (decoded.integer_width != decoded.destination_width) {
		const unsigned index = decoded.destination_width == 16U ?
			(decoded.integer_width == 64U ? 1U : 0U) :
			decoded.destination_width == 32U ? 2U : 3U;
		return (decoded.unsigned_source ? unsigned_cross :
			signed_cross)[index];
	}
	return (decoded.unsigned_source ? unsigned_simd : signed_simd)[
		decoded.shape];
}

[[nodiscard]] Gadget simd_reverse_native_gadget(unsigned block_bits,
	bool q, unsigned size) noexcept
{
	static constexpr SimdWideningGadgets reverse64{
		uml_arm64_gadget_simd_unary_rev64_8b,
		uml_arm64_gadget_simd_unary_rev64_4h,
		uml_arm64_gadget_simd_unary_rev64_2s,
		uml_arm64_gadget_simd_unary_rev64_16b,
		uml_arm64_gadget_simd_unary_rev64_8h,
		uml_arm64_gadget_simd_unary_rev64_4s};
	static constexpr SimdWideningGadgets reverse32{
		uml_arm64_gadget_simd_unary_rev32_8b,
		uml_arm64_gadget_simd_unary_rev32_4h, nullptr,
		uml_arm64_gadget_simd_unary_rev32_16b,
		uml_arm64_gadget_simd_unary_rev32_8h, nullptr};
	static constexpr SimdWideningGadgets reverse16{
		uml_arm64_gadget_simd_unary_rev16_8b, nullptr, nullptr,
		uml_arm64_gadget_simd_unary_rev16_16b, nullptr, nullptr};
	if (size > 2U)
		return nullptr;
	const auto &table = block_bits == 64U ? reverse64 :
		block_bits == 32U ? reverse32 : reverse16;
	return table[size + (q ? 3U : 0U)];
}

[[nodiscard]] Gadget simd_byte_unary_gadget(unsigned operation, bool q) noexcept
{
	static constexpr std::array<Gadget, 2> count{
		uml_arm64_gadget_simd_unary_cnt_8b,
		uml_arm64_gadget_simd_unary_cnt_16b};
	static constexpr std::array<Gadget, 2> invert{
		uml_arm64_gadget_simd_unary_not_8b,
		uml_arm64_gadget_simd_unary_not_16b};
	static constexpr std::array<Gadget, 2> reverse{
		uml_arm64_gadget_simd_unary_rbit_8b,
		uml_arm64_gadget_simd_unary_rbit_16b};
	return operation == 0U ? count[q] : operation == 1U ? invert[q] : reverse[q];
}

[[nodiscard]] Gadget simd_integer_reduce_gadget(unsigned opcode,
	bool unsigned_group, bool q, unsigned size) noexcept
{
	if (size > 2U || (!q && size == 2U))
		return nullptr;
	const unsigned shape = q ? size + 2U : size;
#define UML_SIMD_REDUCE_5(name) std::array<Gadget, 5>{ \
	uml_arm64_gadget_simd_reduce_##name##_8b, \
	uml_arm64_gadget_simd_reduce_##name##_4h, \
	uml_arm64_gadget_simd_reduce_##name##_16b, \
	uml_arm64_gadget_simd_reduce_##name##_8h, \
	uml_arm64_gadget_simd_reduce_##name##_4s}
	static constexpr auto saddlv = UML_SIMD_REDUCE_5(saddlv);
	static constexpr auto uaddlv = UML_SIMD_REDUCE_5(uaddlv);
	static constexpr auto smaxv = UML_SIMD_REDUCE_5(smaxv);
	static constexpr auto umaxv = UML_SIMD_REDUCE_5(umaxv);
	static constexpr auto sminv = UML_SIMD_REDUCE_5(sminv);
	static constexpr auto uminv = UML_SIMD_REDUCE_5(uminv);
	static constexpr auto addv = UML_SIMD_REDUCE_5(addv);
#undef UML_SIMD_REDUCE_5
	if (opcode == 3U)
		return (unsigned_group ? uaddlv : saddlv)[shape];
	if (opcode == 10U)
		return (unsigned_group ? umaxv : smaxv)[shape];
	if (opcode == 26U)
		return (unsigned_group ? uminv : sminv)[shape];
	return opcode == 27U && !unsigned_group ? addv[shape] : nullptr;
}

[[nodiscard]] Gadget simd_unsigned_estimate_gadget(
	bool reciprocal_sqrt, bool q, bool invalid_size) noexcept
{
	if (invalid_size)
		return nullptr;
	if (reciprocal_sqrt)
		return q ? uml_arm64_gadget_simd_unary_ursqrte_4s :
			uml_arm64_gadget_simd_unary_ursqrte_2s;
	return q ? uml_arm64_gadget_simd_unary_urecpe_4s :
		uml_arm64_gadget_simd_unary_urecpe_2s;
}

#undef UML_SIMD_UNARY_V7_SD
#undef UML_SIMD_UNARY_V7_S4
#undef UML_SIMD_UNARY_V7
#undef UML_SIMD_UNARY_V6

[[nodiscard]] Gadget floating_arithmetic_gadget(
	unsigned type, bool one_source, unsigned operation) noexcept
{
	if (one_source) {
		switch (operation) {
		case 0:
			if (type == 3U)
				return uml_arm64_gadget_floating_move_half;
			return type == 1U ? uml_arm64_gadget_floating_move_double :
				uml_arm64_gadget_floating_move_single;
		case 1:
			if (type == 3U)
				return uml_arm64_gadget_floating_absolute_half;
			return type == 1U ? uml_arm64_gadget_floating_absolute_double :
				uml_arm64_gadget_floating_absolute_single;
		case 2:
			if (type == 3U)
				return uml_arm64_gadget_floating_negate_half;
			return type == 1U ? uml_arm64_gadget_floating_negate_double :
				uml_arm64_gadget_floating_negate_single;
		default: return type == 1U ? uml_arm64_gadget_floating_sqrt_double :
			uml_arm64_gadget_floating_sqrt_single;
		}
	}
	switch (operation) {
	case 0: return type == 1U ? uml_arm64_gadget_floating_multiply_double :
		uml_arm64_gadget_floating_multiply_single;
	case 1: return type == 1U ? uml_arm64_gadget_floating_divide_double :
		uml_arm64_gadget_floating_divide_single;
	case 2: return type == 1U ? uml_arm64_gadget_floating_add_double :
		uml_arm64_gadget_floating_add_single;
	case 3: return type == 1U ? uml_arm64_gadget_floating_subtract_double :
		uml_arm64_gadget_floating_subtract_single;
	default: return type == 1U ?
		uml_arm64_gadget_floating_negative_multiply_double :
		uml_arm64_gadget_floating_negative_multiply_single;
	}
}

[[nodiscard]] Gadget basic_floating_arithmetic_gadget(
	const detail::BasicFloatingArithmetic &decoded) noexcept
{
	using Operation = detail::BasicFloatingOperation;
	if (decoded.scalar) {
		switch (decoded.operation) {
		case Operation::multiply:
			return decoded.width == 16U ?
				uml_arm64_gadget_floating_multiply_half :
				decoded.width == 32U ?
				uml_arm64_gadget_floating_multiply_single :
				uml_arm64_gadget_floating_multiply_double;
		case Operation::divide:
			return decoded.width == 16U ?
				uml_arm64_gadget_floating_divide_half :
				decoded.width == 32U ?
				uml_arm64_gadget_floating_divide_single :
				uml_arm64_gadget_floating_divide_double;
		case Operation::add:
			return decoded.width == 16U ?
				uml_arm64_gadget_floating_add_half :
				decoded.width == 32U ?
				uml_arm64_gadget_floating_add_single :
				uml_arm64_gadget_floating_add_double;
		case Operation::subtract:
			return decoded.width == 16U ?
				uml_arm64_gadget_floating_subtract_half :
				decoded.width == 32U ?
				uml_arm64_gadget_floating_subtract_single :
				uml_arm64_gadget_floating_subtract_double;
		case Operation::negative_multiply:
			return decoded.width == 16U ?
				uml_arm64_gadget_floating_negative_multiply_half :
				decoded.width == 32U ?
				uml_arm64_gadget_floating_negative_multiply_single :
				uml_arm64_gadget_floating_negative_multiply_double;
		case Operation::square_root:
			return decoded.width == 16U ?
				uml_arm64_gadget_floating_sqrt_half :
				decoded.width == 32U ?
				uml_arm64_gadget_floating_sqrt_single :
				uml_arm64_gadget_floating_sqrt_double;
		}
	}
	const unsigned shape = decoded.width == 16U ?
		(decoded.vector_bytes == 16U ? 1U : 0U) :
		decoded.width == 32U ?
		(decoded.vector_bytes == 16U ? 3U : 2U) : 4U;
#define UML_BASIC_FLOATING_VECTOR(operation) std::array<Gadget, 5>{ \
	uml_arm64_gadget_simd_basic_##operation##_4h, \
	uml_arm64_gadget_simd_basic_##operation##_8h, \
	uml_arm64_gadget_simd_basic_##operation##_2s, \
	uml_arm64_gadget_simd_basic_##operation##_4s, \
	uml_arm64_gadget_simd_basic_##operation##_2d}
	static constexpr auto multiply = UML_BASIC_FLOATING_VECTOR(fmul);
	static constexpr auto divide = UML_BASIC_FLOATING_VECTOR(fdiv);
	static constexpr auto add = UML_BASIC_FLOATING_VECTOR(fadd);
	static constexpr auto subtract = UML_BASIC_FLOATING_VECTOR(fsub);
	static constexpr auto square_root = UML_BASIC_FLOATING_VECTOR(fsqrt);
#undef UML_BASIC_FLOATING_VECTOR
	switch (decoded.operation) {
	case Operation::multiply: return multiply[shape];
	case Operation::divide: return divide[shape];
	case Operation::add: return add[shape];
	case Operation::subtract: return subtract[shape];
	case Operation::square_root: return square_root[shape];
	default: return nullptr;
	}
}

[[nodiscard]] unsigned floating_round_operation_index(
	detail::FloatingRoundOperation operation) noexcept
{
	using Operation = detail::FloatingRoundOperation;
	switch (operation) {
	case Operation::nearest: return 0;
	case Operation::plus: return 1;
	case Operation::minus: return 2;
	case Operation::zero: return 3;
	case Operation::ties_away: return 4;
	case Operation::exact_current: return 5;
	case Operation::current: return 6;
	case Operation::int32_exact: return 0;
	case Operation::int32_zero: return 1;
	case Operation::int64_exact: return 2;
	case Operation::int64_zero: return 3;
	}
	return 0;
}

[[nodiscard]] Gadget floating_round_gadget(
	const detail::FloatingRound &decoded) noexcept
{
	const unsigned operation =
		floating_round_operation_index(decoded.operation);
	if (!detail::is_sized_round(decoded.operation)) {
#define UML_SCALAR_ROUND(name) std::array<Gadget, 3>{ \
	uml_arm64_gadget_floating_round_##name##_h, \
	uml_arm64_gadget_floating_round_##name##_s, \
	uml_arm64_gadget_floating_round_##name##_d}
		static constexpr std::array scalar{
			UML_SCALAR_ROUND(n), UML_SCALAR_ROUND(p),
			UML_SCALAR_ROUND(m), UML_SCALAR_ROUND(z),
			UML_SCALAR_ROUND(a), UML_SCALAR_ROUND(x),
			UML_SCALAR_ROUND(i)};
#undef UML_SCALAR_ROUND
#define UML_VECTOR_ROUND(name) std::array<Gadget, 5>{ \
	uml_arm64_gadget_simd_round_##name##_4h, \
	uml_arm64_gadget_simd_round_##name##_8h, \
	uml_arm64_gadget_simd_round_##name##_2s, \
	uml_arm64_gadget_simd_round_##name##_4s, \
	uml_arm64_gadget_simd_round_##name##_2d}
		static constexpr std::array vector{
			UML_VECTOR_ROUND(n), UML_VECTOR_ROUND(p),
			UML_VECTOR_ROUND(m), UML_VECTOR_ROUND(z),
			UML_VECTOR_ROUND(a), UML_VECTOR_ROUND(x),
			UML_VECTOR_ROUND(i)};
#undef UML_VECTOR_ROUND
		if (decoded.scalar) {
			const unsigned shape = decoded.width == 16U ? 0U :
				decoded.width == 32U ? 1U : 2U;
			return scalar[operation][shape];
		}
		const unsigned shape = decoded.width == 16U ?
			(decoded.vector_bytes == 16U ? 1U : 0U) :
			decoded.width == 32U ?
			(decoded.vector_bytes == 16U ? 3U : 2U) : 4U;
		return vector[operation][shape];
	}
	if (!host_has_frintts())
		return uml_arm64_gadget_floating_round_sized_software;
	static constexpr std::array scalar{
		std::array<Gadget, 2>{uml_arm64_gadget_floating_round_32x_s,
			uml_arm64_gadget_floating_round_32x_d},
		std::array<Gadget, 2>{uml_arm64_gadget_floating_round_32z_s,
			uml_arm64_gadget_floating_round_32z_d},
		std::array<Gadget, 2>{uml_arm64_gadget_floating_round_64x_s,
			uml_arm64_gadget_floating_round_64x_d},
		std::array<Gadget, 2>{uml_arm64_gadget_floating_round_64z_s,
			uml_arm64_gadget_floating_round_64z_d},
	};
	static constexpr std::array vector{
		std::array<Gadget, 3>{uml_arm64_gadget_simd_round_32x_2s,
			uml_arm64_gadget_simd_round_32x_4s,
			uml_arm64_gadget_simd_round_32x_2d},
		std::array<Gadget, 3>{uml_arm64_gadget_simd_round_32z_2s,
			uml_arm64_gadget_simd_round_32z_4s,
			uml_arm64_gadget_simd_round_32z_2d},
		std::array<Gadget, 3>{uml_arm64_gadget_simd_round_64x_2s,
			uml_arm64_gadget_simd_round_64x_4s,
			uml_arm64_gadget_simd_round_64x_2d},
		std::array<Gadget, 3>{uml_arm64_gadget_simd_round_64z_2s,
			uml_arm64_gadget_simd_round_64z_4s,
			uml_arm64_gadget_simd_round_64z_2d},
	};
	if (decoded.scalar)
		return scalar[operation][decoded.width == 64U ? 1U : 0U];
	const unsigned shape = decoded.width == 64U ? 2U :
		decoded.vector_bytes == 16U ? 1U : 0U;
	return vector[operation][shape];
}

[[nodiscard]] Gadget floating_horizontal_gadget(
	const detail::FloatingHorizontal &decoded) noexcept
{
	const unsigned operation = static_cast<unsigned>(decoded.operation);
	const unsigned scalar_shape = decoded.width == 16U ? 0U :
		decoded.width == 32U ? 1U : 2U;
	const unsigned vector_shape = decoded.width == 16U ?
		(decoded.vector_bytes == 16U ? 1U : 0U) :
		decoded.width == 32U ?
		(decoded.vector_bytes == 16U ? 3U : 2U) : 4U;
#define UML_HORIZONTAL_SCALAR(name) std::array<Gadget, 3>{ \
	uml_arm64_gadget_floating_horizontal_##name##_h, \
	uml_arm64_gadget_floating_horizontal_##name##_s, \
	uml_arm64_gadget_floating_horizontal_##name##_d}
#define UML_HORIZONTAL_VECTOR(name) std::array<Gadget, 5>{ \
	uml_arm64_gadget_floating_horizontal_##name##_4h, \
	uml_arm64_gadget_floating_horizontal_##name##_8h, \
	uml_arm64_gadget_floating_horizontal_##name##_2s, \
	uml_arm64_gadget_floating_horizontal_##name##_4s, \
	uml_arm64_gadget_floating_horizontal_##name##_2d}
	if (decoded.operation ==
			detail::FloatingHorizontalOperation::absolute_maximum ||
	    decoded.operation ==
			detail::FloatingHorizontalOperation::absolute_minimum) {
		if (!host_has_faminmax())
			return uml_arm64_gadget_floating_horizontal_absolute_software;
		static constexpr auto maximum = UML_HORIZONTAL_VECTOR(famax_native);
		static constexpr auto minimum = UML_HORIZONTAL_VECTOR(famin_native);
		return (decoded.operation == detail::FloatingHorizontalOperation::
			absolute_maximum ? maximum : minimum)[vector_shape];
	}
	static constexpr std::array scalar_binary{
		UML_HORIZONTAL_SCALAR(fabd_scalar),
		std::array<Gadget, 3>{},
		UML_HORIZONTAL_SCALAR(fmax_scalar),
		UML_HORIZONTAL_SCALAR(fmaxnm_scalar),
		UML_HORIZONTAL_SCALAR(fmin_scalar),
		UML_HORIZONTAL_SCALAR(fminnm_scalar)};
	static constexpr std::array vector_binary{
		UML_HORIZONTAL_VECTOR(fabd_vector),
		std::array<Gadget, 5>{},
		UML_HORIZONTAL_VECTOR(fmax_vector),
		UML_HORIZONTAL_VECTOR(fmaxnm_vector),
		UML_HORIZONTAL_VECTOR(fmin_vector),
		UML_HORIZONTAL_VECTOR(fminnm_vector)};
	static constexpr std::array scalar_pairwise{
		std::array<Gadget, 3>{},
		UML_HORIZONTAL_SCALAR(faddp_scalar),
		UML_HORIZONTAL_SCALAR(fmaxp_scalar),
		UML_HORIZONTAL_SCALAR(fmaxnmp_scalar),
		UML_HORIZONTAL_SCALAR(fminp_scalar),
		UML_HORIZONTAL_SCALAR(fminnmp_scalar)};
	static constexpr std::array vector_pairwise{
		std::array<Gadget, 5>{},
		UML_HORIZONTAL_VECTOR(faddp_vector),
		UML_HORIZONTAL_VECTOR(fmaxp_vector),
		UML_HORIZONTAL_VECTOR(fmaxnmp_vector),
		UML_HORIZONTAL_VECTOR(fminp_vector),
		UML_HORIZONTAL_VECTOR(fminnmp_vector)};
	static constexpr std::array scalar_arithmetic_checked{
		UML_HORIZONTAL_SCALAR(fabd_checked_scalar),
		UML_HORIZONTAL_SCALAR(faddp_checked_scalar)};
	static constexpr std::array vector_arithmetic_checked{
		UML_HORIZONTAL_VECTOR(fabd_checked_vector),
		UML_HORIZONTAL_VECTOR(faddp_checked_vector)};
	static constexpr std::array scalar_ah{
		std::array{UML_HORIZONTAL_SCALAR(fmax_ah_scalar),
			UML_HORIZONTAL_SCALAR(fmaxp_ah_scalar)},
		std::array{UML_HORIZONTAL_SCALAR(fmaxnm_ah_scalar),
			UML_HORIZONTAL_SCALAR(fmaxnmp_ah_scalar)},
		std::array{UML_HORIZONTAL_SCALAR(fmin_ah_scalar),
			UML_HORIZONTAL_SCALAR(fminp_ah_scalar)},
		std::array{UML_HORIZONTAL_SCALAR(fminnm_ah_scalar),
			UML_HORIZONTAL_SCALAR(fminnmp_ah_scalar)}};
	static constexpr std::array vector_ah{
		std::array{UML_HORIZONTAL_VECTOR(fmax_ah_vector),
			UML_HORIZONTAL_VECTOR(fmaxp_ah_vector)},
		std::array{UML_HORIZONTAL_VECTOR(fmaxnm_ah_vector),
			UML_HORIZONTAL_VECTOR(fmaxnmp_ah_vector)},
		std::array{UML_HORIZONTAL_VECTOR(fmin_ah_vector),
			UML_HORIZONTAL_VECTOR(fminp_ah_vector)},
		std::array{UML_HORIZONTAL_VECTOR(fminnm_ah_vector),
			UML_HORIZONTAL_VECTOR(fminnmp_ah_vector)}};
#undef UML_HORIZONTAL_VECTOR
#undef UML_HORIZONTAL_SCALAR

	using Form = detail::FloatingHorizontalForm;
	using Operation = detail::FloatingHorizontalOperation;
	if (!host_has_afp() && operation <=
			static_cast<unsigned>(Operation::add)) {
		if (decoded.form == Form::scalar_binary ||
		    decoded.form == Form::scalar_pairwise)
			return scalar_arithmetic_checked[operation][scalar_shape];
		return vector_arithmetic_checked[operation][vector_shape];
	}
	if (decoded.form != Form::reduction &&
	    detail::floating_horizontal_uses_ah(decoded) && !host_has_afp()) {
		const unsigned pair = decoded.form == Form::scalar_pairwise ||
			decoded.form == Form::vector_pairwise;
		const unsigned ah_operation = operation -
			static_cast<unsigned>(Operation::maximum);
		if (decoded.form == Form::scalar_binary ||
		    decoded.form == Form::scalar_pairwise)
			return scalar_ah[ah_operation][pair][scalar_shape];
		return vector_ah[ah_operation][pair][vector_shape];
	}
	if (decoded.form == Form::scalar_binary)
		return scalar_binary[operation][scalar_shape];
	if (decoded.form == Form::vector_binary)
		return vector_binary[operation][vector_shape];
	if (decoded.form == Form::scalar_pairwise)
		return scalar_pairwise[operation][scalar_shape];
	if (decoded.form == Form::vector_pairwise)
		return vector_pairwise[operation][vector_shape];

#define UML_HORIZONTAL_REDUCE(name) std::array<Gadget, 3>{ \
	uml_arm64_gadget_floating_horizontal_##name##_4h, \
	uml_arm64_gadget_floating_horizontal_##name##_8h, \
	uml_arm64_gadget_floating_horizontal_##name##_4s}
	static constexpr std::array reduction{
		std::array<Gadget, 3>{}, std::array<Gadget, 3>{},
		UML_HORIZONTAL_REDUCE(fmaxv),
		UML_HORIZONTAL_REDUCE(fmaxnmv),
		UML_HORIZONTAL_REDUCE(fminv),
		UML_HORIZONTAL_REDUCE(fminnmv)};
	static constexpr std::array reduction_ah{
		UML_HORIZONTAL_REDUCE(fmaxv_ah),
		UML_HORIZONTAL_REDUCE(fmaxnmv_ah),
		UML_HORIZONTAL_REDUCE(fminv_ah),
		UML_HORIZONTAL_REDUCE(fminnmv_ah)};
#undef UML_HORIZONTAL_REDUCE
	const unsigned shape = decoded.width == 32U ? 2U :
		decoded.vector_bytes == 16U ? 1U : 0U;
	if (detail::floating_horizontal_uses_ah(decoded) && !host_has_afp())
		return reduction_ah[operation -
			static_cast<unsigned>(Operation::maximum)][shape];
	return reduction[operation][shape];
}

[[nodiscard]] Gadget floating_scale_gadget(
	const detail::FloatingScale &decoded) noexcept
{
	static constexpr std::array native{
		uml_arm64_gadget_floating_scale_native_4h,
		uml_arm64_gadget_floating_scale_native_8h,
		uml_arm64_gadget_floating_scale_native_2s,
		uml_arm64_gadget_floating_scale_native_4s,
		uml_arm64_gadget_floating_scale_native_2d};
	static constexpr std::array software{
		uml_arm64_gadget_floating_scale_software_4h,
		uml_arm64_gadget_floating_scale_software_8h,
		uml_arm64_gadget_floating_scale_software_2s,
		uml_arm64_gadget_floating_scale_software_4s,
		uml_arm64_gadget_floating_scale_software_2d};
	const unsigned shape = decoded.width == 16U ?
		(decoded.vector_bytes == 16U ? 1U : 0U) :
		decoded.width == 32U ?
			(decoded.vector_bytes == 16U ? 3U : 2U) : 4U;
	return (host_has_fp8() ? native : software)[shape];
}

[[nodiscard]] Gadget fp8_gadget(const detail::Fp8Instruction &decoded) noexcept
{
	using Operation = detail::Fp8Operation;
	if (decoded.operation == Operation::convert_to_half ||
	    decoded.operation == Operation::convert_to_bfloat) {
		if (!host_has_fp8())
			return uml_arm64_gadget_fp8_software;
		const unsigned format = decoded.operation == Operation::convert_to_bfloat ?
			1U : 0U;
		return uml_arm64_fp8_native_up_table[format * 4U +
			(decoded.upper ? 2U : 0U) + decoded.source2];
	}
	if (decoded.operation == Operation::convert_from_half ||
	    decoded.operation == Operation::convert_from_single) {
		if (!host_has_fp8())
			return uml_arm64_gadget_fp8_software;
		const unsigned index = decoded.operation == Operation::convert_from_half ?
			(decoded.lanes == 8U ? 1U : 0U) : (decoded.upper ? 3U : 2U);
		return uml_arm64_fp8_native_down_table[index];
	}
	if (decoded.operation == Operation::dot_half) {
		if (!host_has_fp8dot2() || !host_has_afp())
			return uml_arm64_gadget_fp8_software;
		const unsigned q = decoded.lanes == 8U;
		return decoded.by_element ?
			uml_arm64_fp8_native_dot_h_indexed_table[q * 8U + decoded.index] :
			uml_arm64_fp8_native_dot_h_vector_table[q];
	}
	if (decoded.operation == Operation::dot_single) {
		if (!host_has_fp8dot4() || !host_has_afp())
			return uml_arm64_gadget_fp8_software;
		const unsigned q = decoded.lanes == 4U;
		return decoded.by_element ?
			uml_arm64_fp8_native_dot_s_indexed_table[q * 4U + decoded.index] :
			uml_arm64_fp8_native_dot_s_vector_table[q];
	}
	if (decoded.operation == Operation::multiply_half) {
		if (!host_has_fp8fma() || !host_has_afp())
			return uml_arm64_gadget_fp8_software;
		return decoded.by_element ?
			uml_arm64_fp8_native_mul_h_indexed_table[
				decoded.selection * 16U + decoded.index] :
			uml_arm64_fp8_native_mul_h_vector_table[decoded.selection];
	}
	if (decoded.operation == Operation::multiply_single) {
		if (!host_has_fp8fma() || !host_has_afp())
			return uml_arm64_gadget_fp8_software;
		return decoded.by_element ?
			uml_arm64_fp8_native_mul_s_indexed_table[
				decoded.selection * 16U + decoded.index] :
			uml_arm64_fp8_native_mul_s_vector_table[decoded.selection];
	}
	if (decoded.operation == Operation::matrix_half)
		return host_has_f8f16mm() && host_has_afp() ?
			uml_arm64_fp8_native_matrix_table[0] :
			uml_arm64_gadget_fp8_software;
	return host_has_f8f32mm() && host_has_afp() ?
		uml_arm64_fp8_native_matrix_table[1] :
		uml_arm64_gadget_fp8_software;
}

[[nodiscard]] Gadget bfloat_conversion_gadget(
	const detail::BFloatConversion &decoded) noexcept
{
	if (!host_has_bf16() || !host_has_afp())
		return uml_arm64_gadget_bfloat_convert_software;
	if (decoded.scalar)
		return uml_arm64_gadget_bfloat_convert_scalar_native;
	return decoded.upper ? uml_arm64_gadget_bfloat_convert_upper_native :
		uml_arm64_gadget_bfloat_convert_lower_native;
}

[[nodiscard]] Gadget bfloat_multiply_long_gadget(
	const detail::BFloatMultiplyLong &decoded) noexcept
{
	if (!host_has_bf16() || !host_has_afp())
		return uml_arm64_gadget_bfloat_multiply_long_software;
	static constexpr std::array vector{
		uml_arm64_gadget_bfloat_multiply_long_native_vector_b,
		uml_arm64_gadget_bfloat_multiply_long_native_vector_t};
	static constexpr std::array indexed{
		std::array<Gadget, 8>{
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_0,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_1,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_2,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_3,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_4,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_5,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_6,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_7},
		std::array<Gadget, 8>{
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_0,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_1,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_2,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_3,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_4,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_5,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_6,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_7}};
	return decoded.by_element ? indexed[decoded.top][decoded.index] :
		vector[decoded.top];
}

[[nodiscard]] Gadget bfloat_dot_gadget(const detail::BFloatDot &decoded) noexcept
{
	if (!host_has_bf16() || !host_has_ebf16())
		return uml_arm64_gadget_bfloat_dot_software;
	static constexpr std::array vector{
		uml_arm64_gadget_bfloat_dot_native_vector_2s,
		uml_arm64_gadget_bfloat_dot_native_vector_4s};
	static constexpr std::array indexed{
		std::array<Gadget, 4>{
			uml_arm64_gadget_bfloat_dot_native_indexed_2s_0,
			uml_arm64_gadget_bfloat_dot_native_indexed_2s_1,
			uml_arm64_gadget_bfloat_dot_native_indexed_2s_2,
			uml_arm64_gadget_bfloat_dot_native_indexed_2s_3},
		std::array<Gadget, 4>{
			uml_arm64_gadget_bfloat_dot_native_indexed_4s_0,
			uml_arm64_gadget_bfloat_dot_native_indexed_4s_1,
			uml_arm64_gadget_bfloat_dot_native_indexed_4s_2,
			uml_arm64_gadget_bfloat_dot_native_indexed_4s_3}};
	return decoded.by_element ? indexed[decoded.wide][decoded.index] :
		vector[decoded.wide];
}

[[nodiscard]] Gadget bfloat_matrix_gadget() noexcept
{
	return host_has_bf16() && host_has_ebf16() ?
		uml_arm64_gadget_bfloat_matrix_native :
		uml_arm64_gadget_bfloat_matrix_software;
}

[[nodiscard]] Gadget half_fused_long_gadget(
	const detail::HalfFusedLong &decoded) noexcept
{
	if (!host_has_fhm() || !host_has_afp())
		return uml_arm64_gadget_half_fused_long_software;
#define UML_HALF_FUSED_LONG_NATIVE(name) \
	uml_arm64_gadget_half_fused_long_native_##name
	static constexpr std::array vector{
		UML_HALF_FUSED_LONG_NATIVE(vector_add_lower_2s),
		UML_HALF_FUSED_LONG_NATIVE(vector_add_lower_4s),
		UML_HALF_FUSED_LONG_NATIVE(vector_add_upper_2s),
		UML_HALF_FUSED_LONG_NATIVE(vector_add_upper_4s),
		UML_HALF_FUSED_LONG_NATIVE(vector_sub_lower_2s),
		UML_HALF_FUSED_LONG_NATIVE(vector_sub_lower_4s),
		UML_HALF_FUSED_LONG_NATIVE(vector_sub_upper_2s),
		UML_HALF_FUSED_LONG_NATIVE(vector_sub_upper_4s)};
#define UML_HALF_FUSED_LONG_INDEXED(operation, half, shape) \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_0), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_1), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_2), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_3), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_4), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_5), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_6), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_7)
	static constexpr std::array indexed{
		UML_HALF_FUSED_LONG_INDEXED(add, lower, 2s),
		UML_HALF_FUSED_LONG_INDEXED(add, lower, 4s),
		UML_HALF_FUSED_LONG_INDEXED(add, upper, 2s),
		UML_HALF_FUSED_LONG_INDEXED(add, upper, 4s),
		UML_HALF_FUSED_LONG_INDEXED(sub, lower, 2s),
		UML_HALF_FUSED_LONG_INDEXED(sub, lower, 4s),
		UML_HALF_FUSED_LONG_INDEXED(sub, upper, 2s),
		UML_HALF_FUSED_LONG_INDEXED(sub, upper, 4s)};
#undef UML_HALF_FUSED_LONG_INDEXED
#undef UML_HALF_FUSED_LONG_NATIVE
	const unsigned operation = static_cast<unsigned>(decoded.subtract) * 4U +
		static_cast<unsigned>(decoded.upper) * 2U + decoded.wide;
	return decoded.by_element ?
		indexed[operation * 8U + decoded.index] :
		vector[operation];
}

[[nodiscard]] Gadget half_matrix_gadget(const detail::HalfMatrix &decoded) noexcept
{
	const bool native = host_has_afp() &&
		(decoded.widening ? host_has_f16f32mm() : host_has_f16mm());
	if (!native)
		return uml_arm64_gadget_half_matrix_software;
	static constexpr std::array gadgets{
		uml_arm64_gadget_half_matrix_native_non_widening,
		uml_arm64_gadget_half_matrix_native_widening};
	return gadgets[decoded.widening];
}

[[nodiscard]] Gadget half_dot_gadget(const detail::HalfDot &decoded) noexcept
{
	if (!host_has_f16f32dot() || !host_has_afp())
		return uml_arm64_gadget_half_dot_software;
	static constexpr std::array vector{
		uml_arm64_gadget_half_dot_native_vector_2s,
		uml_arm64_gadget_half_dot_native_vector_4s};
#define UML_HALF_DOT_INDEXED(shape) \
	uml_arm64_gadget_half_dot_native_indexed_##shape##_0, \
	uml_arm64_gadget_half_dot_native_indexed_##shape##_1, \
	uml_arm64_gadget_half_dot_native_indexed_##shape##_2, \
	uml_arm64_gadget_half_dot_native_indexed_##shape##_3
	static constexpr std::array indexed{
		UML_HALF_DOT_INDEXED(2s), UML_HALF_DOT_INDEXED(4s)};
#undef UML_HALF_DOT_INDEXED
	return decoded.by_element ?
		indexed[static_cast<unsigned>(decoded.wide) * 4U +
			decoded.index] :
		vector[decoded.wide];
}

[[nodiscard]] Gadget floating_reciprocal_gadget(
	const detail::FloatingReciprocal &decoded) noexcept
{
#define UML_RECIPROCAL_SCALAR(host, name) std::array<Gadget, 3>{ \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_h, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_s, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_d}
#define UML_RECIPROCAL_VECTOR(host, name) std::array<Gadget, 5>{ \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4h, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_8h, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2s, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4s, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2d}
#define UML_RECIPROCAL_SCALAR_PAIR(name) std::array{ \
	UML_RECIPROCAL_SCALAR(native, name), \
	UML_RECIPROCAL_SCALAR(checked, name)}
#define UML_RECIPROCAL_VECTOR_PAIR(name) std::array{ \
	UML_RECIPROCAL_VECTOR(native, name), \
	UML_RECIPROCAL_VECTOR(checked, name)}
	static constexpr std::array scalar{
		UML_RECIPROCAL_SCALAR_PAIR(frecpe),
		UML_RECIPROCAL_SCALAR_PAIR(frecps),
		UML_RECIPROCAL_SCALAR_PAIR(frecpx),
		UML_RECIPROCAL_SCALAR_PAIR(frsqrte),
		UML_RECIPROCAL_SCALAR_PAIR(frsqrts)};
	static constexpr std::array vector{
		UML_RECIPROCAL_VECTOR_PAIR(frecpe),
		UML_RECIPROCAL_VECTOR_PAIR(frecps),
		std::array{std::array<Gadget, 5>{},
			std::array<Gadget, 5>{}},
		UML_RECIPROCAL_VECTOR_PAIR(frsqrte),
		UML_RECIPROCAL_VECTOR_PAIR(frsqrts)};
#undef UML_RECIPROCAL_VECTOR_PAIR
#undef UML_RECIPROCAL_SCALAR_PAIR
#undef UML_RECIPROCAL_VECTOR
#undef UML_RECIPROCAL_SCALAR
	const unsigned operation = static_cast<unsigned>(decoded.operation);
	const unsigned host_variant = host_has_afp() ? 0U : 1U;
	if (decoded.scalar) {
		const unsigned shape = decoded.width == 16U ? 0U :
			decoded.width == 32U ? 1U : 2U;
		return scalar[operation][host_variant][shape];
	}
	const unsigned shape = decoded.width == 16U ?
		(decoded.vector_bytes == 16U ? 1U : 0U) :
		decoded.width == 32U ?
		(decoded.vector_bytes == 16U ? 3U : 2U) : 4U;
	return vector[operation][host_variant][shape];
}

[[nodiscard]] Gadget floating_complex_gadget(
	const detail::FloatingComplex &decoded) noexcept
{
	const unsigned shape = decoded.width == 16U ?
		(decoded.vector_bytes == 16U ? 1U : 0U) :
		decoded.width == 32U ?
		(decoded.vector_bytes == 16U ? 3U : 2U) : 4U;
#define UML_COMPLEX_ADD(shape) std::array<Gadget, 2>{ \
	uml_arm64_gadget_floating_complex_fcadd_##shape##_90, \
	uml_arm64_gadget_floating_complex_fcadd_##shape##_270}
	static constexpr std::array add{
		UML_COMPLEX_ADD(4h), UML_COMPLEX_ADD(8h),
		UML_COMPLEX_ADD(2s), UML_COMPLEX_ADD(4s),
		UML_COMPLEX_ADD(2d)};
#undef UML_COMPLEX_ADD
#define UML_COMPLEX_ROTATIONS(name) std::array<Gadget, 4>{ \
	uml_arm64_gadget_floating_complex_##name##_0, \
	uml_arm64_gadget_floating_complex_##name##_90, \
	uml_arm64_gadget_floating_complex_##name##_180, \
	uml_arm64_gadget_floating_complex_##name##_270}
	static constexpr std::array multiply{
		UML_COMPLEX_ROTATIONS(fcmla_4h),
		UML_COMPLEX_ROTATIONS(fcmla_8h),
		UML_COMPLEX_ROTATIONS(fcmla_2s),
		UML_COMPLEX_ROTATIONS(fcmla_4s),
		UML_COMPLEX_ROTATIONS(fcmla_2d)};
	static constexpr std::array half64{
		UML_COMPLEX_ROTATIONS(fcmla_element_4h_0),
		UML_COMPLEX_ROTATIONS(fcmla_element_4h_1)};
	static constexpr std::array half128{
		UML_COMPLEX_ROTATIONS(fcmla_element_8h_0),
		UML_COMPLEX_ROTATIONS(fcmla_element_8h_1),
		UML_COMPLEX_ROTATIONS(fcmla_element_8h_2),
		UML_COMPLEX_ROTATIONS(fcmla_element_8h_3)};
	static constexpr std::array single128{
		UML_COMPLEX_ROTATIONS(fcmla_element_4s_0),
		UML_COMPLEX_ROTATIONS(fcmla_element_4s_1)};
#undef UML_COMPLEX_ROTATIONS
	if (decoded.operation == detail::FloatingComplexOperation::add)
		return add[shape][decoded.rotation == 3U ? 1U : 0U];
	if (!decoded.by_element)
		return multiply[shape][decoded.rotation];
	if (decoded.width == 32U)
		return single128[decoded.index][decoded.rotation];
	return (decoded.vector_bytes == 16U ? half128[decoded.index] :
		half64[decoded.index])[decoded.rotation];
}

[[nodiscard]] constexpr std::uintptr_t floating_complex_operand(
	std::uint32_t instruction,
	const detail::FloatingComplex &decoded) noexcept
{
	const auto vector_offset = [](unsigned index) {
		return offsetof(CpuState, vectors) +
			index * sizeof(VectorRegister);
	};
	static_assert(offsetof(CpuState, vectors) +
		31U * sizeof(VectorRegister) <= 0xffffU);
	return vector_offset(instruction & 31U) |
		(static_cast<std::uintptr_t>(vector_offset(
			(instruction >> 5) & 31U)) << 16U) |
		(static_cast<std::uintptr_t>(vector_offset(
			decoded.second_register)) << 32U);
}

[[nodiscard]] constexpr std::uint64_t expand_vfp_immediate(
	std::uint8_t immediate, unsigned bits) noexcept
{
	const unsigned exponent_bits = bits == 16U ? 5U : bits == 32U ? 8U : 11U;
	const unsigned fraction_bits = bits - exponent_bits - 1U;
	const auto sign = static_cast<std::uint64_t>(immediate >> 7) << (bits - 1U);
	const auto control = static_cast<std::uint64_t>((immediate >> 6) & 1U);
	const auto exponent_top = (control ^ 1U) << (bits - 2U);
	const auto exponent_repeat = control == 0U ? 0U :
		((std::uint64_t{1} << (exponent_bits - 3U)) - 1U) <<
			(fraction_bits + 2U);
	const auto payload = static_cast<std::uint64_t>(immediate & 0x3fU) <<
		(fraction_bits - 4U);
	return sign | exponent_top | exponent_repeat | payload;
}

[[nodiscard]] Gadget simd_floating_sign_gadget(bool negate, bool half,
	bool double_precision, bool q) noexcept
{
	if (half) {
		if (negate)
			return q ? uml_arm64_gadget_simd_floating_fneg_8h :
				uml_arm64_gadget_simd_floating_fneg_4h;
		return q ? uml_arm64_gadget_simd_floating_fabs_8h :
			uml_arm64_gadget_simd_floating_fabs_4h;
	}
	if (double_precision)
		return q ? (negate ? uml_arm64_gadget_simd_floating_fneg_2d :
			uml_arm64_gadget_simd_floating_fabs_2d) : nullptr;
	if (negate)
		return q ? uml_arm64_gadget_simd_floating_fneg_4s :
			uml_arm64_gadget_simd_floating_fneg_2s;
	return q ? uml_arm64_gadget_simd_floating_fabs_4s :
		uml_arm64_gadget_simd_floating_fabs_2s;
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

[[nodiscard]] Gadget fused_floating_arithmetic_gadget(
	const detail::FusedFloatingArithmetic &decoded) noexcept
{
	if (decoded.scalar) {
#define UML_SCALAR_FUSED_WIDTH(name) \
		(decoded.width == 16U ? uml_arm64_gadget_floating_##name##_half : \
		 decoded.width == 32U ? uml_arm64_gadget_floating_##name##_single : \
		 uml_arm64_gadget_floating_##name##_double)
		if (!decoded.negate && !decoded.subtract)
			return UML_SCALAR_FUSED_WIDTH(madd);
		if (!decoded.negate)
			return UML_SCALAR_FUSED_WIDTH(msub);
		if (!decoded.subtract)
			return UML_SCALAR_FUSED_WIDTH(nmadd);
		return UML_SCALAR_FUSED_WIDTH(nmsub);
#undef UML_SCALAR_FUSED_WIDTH
	}
	const unsigned shape = decoded.width == 16U ?
		(decoded.vector_bytes == 16U ? 1U : 0U) :
		decoded.width == 32U ?
		(decoded.vector_bytes == 16U ? 3U : 2U) : 4U;
#define UML_SIMD_FUSED_ARRAY(name) std::array<Gadget, 5>{ \
	uml_arm64_gadget_simd_fused_##name##_4h, \
	uml_arm64_gadget_simd_fused_##name##_8h, \
	uml_arm64_gadget_simd_fused_##name##_2s, \
	uml_arm64_gadget_simd_fused_##name##_4s, \
	uml_arm64_gadget_simd_fused_##name##_2d}
	static constexpr auto fmla = UML_SIMD_FUSED_ARRAY(fmla);
	static constexpr auto fmls = UML_SIMD_FUSED_ARRAY(fmls);
#undef UML_SIMD_FUSED_ARRAY
	return (decoded.subtract ? fmls : fmla)[shape];
}

[[nodiscard]] Gadget simd_bitwise_gadget(unsigned operation, bool q) noexcept
{
	const unsigned shape = q ? 1U : 0U;
#define UML_SIMD_BITWISE(name) std::array<Gadget, 2>{ \
	uml_arm64_gadget_##name##_8b, uml_arm64_gadget_##name##_16b}
	static constexpr auto bit_and = UML_SIMD_BITWISE(simd_and);
	static constexpr auto bit_clear = UML_SIMD_BITWISE(simd_bic);
	static constexpr auto bit_or = UML_SIMD_BITWISE(simd_orr);
	static constexpr auto or_not = UML_SIMD_BITWISE(simd_orn);
	static constexpr auto bit_xor = UML_SIMD_BITWISE(simd_eor);
	static constexpr auto select = UML_SIMD_BITWISE(simd_bsl);
	static constexpr auto insert_true = UML_SIMD_BITWISE(simd_bit);
	static constexpr auto insert_false = UML_SIMD_BITWISE(simd_bif);
#undef UML_SIMD_BITWISE
	switch (operation) {
	case 0: return bit_and[shape];
	case 1: return bit_clear[shape];
	case 2: return bit_or[shape];
	case 3: return or_not[shape];
	case 4: return bit_xor[shape];
	case 5: return select[shape];
	case 6: return insert_true[shape];
	default: return insert_false[shape];
	}
}

[[nodiscard]] Gadget simd_move_gadget(bool q) noexcept
{
	return q ? uml_arm64_gadget_simd_mov_16b :
		uml_arm64_gadget_simd_mov_8b;
}

[[nodiscard]] Gadget simd_modified_immediate_gadget(
	unsigned action, bool q) noexcept
{
	const unsigned shape = q ? 1U : 0U;
	static constexpr std::array replace{
		uml_arm64_gadget_simd_immediate_replace_8b,
		uml_arm64_gadget_simd_immediate_replace_16b};
	static constexpr std::array bit_or{
		uml_arm64_gadget_simd_immediate_orr_8b,
		uml_arm64_gadget_simd_immediate_orr_16b};
	static constexpr std::array bit_clear{
		uml_arm64_gadget_simd_immediate_bic_8b,
		uml_arm64_gadget_simd_immediate_bic_16b};
	switch (action) {
	case 0: return replace[shape];
	case 1: return bit_or[shape];
	default: return bit_clear[shape];
	}
}

[[nodiscard]] Gadget simd_duplicate_element_gadget(
	bool scalar, bool q, unsigned element_shift) noexcept
{
	static constexpr std::array<Gadget, 4> scalar_gadgets{
		uml_arm64_gadget_simd_duplicate_element_b,
		uml_arm64_gadget_simd_duplicate_element_h,
		uml_arm64_gadget_simd_duplicate_element_s,
		uml_arm64_gadget_simd_duplicate_element_d};
	static constexpr std::array<Gadget, 4> narrow_gadgets{
		uml_arm64_gadget_simd_duplicate_element_8b,
		uml_arm64_gadget_simd_duplicate_element_4h,
		uml_arm64_gadget_simd_duplicate_element_2s,
		nullptr};
	static constexpr std::array<Gadget, 4> wide_gadgets{
		uml_arm64_gadget_simd_duplicate_element_16b,
		uml_arm64_gadget_simd_duplicate_element_8h,
		uml_arm64_gadget_simd_duplicate_element_4s,
		uml_arm64_gadget_simd_duplicate_element_2d};
	if (element_shift > 3U)
		return nullptr;
	return scalar ? scalar_gadgets[element_shift] :
		(q ? wide_gadgets[element_shift] : narrow_gadgets[element_shift]);
}

[[nodiscard]] Gadget simd_duplicate_general_gadget(
	bool q, unsigned element_shift, bool zero_source) noexcept
{
	if (element_shift > 3U || (!q && element_shift == 3U))
		return nullptr;
	if (zero_source)
		return q ? uml_arm64_gadget_simd_duplicate_general_zero_16b :
			uml_arm64_gadget_simd_duplicate_general_zero_8b;
	static constexpr std::array<Gadget, 4> narrow_gadgets{
		uml_arm64_gadget_simd_duplicate_general_8b,
		uml_arm64_gadget_simd_duplicate_general_4h,
		uml_arm64_gadget_simd_duplicate_general_2s,
		nullptr};
	static constexpr std::array<Gadget, 4> wide_gadgets{
		uml_arm64_gadget_simd_duplicate_general_16b,
		uml_arm64_gadget_simd_duplicate_general_8h,
		uml_arm64_gadget_simd_duplicate_general_4s,
		uml_arm64_gadget_simd_duplicate_general_2d};
	return (q ? wide_gadgets : narrow_gadgets)[element_shift];
}

[[nodiscard]] Gadget simd_insert_general_gadget(
	unsigned element_shift, bool zero_source) noexcept
{
	if (element_shift > 3U)
		return nullptr;
	static constexpr std::array<Gadget, 4> regular{
		uml_arm64_gadget_simd_insert_general_b,
		uml_arm64_gadget_simd_insert_general_h,
		uml_arm64_gadget_simd_insert_general_s,
		uml_arm64_gadget_simd_insert_general_d};
	static constexpr std::array<Gadget, 4> zero{
		uml_arm64_gadget_simd_insert_general_zero_b,
		uml_arm64_gadget_simd_insert_general_zero_h,
		uml_arm64_gadget_simd_insert_general_zero_s,
		uml_arm64_gadget_simd_insert_general_zero_d};
	return (zero_source ? zero : regular)[element_shift];
}

[[nodiscard]] Gadget simd_insert_element_gadget(
	unsigned element_shift) noexcept
{
	static constexpr std::array<Gadget, 4> gadgets{
		uml_arm64_gadget_simd_insert_element_b,
		uml_arm64_gadget_simd_insert_element_h,
		uml_arm64_gadget_simd_insert_element_s,
		uml_arm64_gadget_simd_insert_element_d};
	return element_shift < gadgets.size() ? gadgets[element_shift] : nullptr;
}

[[nodiscard]] Gadget simd_lane_to_gpr_gadget(bool signed_move,
	bool is_64_bit, unsigned element_shift, bool discard) noexcept
{
	Gadget gadget = nullptr;
	if (signed_move) {
		if (!is_64_bit && element_shift == 0U)
			gadget = uml_arm64_gadget_simd_lane_to_gpr_smov_w_b;
		else if (!is_64_bit && element_shift == 1U)
			gadget = uml_arm64_gadget_simd_lane_to_gpr_smov_w_h;
		else if (is_64_bit && element_shift == 0U)
			gadget = uml_arm64_gadget_simd_lane_to_gpr_smov_x_b;
		else if (is_64_bit && element_shift == 1U)
			gadget = uml_arm64_gadget_simd_lane_to_gpr_smov_x_h;
		else if (is_64_bit && element_shift == 2U)
			gadget = uml_arm64_gadget_simd_lane_to_gpr_smov_x_s;
	} else if (!is_64_bit && element_shift == 0U) {
		gadget = uml_arm64_gadget_simd_lane_to_gpr_umov_w_b;
	} else if (!is_64_bit && element_shift == 1U) {
		gadget = uml_arm64_gadget_simd_lane_to_gpr_umov_w_h;
	} else if (!is_64_bit && element_shift == 2U) {
		gadget = uml_arm64_gadget_simd_lane_to_gpr_umov_w_s;
	} else if (is_64_bit && element_shift == 3U) {
		gadget = uml_arm64_gadget_simd_lane_to_gpr_umov_x_d;
	}
	return gadget != nullptr && discard ?
		uml_arm64_gadget_simd_lane_to_gpr_discard : gadget;
}

struct SimdImmediateShift final {
	unsigned lane_bits;
	unsigned shift;
	unsigned shape;
};

[[nodiscard]] std::optional<SimdImmediateShift> decode_simd_immediate_shift(
	std::uint32_t instruction, bool left) noexcept
{
	const unsigned encoded = (instruction >> 16) & 0x7fU;
	if (encoded < 8U)
		return std::nullopt;
	const unsigned lane_bits = std::bit_floor(encoded);
	if (lane_bits > 64U)
		return std::nullopt;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const bool q = (instruction & 0x4000'0000U) != 0;
	if ((scalar && lane_bits != 64U) ||
	    (!scalar && !q && lane_bits == 64U))
		return std::nullopt;
	const unsigned shift = left ? encoded - lane_bits :
		2U * lane_bits - encoded;
	if ((left && shift >= lane_bits) ||
	    (!left && (shift == 0U || shift > lane_bits)))
		return std::nullopt;
	const unsigned size = std::countr_zero(lane_bits / 8U);
	return SimdImmediateShift{lane_bits, shift,
		scalar ? 7U : q ? size + 3U : size};
}

[[nodiscard]] Gadget simd_shift_right_immediate_gadget(
	unsigned opcode, bool alternate, unsigned shape) noexcept
{
#define UML_SIMD_SHIFT_ARRAY(name) std::array<Gadget, 8>{ \
	uml_arm64_gadget_simd_shift_right_##name##_8b, \
	uml_arm64_gadget_simd_shift_right_##name##_4h, \
	uml_arm64_gadget_simd_shift_right_##name##_2s, \
	uml_arm64_gadget_simd_shift_right_##name##_16b, \
	uml_arm64_gadget_simd_shift_right_##name##_8h, \
	uml_arm64_gadget_simd_shift_right_##name##_4s, \
	uml_arm64_gadget_simd_shift_right_##name##_2d, \
	uml_arm64_gadget_simd_shift_right_##name##_d}
	static constexpr auto sshr = UML_SIMD_SHIFT_ARRAY(sshr);
	static constexpr auto ushr = UML_SIMD_SHIFT_ARRAY(ushr);
	static constexpr auto ssra = UML_SIMD_SHIFT_ARRAY(ssra);
	static constexpr auto usra = UML_SIMD_SHIFT_ARRAY(usra);
	static constexpr auto srshr = UML_SIMD_SHIFT_ARRAY(srshr);
	static constexpr auto urshr = UML_SIMD_SHIFT_ARRAY(urshr);
	static constexpr auto srsra = UML_SIMD_SHIFT_ARRAY(srsra);
	static constexpr auto ursra = UML_SIMD_SHIFT_ARRAY(ursra);
	static constexpr auto sri = UML_SIMD_SHIFT_ARRAY(sri);
#undef UML_SIMD_SHIFT_ARRAY
	if (shape >= 8U)
		return nullptr;
	switch (opcode) {
	case 0: return (alternate ? ushr : sshr)[shape];
	case 2: return (alternate ? usra : ssra)[shape];
	case 4: return (alternate ? urshr : srshr)[shape];
	case 6: return (alternate ? ursra : srsra)[shape];
	case 8: return alternate ? sri[shape] : nullptr;
	default: return nullptr;
	}
}

[[nodiscard]] Gadget simd_shift_left_immediate_gadget(
	bool insert, unsigned shape) noexcept
{
#define UML_SIMD_SHIFT_ARRAY(name) std::array<Gadget, 8>{ \
	uml_arm64_gadget_simd_shift_left_##name##_8b, \
	uml_arm64_gadget_simd_shift_left_##name##_4h, \
	uml_arm64_gadget_simd_shift_left_##name##_2s, \
	uml_arm64_gadget_simd_shift_left_##name##_16b, \
	uml_arm64_gadget_simd_shift_left_##name##_8h, \
	uml_arm64_gadget_simd_shift_left_##name##_4s, \
	uml_arm64_gadget_simd_shift_left_##name##_2d, \
	uml_arm64_gadget_simd_shift_left_##name##_d}
	static constexpr auto shl = UML_SIMD_SHIFT_ARRAY(shl);
	static constexpr auto sli = UML_SIMD_SHIFT_ARRAY(sli);
#undef UML_SIMD_SHIFT_ARRAY
	return shape < 8U ? (insert ? sli : shl)[shape] : nullptr;
}

[[nodiscard]] std::optional<SimdImmediateShift>
decode_simd_saturating_shift_left(std::uint32_t instruction) noexcept
{
	const unsigned encoded = (instruction >> 16) & 0x7fU;
	if (encoded < 8U)
		return std::nullopt;
	const unsigned lane_bits = std::bit_floor(encoded);
	if (lane_bits > 64U)
		return std::nullopt;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const bool q = (instruction & 0x4000'0000U) != 0;
	if (!scalar && !q && lane_bits == 64U)
		return std::nullopt;
	const unsigned shift = encoded - lane_bits;
	const unsigned size = std::countr_zero(lane_bits / 8U);
	return SimdImmediateShift{lane_bits, shift,
		scalar ? size + 7U : q ? size + 3U : size};
}

enum class SimdSaturatingShiftLeft {
	signed_to_signed,
	signed_to_unsigned,
	unsigned_to_unsigned,
};

[[nodiscard]] Gadget simd_saturating_shift_left_gadget(
	SimdSaturatingShiftLeft operation, unsigned shape, unsigned shift) noexcept
{
#define UML_SIMD_SAT_SHIFT_ARRAY(name) std::array<Gadget, 11>{ \
	uml_arm64_gadget_simd_shift_left_##name##_8b, \
	uml_arm64_gadget_simd_shift_left_##name##_4h, \
	uml_arm64_gadget_simd_shift_left_##name##_2s, \
	uml_arm64_gadget_simd_shift_left_##name##_16b, \
	uml_arm64_gadget_simd_shift_left_##name##_8h, \
	uml_arm64_gadget_simd_shift_left_##name##_4s, \
	uml_arm64_gadget_simd_shift_left_##name##_2d, \
	uml_arm64_gadget_simd_shift_left_##name##_b, \
	uml_arm64_gadget_simd_shift_left_##name##_h, \
	uml_arm64_gadget_simd_shift_left_##name##_s, \
	uml_arm64_gadget_simd_shift_left_##name##_d}
	static constexpr auto signed_shift = UML_SIMD_SAT_SHIFT_ARRAY(sqshl);
	static constexpr auto unsigned_shift = UML_SIMD_SAT_SHIFT_ARRAY(uqshl);
#undef UML_SIMD_SAT_SHIFT_ARRAY
	if (shape >= 11U)
		return nullptr;
	switch (operation) {
	case SimdSaturatingShiftLeft::signed_to_signed:
		return signed_shift[shape];
	case SimdSaturatingShiftLeft::unsigned_to_unsigned:
		return unsigned_shift[shape];
	case SimdSaturatingShiftLeft::signed_to_unsigned:
		break;
	}
#define UML_SIMD_SQSHLU_ITEM(shape_name, amount) \
	uml_arm64_gadget_simd_shift_left_sqshlu_##shape_name##_##amount,
#define UML_SIMD_SQSHLU_ARRAY(bits, shape_name) \
	static constexpr std::array<Gadget, bits> sqshlu_##shape_name{ \
		UML_SIMD_FOR_EACH_SHIFT_##bits(UML_SIMD_SQSHLU_ITEM, shape_name)}
	UML_SIMD_SQSHLU_ARRAY(8, 8b);
	UML_SIMD_SQSHLU_ARRAY(16, 4h);
	UML_SIMD_SQSHLU_ARRAY(32, 2s);
	UML_SIMD_SQSHLU_ARRAY(8, 16b);
	UML_SIMD_SQSHLU_ARRAY(16, 8h);
	UML_SIMD_SQSHLU_ARRAY(32, 4s);
	UML_SIMD_SQSHLU_ARRAY(64, 2d);
	UML_SIMD_SQSHLU_ARRAY(8, b);
	UML_SIMD_SQSHLU_ARRAY(16, h);
	UML_SIMD_SQSHLU_ARRAY(32, s);
	UML_SIMD_SQSHLU_ARRAY(64, d);
#undef UML_SIMD_SQSHLU_ARRAY
#undef UML_SIMD_SQSHLU_ITEM
	switch (shape) {
	case 0: return sqshlu_8b[shift];
	case 1: return sqshlu_4h[shift];
	case 2: return sqshlu_2s[shift];
	case 3: return sqshlu_16b[shift];
	case 4: return sqshlu_8h[shift];
	case 5: return sqshlu_4s[shift];
	case 6: return sqshlu_2d[shift];
	case 7: return sqshlu_b[shift];
	case 8: return sqshlu_h[shift];
	case 9: return sqshlu_s[shift];
	case 10: return sqshlu_d[shift];
	default: return nullptr;
	}
}

#undef UML_SIMD_FOR_EACH_SHIFT_64
#undef UML_SIMD_FOR_EACH_SHIFT_32
#undef UML_SIMD_FOR_EACH_SHIFT_16
#undef UML_SIMD_FOR_EACH_SHIFT_8

[[nodiscard]] Gadget simd_shift_narrow_gadget(
	unsigned source_bits, bool high, bool rounding) noexcept
{
	static constexpr std::array<Gadget, 12> gadgets{
		uml_arm64_gadget_simd_shrn_8h_to_8b,
		uml_arm64_gadget_simd_shrn_4s_to_4h,
		uml_arm64_gadget_simd_shrn_2d_to_2s,
		uml_arm64_gadget_simd_shrn2_8h_to_8b,
		uml_arm64_gadget_simd_shrn2_4s_to_4h,
		uml_arm64_gadget_simd_shrn2_2d_to_2s,
		uml_arm64_gadget_simd_rshrn_8h_to_8b,
		uml_arm64_gadget_simd_rshrn_4s_to_4h,
		uml_arm64_gadget_simd_rshrn_2d_to_2s,
		uml_arm64_gadget_simd_rshrn2_8h_to_8b,
		uml_arm64_gadget_simd_rshrn2_4s_to_4h,
		uml_arm64_gadget_simd_rshrn2_2d_to_2s};
	if (source_bits != 16U && source_bits != 32U && source_bits != 64U)
		return nullptr;
	const unsigned source = std::countr_zero(source_bits) - 4U;
	return gadgets[source + (high ? 3U : 0U) + (rounding ? 6U : 0U)];
}

[[nodiscard]] Gadget simd_saturating_narrow_gadget(unsigned opcode,
	bool alternate, unsigned source_bits, bool scalar, bool high) noexcept
{
	if (source_bits != 16U && source_bits != 32U && source_bits != 64U)
		return nullptr;
	if ((opcode == 16U || opcode == 17U) && !alternate)
		return nullptr;
	const unsigned shape = (std::countr_zero(source_bits) - 4U) * 3U +
		(scalar ? 2U : high ? 1U : 0U);
#define UML_SIMD_SAT_NARROW_ARRAY(name) std::array<Gadget, 9>{ \
	uml_arm64_gadget_simd_##name##_h_b_low, \
	uml_arm64_gadget_simd_##name##_h_b_high, \
	uml_arm64_gadget_simd_##name##_h_b_scalar, \
	uml_arm64_gadget_simd_##name##_s_h_low, \
	uml_arm64_gadget_simd_##name##_s_h_high, \
	uml_arm64_gadget_simd_##name##_s_h_scalar, \
	uml_arm64_gadget_simd_##name##_d_s_low, \
	uml_arm64_gadget_simd_##name##_d_s_high, \
	uml_arm64_gadget_simd_##name##_d_s_scalar}
	static constexpr auto sqshrn = UML_SIMD_SAT_NARROW_ARRAY(sqshrn);
	static constexpr auto sqrshrn = UML_SIMD_SAT_NARROW_ARRAY(sqrshrn);
	static constexpr auto sqshrun = UML_SIMD_SAT_NARROW_ARRAY(sqshrun);
	static constexpr auto sqrshrun = UML_SIMD_SAT_NARROW_ARRAY(sqrshrun);
	static constexpr auto uqshrn = UML_SIMD_SAT_NARROW_ARRAY(uqshrn);
	static constexpr auto uqrshrn = UML_SIMD_SAT_NARROW_ARRAY(uqrshrn);
#undef UML_SIMD_SAT_NARROW_ARRAY
	switch (opcode) {
	case 16: return sqshrun[shape];
	case 17: return sqrshrun[shape];
	case 18: return (alternate ? uqshrn : sqshrn)[shape];
	case 19: return (alternate ? uqrshrn : sqrshrn)[shape];
	default: return nullptr;
	}
}

[[nodiscard]] Gadget simd_shift_left_long_gadget(
	unsigned source_bits, bool high, bool unsigned_source) noexcept
{
	if (source_bits != 8U && source_bits != 16U && source_bits != 32U)
		return nullptr;
	const unsigned shape = std::countr_zero(source_bits) - 3U;
	static constexpr std::array<Gadget, 3> sshll{
		uml_arm64_gadget_simd_sshll_8h_8b,
		uml_arm64_gadget_simd_sshll_4s_4h,
		uml_arm64_gadget_simd_sshll_2d_2s};
	static constexpr std::array<Gadget, 3> sshll2{
		uml_arm64_gadget_simd_sshll2_8h_8b,
		uml_arm64_gadget_simd_sshll2_4s_4h,
		uml_arm64_gadget_simd_sshll2_2d_2s};
	static constexpr std::array<Gadget, 3> ushll{
		uml_arm64_gadget_simd_ushll_8h_8b,
		uml_arm64_gadget_simd_ushll_4s_4h,
		uml_arm64_gadget_simd_ushll_2d_2s};
	static constexpr std::array<Gadget, 3> ushll2{
		uml_arm64_gadget_simd_ushll2_8h_8b,
		uml_arm64_gadget_simd_ushll2_4s_4h,
		uml_arm64_gadget_simd_ushll2_2d_2s};
	return unsigned_source ? (high ? ushll2 : ushll)[shape] :
		(high ? sshll2 : sshll)[shape];
}

#define UML_SIMD_INDEXED_ITEMS_4(name, shape) \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_0, \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_1, \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_2, \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_3
#define UML_SIMD_INDEXED_ITEMS_8(name, shape) \
	UML_SIMD_INDEXED_ITEMS_4(name, shape), \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_4, \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_5, \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_6, \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_7
#define UML_SIMD_INDEXED_ARRAY_4(name, shape) \
	std::array<Gadget, 8>{UML_SIMD_INDEXED_ITEMS_4(name, shape), \
		nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_INDEXED_ARRAY_2(name, shape) \
	std::array<Gadget, 8>{ \
		uml_arm64_gadget_simd_indexed_##name##_##shape##_0, \
		uml_arm64_gadget_simd_indexed_##name##_##shape##_1, \
		nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_INDEXED_ARRAY_8(name, shape) \
	std::array<Gadget, 8>{UML_SIMD_INDEXED_ITEMS_8(name, shape)}

[[nodiscard]] Gadget simd_indexed_same_gadget(unsigned operation,
	unsigned size, bool q, unsigned index) noexcept
{
#define UML_SIMD_INDEXED_SAME_FAMILY(name) \
	std::array<std::array<Gadget, 8>, 4>{ \
		UML_SIMD_INDEXED_ARRAY_8(name, 4h), \
		UML_SIMD_INDEXED_ARRAY_8(name, 8h), \
		UML_SIMD_INDEXED_ARRAY_4(name, 2s), \
		UML_SIMD_INDEXED_ARRAY_4(name, 4s)}
	static constexpr std::array families{
		UML_SIMD_INDEXED_SAME_FAMILY(mla),
		UML_SIMD_INDEXED_SAME_FAMILY(mls),
		UML_SIMD_INDEXED_SAME_FAMILY(mul)};
#undef UML_SIMD_INDEXED_SAME_FAMILY
	const unsigned family = operation / 4U;
	const unsigned shape = (size == 2U ? 2U : 0U) + (q ? 1U : 0U);
	return families[family][shape][index];
}

[[nodiscard]] Gadget simd_indexed_long_gadget(unsigned operation,
	bool unsigned_group, unsigned size, bool high, unsigned index) noexcept
{
#define UML_SIMD_INDEXED_LONG_FAMILY(name) \
	std::array<std::array<Gadget, 8>, 4>{ \
		UML_SIMD_INDEXED_ARRAY_8(name, h_low), \
		UML_SIMD_INDEXED_ARRAY_8(name, h_high), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_low), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_high)}
	static constexpr std::array families{
		UML_SIMD_INDEXED_LONG_FAMILY(smlal),
		UML_SIMD_INDEXED_LONG_FAMILY(smlsl),
		UML_SIMD_INDEXED_LONG_FAMILY(smull),
		UML_SIMD_INDEXED_LONG_FAMILY(umlal),
		UML_SIMD_INDEXED_LONG_FAMILY(umlsl),
		UML_SIMD_INDEXED_LONG_FAMILY(umull)};
#undef UML_SIMD_INDEXED_LONG_FAMILY
	const unsigned family = (unsigned_group ? 3U : 0U) +
		(operation - 2U) / 4U;
	const unsigned shape = (size == 2U ? 2U : 0U) + (high ? 1U : 0U);
	return families[family][shape][index];
}

[[nodiscard]] Gadget simd_indexed_saturating_long_gadget(unsigned operation,
	unsigned size, bool scalar, bool high, unsigned index) noexcept
{
#define UML_SIMD_INDEXED_SATURATING_FAMILY(name) \
	std::array<std::array<Gadget, 8>, 6>{ \
		UML_SIMD_INDEXED_ARRAY_8(name, h_low), \
		UML_SIMD_INDEXED_ARRAY_8(name, h_high), \
		UML_SIMD_INDEXED_ARRAY_8(name, h_scalar), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_low), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_high), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_scalar)}
	static constexpr std::array families{
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqdmlal),
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqdmlsl),
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqdmull)};
#undef UML_SIMD_INDEXED_SATURATING_FAMILY
	const unsigned family = (operation - 3U) / 4U;
	const unsigned shape = (size == 2U ? 3U : 0U) +
		(scalar ? 2U : high ? 1U : 0U);
	return families[family][shape][index];
}

[[nodiscard]] Gadget simd_indexed_saturating_high_gadget(unsigned operation,
	unsigned size, bool scalar, bool wide, unsigned index) noexcept
{
#define UML_SIMD_INDEXED_SATURATING_FAMILY(name) \
	std::array<std::array<Gadget, 8>, 6>{ \
		UML_SIMD_INDEXED_ARRAY_8(name, h_low), \
		UML_SIMD_INDEXED_ARRAY_8(name, h_high), \
		UML_SIMD_INDEXED_ARRAY_8(name, h_scalar), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_low), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_high), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_scalar)}
	static constexpr std::array families{
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqdmulh),
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqrdmulh)};
#undef UML_SIMD_INDEXED_SATURATING_FAMILY
	const unsigned shape = (size == 2U ? 3U : 0U) +
		(scalar ? 2U : wide ? 1U : 0U);
	return families[operation - 12U][shape][index];
}

[[nodiscard]] Gadget simd_indexed_saturating_accumulate_high_gadget(
	unsigned operation, unsigned size, bool scalar, bool wide,
	unsigned index) noexcept
{
#define UML_SIMD_INDEXED_SATURATING_FAMILY(name) \
	std::array<std::array<Gadget, 8>, 6>{ \
		UML_SIMD_INDEXED_ARRAY_8(name, h_low), \
		UML_SIMD_INDEXED_ARRAY_8(name, h_high), \
		UML_SIMD_INDEXED_ARRAY_8(name, h_scalar), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_low), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_high), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_scalar)}
	static constexpr std::array families{
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqrdmlah),
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqrdmlsh)};
#undef UML_SIMD_INDEXED_SATURATING_FAMILY
	const unsigned shape = (size == 2U ? 3U : 0U) +
		(scalar ? 2U : wide ? 1U : 0U);
	return families[(operation - 13U) / 2U][shape][index];
}

[[nodiscard]] Gadget simd_indexed_integer_gadget(unsigned operation,
	bool unsigned_group, bool scalar, bool q, unsigned size,
	unsigned index) noexcept
{
	if (size != 1U && size != 2U)
		return nullptr;
	switch (operation) {
	case 0: case 4:
		return unsigned_group && !scalar ? simd_indexed_same_gadget(
			operation, size, q, index) : nullptr;
	case 8:
		return !unsigned_group && !scalar ? simd_indexed_same_gadget(
			operation, size, q, index) : nullptr;
	case 2: case 6: case 10:
		return !scalar ? simd_indexed_long_gadget(operation, unsigned_group,
			size, q, index) : nullptr;
	case 3: case 7: case 11:
		return !unsigned_group ? simd_indexed_saturating_long_gadget(
			operation, size, scalar, q, index) : nullptr;
	case 12:
		return !unsigned_group ? simd_indexed_saturating_high_gadget(
			operation, size, scalar, q, index) : nullptr;
	case 13:
		return unsigned_group ?
			simd_indexed_saturating_accumulate_high_gadget(
				operation, size, scalar, q, index) :
			simd_indexed_saturating_high_gadget(
				operation, size, scalar, q, index);
	case 15:
		return unsigned_group ?
			simd_indexed_saturating_accumulate_high_gadget(
				operation, size, scalar, q, index) : nullptr;
	default:
		return nullptr;
	}
}

[[nodiscard]] Gadget simd_indexed_floating_gadget(unsigned operation,
	bool extended, bool scalar, bool q, unsigned size, unsigned index) noexcept
{
#define UML_SIMD_INDEXED_FLOATING_FAMILY(name) \
	std::array<std::array<Gadget, 8>, 8>{ \
		UML_SIMD_INDEXED_ARRAY_8(name, h), \
		UML_SIMD_INDEXED_ARRAY_8(name, 4h), \
		UML_SIMD_INDEXED_ARRAY_8(name, 8h), \
		UML_SIMD_INDEXED_ARRAY_4(name, s), \
		UML_SIMD_INDEXED_ARRAY_4(name, 2s), \
		UML_SIMD_INDEXED_ARRAY_4(name, 4s), \
		UML_SIMD_INDEXED_ARRAY_2(name, d), \
		UML_SIMD_INDEXED_ARRAY_2(name, 2d)}
	static constexpr std::array native_families{
		UML_SIMD_INDEXED_FLOATING_FAMILY(fmla),
		UML_SIMD_INDEXED_FLOATING_FAMILY(fmls),
		UML_SIMD_INDEXED_FLOATING_FAMILY(fmul),
		UML_SIMD_INDEXED_FLOATING_FAMILY(fmulx)};
	static constexpr std::array checked_families{
		UML_SIMD_INDEXED_FLOATING_FAMILY(fmla_checked),
		UML_SIMD_INDEXED_FLOATING_FAMILY(fmls_checked),
		UML_SIMD_INDEXED_FLOATING_FAMILY(fmul_checked),
		UML_SIMD_INDEXED_FLOATING_FAMILY(fmulx_checked)};
#undef UML_SIMD_INDEXED_FLOATING_FAMILY
	unsigned family;
	if (operation == 1U && !extended)
		family = 0U;
	else if (operation == 5U && !extended)
		family = 1U;
	else if (operation == 9U)
		family = extended ? 3U : 2U;
	else
		return nullptr;
	unsigned shape;
	if (size == 0U)
		shape = scalar ? 0U : q ? 2U : 1U;
	else if (size == 2U)
		shape = scalar ? 3U : q ? 5U : 4U;
	else if (size == 3U && (scalar || q))
		shape = scalar ? 6U : 7U;
	else
		return nullptr;
	const auto &families = host_has_afp() ?
		native_families : checked_families;
	return index < 8U ? families[family][shape][index] : nullptr;
}

[[nodiscard]] Gadget simd_fmulx_gadget(bool scalar, bool q, bool half,
	bool double_precision) noexcept
{
	static constexpr std::array<Gadget, 8> gadgets{
		uml_arm64_gadget_simd_fmulx_h,
		uml_arm64_gadget_simd_fmulx_4h,
		uml_arm64_gadget_simd_fmulx_8h,
		uml_arm64_gadget_simd_fmulx_s,
		uml_arm64_gadget_simd_fmulx_2s,
		uml_arm64_gadget_simd_fmulx_4s,
		uml_arm64_gadget_simd_fmulx_d,
		uml_arm64_gadget_simd_fmulx_2d};
	if (half)
		return gadgets[scalar ? 0U : q ? 2U : 1U];
	if (double_precision && !scalar && !q)
		return nullptr;
	return gadgets[double_precision ? (scalar ? 6U : 7U) :
		(scalar ? 3U : q ? 5U : 4U)];
}

struct SimdIndexedElement final {
	unsigned register_index;
	unsigned lane_index;
};

[[nodiscard]] constexpr std::optional<SimdIndexedElement>
decode_simd_indexed_element(std::uint32_t instruction) noexcept
{
	const unsigned size = (instruction >> 22) & 3U;
	unsigned register_index = (instruction >> 16) & 15U;
	if (size == 1U)
		return SimdIndexedElement{register_index,
			((instruction >> 9) & 4U) |
			((instruction >> 20) & 1U) |
			((instruction >> 20) & 2U)};
	if (size == 2U) {
		register_index |= ((instruction >> 20) & 1U) << 4;
		return SimdIndexedElement{register_index,
			((instruction >> 10) & 2U) |
			((instruction >> 21) & 1U)};
	}
	return std::nullopt;
}

[[nodiscard]] constexpr std::optional<SimdIndexedElement>
decode_simd_indexed_floating_element(std::uint32_t instruction) noexcept
{
	const unsigned size = (instruction >> 22) & 3U;
	unsigned register_index = (instruction >> 16) & 15U;
	if (size == 0U)
		return SimdIndexedElement{register_index,
			((instruction >> 9) & 4U) |
			((instruction >> 20) & 1U) |
			((instruction >> 20) & 2U)};
	if (size != 2U && size != 3U)
		return std::nullopt;
	register_index |= ((instruction >> 20) & 1U) << 4;
	if (size == 3U && (instruction & 0x0020'0000U) != 0)
		return std::nullopt;
	return SimdIndexedElement{register_index,
		size == 2U ? ((instruction >> 10) & 2U) |
			((instruction >> 21) & 1U) : (instruction >> 11) & 1U};
}

[[nodiscard]] constexpr SimdIndexedElement decode_simd_dot_indexed_element(
	std::uint32_t instruction) noexcept
{
	return SimdIndexedElement{
		((instruction >> 16) & 15U) | (((instruction >> 20) & 1U) << 4),
		((instruction >> 10) & 2U) | ((instruction >> 21) & 1U)};
}

[[nodiscard]] constexpr bool is_simd_indexed_integer_group(
	unsigned operation, bool unsigned_group, bool scalar) noexcept
{
	if (scalar)
		return unsigned_group ? operation == 13U || operation == 15U :
			operation == 3U || operation == 7U || operation == 11U ||
			operation == 12U || operation == 13U;
	if (unsigned_group)
		return operation == 0U || operation == 2U || operation == 4U ||
			operation == 6U || operation == 10U || operation == 13U ||
			operation == 15U;
	return operation == 2U || operation == 3U || operation == 6U ||
		operation == 7U || operation == 8U || operation == 10U ||
		operation == 11U || operation == 12U || operation == 13U;
}

[[nodiscard]] constexpr bool is_simd_indexed_scalar_reserved(
	unsigned operation, bool unsigned_group, unsigned size) noexcept
{
	const bool allocated_operation = unsigned_group ?
		operation == 9U || operation == 13U || operation == 15U :
		operation == 1U || operation == 3U || operation == 5U ||
		operation == 7U || operation == 9U || operation == 11U ||
		operation == 12U || operation == 13U;
	if (!allocated_operation)
		return true;
	const bool floating = operation == 1U || operation == 5U ||
		operation == 9U;
	return floating && size == 1U;
}

#undef UML_SIMD_INDEXED_ARRAY_8
#undef UML_SIMD_INDEXED_ARRAY_4
#undef UML_SIMD_INDEXED_ARRAY_2
#undef UML_SIMD_INDEXED_ITEMS_8
#undef UML_SIMD_INDEXED_ITEMS_4

[[nodiscard]] constexpr std::uint64_t repeat_simd_element(
	std::uint64_t element, unsigned lane_bits) noexcept
{
	std::uint64_t result = 0;
	for (unsigned bit = 0; bit < 64U; bit += lane_bits)
		result |= element << bit;
	return result;
}

[[nodiscard]] constexpr std::uint64_t simd_shift_insert_mask(
	unsigned lane_bits, unsigned shift, bool left) noexcept
{
	const auto lane_mask = lane_bits == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << lane_bits) - 1U;
	const auto element = left ? (std::uint64_t{1} << shift) - 1U :
		shift == lane_bits ? lane_mask : lane_mask ^ (lane_mask >> shift);
	return repeat_simd_element(element, lane_bits);
}

[[nodiscard]] Gadget simd_permute_gadget(
	unsigned opcode, bool q, unsigned size) noexcept
{
	if (size > 3U || (!q && size == 3U))
		return nullptr;
	const unsigned shape = q ? size + 3U : size;
#define UML_SIMD_PERMUTE(name) std::array<Gadget, 7>{ \
	uml_arm64_gadget_simd_permute_##name##_8b, \
	uml_arm64_gadget_simd_permute_##name##_4h, \
	uml_arm64_gadget_simd_permute_##name##_2s, \
	uml_arm64_gadget_simd_permute_##name##_16b, \
	uml_arm64_gadget_simd_permute_##name##_8h, \
	uml_arm64_gadget_simd_permute_##name##_4s, \
	uml_arm64_gadget_simd_permute_##name##_2d}
	static constexpr auto uzp1 = UML_SIMD_PERMUTE(uzp1);
	static constexpr auto trn1 = UML_SIMD_PERMUTE(trn1);
	static constexpr auto zip1 = UML_SIMD_PERMUTE(zip1);
	static constexpr auto uzp2 = UML_SIMD_PERMUTE(uzp2);
	static constexpr auto trn2 = UML_SIMD_PERMUTE(trn2);
	static constexpr auto zip2 = UML_SIMD_PERMUTE(zip2);
#undef UML_SIMD_PERMUTE
	switch (opcode) {
	case 1: return uzp1[shape];
	case 2: return trn1[shape];
	case 3: return zip1[shape];
	case 5: return uzp2[shape];
	case 6: return trn2[shape];
	case 7: return zip2[shape];
	default: return nullptr;
	}
}

[[nodiscard]] Gadget simd_table_lookup_gadget(
	bool extend, unsigned count, bool q) noexcept
{
	const unsigned shape = (count - 1U) * 2U + (q ? 1U : 0U);
	static constexpr std::array<Gadget, 8> tbl{
		uml_arm64_gadget_simd_tbl_1_8b,
		uml_arm64_gadget_simd_tbl_1_16b,
		uml_arm64_gadget_simd_tbl_2_8b,
		uml_arm64_gadget_simd_tbl_2_16b,
		uml_arm64_gadget_simd_tbl_3_8b,
		uml_arm64_gadget_simd_tbl_3_16b,
		uml_arm64_gadget_simd_tbl_4_8b,
		uml_arm64_gadget_simd_tbl_4_16b};
	static constexpr std::array<Gadget, 8> tbx{
		uml_arm64_gadget_simd_tbx_1_8b,
		uml_arm64_gadget_simd_tbx_1_16b,
		uml_arm64_gadget_simd_tbx_2_8b,
		uml_arm64_gadget_simd_tbx_2_16b,
		uml_arm64_gadget_simd_tbx_3_8b,
		uml_arm64_gadget_simd_tbx_3_16b,
		uml_arm64_gadget_simd_tbx_4_8b,
		uml_arm64_gadget_simd_tbx_4_16b};
	return (extend ? tbx : tbl)[shape];
}

[[nodiscard]] Gadget simd_lut_native_gadget(
	unsigned shape, unsigned part) noexcept
{
	static constexpr std::array<Gadget, 4> luti2_b{
		uml_arm64_gadget_simd_lut_native_luti2_b_0,
		uml_arm64_gadget_simd_lut_native_luti2_b_1,
		uml_arm64_gadget_simd_lut_native_luti2_b_2,
		uml_arm64_gadget_simd_lut_native_luti2_b_3};
	static constexpr std::array<Gadget, 8> luti2_h{
		uml_arm64_gadget_simd_lut_native_luti2_h_0,
		uml_arm64_gadget_simd_lut_native_luti2_h_1,
		uml_arm64_gadget_simd_lut_native_luti2_h_2,
		uml_arm64_gadget_simd_lut_native_luti2_h_3,
		uml_arm64_gadget_simd_lut_native_luti2_h_4,
		uml_arm64_gadget_simd_lut_native_luti2_h_5,
		uml_arm64_gadget_simd_lut_native_luti2_h_6,
		uml_arm64_gadget_simd_lut_native_luti2_h_7};
	static constexpr std::array<Gadget, 2> luti4_b{
		uml_arm64_gadget_simd_lut_native_luti4_b_0,
		uml_arm64_gadget_simd_lut_native_luti4_b_1};
        static constexpr std::array<Gadget, 4> luti4_h{
            uml_arm64_gadget_simd_lut_native_luti4_h_0,
            uml_arm64_gadget_simd_lut_native_luti4_h_1,
            uml_arm64_gadget_simd_lut_native_luti4_h_2,
            uml_arm64_gadget_simd_lut_native_luti4_h_3};
        switch (shape) {
        case 0:
          return part < luti2_b.size() ? luti2_b[part] : nullptr;
        case 1:
          return part < luti2_h.size() ? luti2_h[part] : nullptr;
        case 2:
          return part < luti4_b.size() ? luti4_b[part] : nullptr;
        case 3:
          return part < luti4_h.size() ? luti4_h[part] : nullptr;
        default:
          return nullptr;
        }
}

[[nodiscard]] Gadget simd_aes_gadget(unsigned operation,
                                     bool force_software) noexcept {
  const bool native = !force_software && host_has_aes();
  switch (operation) {
  case 4:
    return native ? uml_arm64_gadget_simd_aese_native
                  : uml_arm64_gadget_simd_aese_soft;
  case 5:
    return native ? uml_arm64_gadget_simd_aesd_native
                  : uml_arm64_gadget_simd_aesd_soft;
  case 6:
    return native ? uml_arm64_gadget_simd_aesmc_native
                  : uml_arm64_gadget_simd_aesmc_soft;
  case 7:
    return native ? uml_arm64_gadget_simd_aesimc_native
                  : uml_arm64_gadget_simd_aesimc_soft;
  default:
    return nullptr;
  }
}

[[nodiscard]] Gadget simd_sha_three_gadget(unsigned operation,
                                           bool force_software) noexcept {
  static constexpr std::array<Gadget, 7> native{
      uml_arm64_gadget_simd_sha1c,    uml_arm64_gadget_simd_sha1p,
      uml_arm64_gadget_simd_sha1m,    uml_arm64_gadget_simd_sha1su0,
      uml_arm64_gadget_simd_sha256h,  uml_arm64_gadget_simd_sha256h2,
      uml_arm64_gadget_simd_sha256su1};
  static constexpr std::array<Gadget, 7> software{
      uml_arm64_gadget_simd_sha1c_soft,    uml_arm64_gadget_simd_sha1p_soft,
      uml_arm64_gadget_simd_sha1m_soft,    uml_arm64_gadget_simd_sha1su0_soft,
      uml_arm64_gadget_simd_sha256h_soft,  uml_arm64_gadget_simd_sha256h2_soft,
      uml_arm64_gadget_simd_sha256su1_soft};
  if (operation >= native.size())
    return nullptr;
  const bool supported = operation <= 3U ? host_has_sha1() : host_has_sha256();
  return (!force_software && supported ? native : software)[operation];
}

[[nodiscard]] Gadget simd_sha_two_gadget(unsigned operation,
                                         bool force_software) noexcept {
  static constexpr std::array<Gadget, 3> native{
      uml_arm64_gadget_simd_sha1h, uml_arm64_gadget_simd_sha1su1,
      uml_arm64_gadget_simd_sha256su0};
  static constexpr std::array<Gadget, 3> software{
      uml_arm64_gadget_simd_sha1h_soft, uml_arm64_gadget_simd_sha1su1_soft,
      uml_arm64_gadget_simd_sha256su0_soft};
  if (operation >= native.size())
    return nullptr;
  const bool supported = operation <= 1U ? host_has_sha1() : host_has_sha256();
  return (!force_software && supported ? native : software)[operation];
}

[[nodiscard]] Gadget simd_sha512_three_gadget(std::uint32_t instruction,
                                              bool force_software) noexcept {
  const bool native = !force_software && host_has_sha512();
  switch (instruction & 0xffe0'fc00U) {
  case 0xce60'8000U:
    return native ? uml_arm64_gadget_simd_sha512h
                  : uml_arm64_gadget_simd_sha512h_soft;
  case 0xce60'8400U:
    return native ? uml_arm64_gadget_simd_sha512h2
                  : uml_arm64_gadget_simd_sha512h2_soft;
  case 0xce60'8800U:
    return native ? uml_arm64_gadget_simd_sha512su1
                  : uml_arm64_gadget_simd_sha512su1_soft;
  default:
    return nullptr;
  }
}

[[nodiscard]] Gadget simd_sm3tt_gadget(unsigned operation, unsigned index,
                                       bool force_software) noexcept {
  static constexpr std::array<Gadget, 16> native{
      uml_arm64_gadget_simd_sm3tt1a_0, uml_arm64_gadget_simd_sm3tt1a_1,
      uml_arm64_gadget_simd_sm3tt1a_2, uml_arm64_gadget_simd_sm3tt1a_3,
      uml_arm64_gadget_simd_sm3tt1b_0, uml_arm64_gadget_simd_sm3tt1b_1,
      uml_arm64_gadget_simd_sm3tt1b_2, uml_arm64_gadget_simd_sm3tt1b_3,
      uml_arm64_gadget_simd_sm3tt2a_0, uml_arm64_gadget_simd_sm3tt2a_1,
      uml_arm64_gadget_simd_sm3tt2a_2, uml_arm64_gadget_simd_sm3tt2a_3,
      uml_arm64_gadget_simd_sm3tt2b_0, uml_arm64_gadget_simd_sm3tt2b_1,
      uml_arm64_gadget_simd_sm3tt2b_2, uml_arm64_gadget_simd_sm3tt2b_3};
  static constexpr std::array<Gadget, 16> software{
      uml_arm64_gadget_simd_sm3tt1a_0_soft,
      uml_arm64_gadget_simd_sm3tt1a_1_soft,
      uml_arm64_gadget_simd_sm3tt1a_2_soft,
      uml_arm64_gadget_simd_sm3tt1a_3_soft,
      uml_arm64_gadget_simd_sm3tt1b_0_soft,
      uml_arm64_gadget_simd_sm3tt1b_1_soft,
      uml_arm64_gadget_simd_sm3tt1b_2_soft,
      uml_arm64_gadget_simd_sm3tt1b_3_soft,
      uml_arm64_gadget_simd_sm3tt2a_0_soft,
      uml_arm64_gadget_simd_sm3tt2a_1_soft,
      uml_arm64_gadget_simd_sm3tt2a_2_soft,
      uml_arm64_gadget_simd_sm3tt2a_3_soft,
      uml_arm64_gadget_simd_sm3tt2b_0_soft,
      uml_arm64_gadget_simd_sm3tt2b_1_soft,
      uml_arm64_gadget_simd_sm3tt2b_2_soft,
      uml_arm64_gadget_simd_sm3tt2b_3_soft};
  return (!force_software && host_has_sm3() ? native
                                            : software)[operation * 4U + index];
}

[[nodiscard]] Gadget simd_xar_gadget(
	unsigned rotate, bool native_path) noexcept
{
#define UML_SIMD_XAR_GADGETS(implementation) std::array<Gadget, 64>{ \
	uml_arm64_gadget_simd_xar_##implementation##_0, \
	uml_arm64_gadget_simd_xar_##implementation##_1, \
	uml_arm64_gadget_simd_xar_##implementation##_2, \
	uml_arm64_gadget_simd_xar_##implementation##_3, \
	uml_arm64_gadget_simd_xar_##implementation##_4, \
	uml_arm64_gadget_simd_xar_##implementation##_5, \
	uml_arm64_gadget_simd_xar_##implementation##_6, \
	uml_arm64_gadget_simd_xar_##implementation##_7, \
	uml_arm64_gadget_simd_xar_##implementation##_8, \
	uml_arm64_gadget_simd_xar_##implementation##_9, \
	uml_arm64_gadget_simd_xar_##implementation##_10, \
	uml_arm64_gadget_simd_xar_##implementation##_11, \
	uml_arm64_gadget_simd_xar_##implementation##_12, \
	uml_arm64_gadget_simd_xar_##implementation##_13, \
	uml_arm64_gadget_simd_xar_##implementation##_14, \
	uml_arm64_gadget_simd_xar_##implementation##_15, \
	uml_arm64_gadget_simd_xar_##implementation##_16, \
	uml_arm64_gadget_simd_xar_##implementation##_17, \
	uml_arm64_gadget_simd_xar_##implementation##_18, \
	uml_arm64_gadget_simd_xar_##implementation##_19, \
	uml_arm64_gadget_simd_xar_##implementation##_20, \
	uml_arm64_gadget_simd_xar_##implementation##_21, \
	uml_arm64_gadget_simd_xar_##implementation##_22, \
	uml_arm64_gadget_simd_xar_##implementation##_23, \
	uml_arm64_gadget_simd_xar_##implementation##_24, \
	uml_arm64_gadget_simd_xar_##implementation##_25, \
	uml_arm64_gadget_simd_xar_##implementation##_26, \
	uml_arm64_gadget_simd_xar_##implementation##_27, \
	uml_arm64_gadget_simd_xar_##implementation##_28, \
	uml_arm64_gadget_simd_xar_##implementation##_29, \
	uml_arm64_gadget_simd_xar_##implementation##_30, \
	uml_arm64_gadget_simd_xar_##implementation##_31, \
	uml_arm64_gadget_simd_xar_##implementation##_32, \
	uml_arm64_gadget_simd_xar_##implementation##_33, \
	uml_arm64_gadget_simd_xar_##implementation##_34, \
	uml_arm64_gadget_simd_xar_##implementation##_35, \
	uml_arm64_gadget_simd_xar_##implementation##_36, \
	uml_arm64_gadget_simd_xar_##implementation##_37, \
	uml_arm64_gadget_simd_xar_##implementation##_38, \
	uml_arm64_gadget_simd_xar_##implementation##_39, \
	uml_arm64_gadget_simd_xar_##implementation##_40, \
	uml_arm64_gadget_simd_xar_##implementation##_41, \
	uml_arm64_gadget_simd_xar_##implementation##_42, \
	uml_arm64_gadget_simd_xar_##implementation##_43, \
	uml_arm64_gadget_simd_xar_##implementation##_44, \
	uml_arm64_gadget_simd_xar_##implementation##_45, \
	uml_arm64_gadget_simd_xar_##implementation##_46, \
	uml_arm64_gadget_simd_xar_##implementation##_47, \
	uml_arm64_gadget_simd_xar_##implementation##_48, \
	uml_arm64_gadget_simd_xar_##implementation##_49, \
	uml_arm64_gadget_simd_xar_##implementation##_50, \
	uml_arm64_gadget_simd_xar_##implementation##_51, \
	uml_arm64_gadget_simd_xar_##implementation##_52, \
	uml_arm64_gadget_simd_xar_##implementation##_53, \
	uml_arm64_gadget_simd_xar_##implementation##_54, \
	uml_arm64_gadget_simd_xar_##implementation##_55, \
	uml_arm64_gadget_simd_xar_##implementation##_56, \
	uml_arm64_gadget_simd_xar_##implementation##_57, \
	uml_arm64_gadget_simd_xar_##implementation##_58, \
	uml_arm64_gadget_simd_xar_##implementation##_59, \
	uml_arm64_gadget_simd_xar_##implementation##_60, \
	uml_arm64_gadget_simd_xar_##implementation##_61, \
	uml_arm64_gadget_simd_xar_##implementation##_62, \
	uml_arm64_gadget_simd_xar_##implementation##_63}
	static constexpr auto native_gadgets = UML_SIMD_XAR_GADGETS(native);
	static constexpr auto software_gadgets = UML_SIMD_XAR_GADGETS(soft);
#undef UML_SIMD_XAR_GADGETS
	return (native_path ? native_gadgets : software_gadgets)[rotate];
}

[[nodiscard]] Gadget simd_pmull_gadget(
	bool high, unsigned size, bool force_software) noexcept
{
	if (size == 0U)
		return high ? uml_arm64_gadget_simd_pmull_8b2_direct :
			uml_arm64_gadget_simd_pmull_8b_direct;
	if (size == 3U) {
		const bool native = !force_software && host_has_pmull();
		if (native)
			return high ? uml_arm64_gadget_simd_pmull_1d2_native_direct :
				uml_arm64_gadget_simd_pmull_1d_native_direct;
		return high ? uml_arm64_gadget_simd_pmull_1d2_soft_direct :
			uml_arm64_gadget_simd_pmull_1d_soft_direct;
	}
	return nullptr;
}

[[nodiscard]] Gadget floating_select_gadget(
	unsigned condition, unsigned type) noexcept
{
#define UML_FLOATING_SELECT_WIDTH(name) \
	(type == 3U ? uml_arm64_gadget_floating_select_##name##_h : \
	 type == 1U ? uml_arm64_gadget_floating_select_##name##_d : \
	 uml_arm64_gadget_floating_select_##name##_s)
	switch (condition) {
	case 0: return UML_FLOATING_SELECT_WIDTH(eq);
	case 1: return UML_FLOATING_SELECT_WIDTH(ne);
	case 2: return UML_FLOATING_SELECT_WIDTH(hs);
	case 3: return UML_FLOATING_SELECT_WIDTH(lo);
	case 4: return UML_FLOATING_SELECT_WIDTH(mi);
	case 5: return UML_FLOATING_SELECT_WIDTH(pl);
	case 6: return UML_FLOATING_SELECT_WIDTH(vs);
	case 7: return UML_FLOATING_SELECT_WIDTH(vc);
	case 8: return UML_FLOATING_SELECT_WIDTH(hi);
	case 9: return UML_FLOATING_SELECT_WIDTH(ls);
	case 10: return UML_FLOATING_SELECT_WIDTH(ge);
	case 11: return UML_FLOATING_SELECT_WIDTH(lt);
	case 12: return UML_FLOATING_SELECT_WIDTH(gt);
	case 13: return UML_FLOATING_SELECT_WIDTH(le);
	default: return UML_FLOATING_SELECT_WIDTH(always);
	}
#undef UML_FLOATING_SELECT_WIDTH
}

[[nodiscard]] Gadget floating_compare_gadget(
	unsigned type, bool signaling, bool zero) noexcept
{
#define UML_FLOATING_COMPARE_WIDTH(kind, operand) \
	(type == 3U ? uml_arm64_gadget_floating_compare_##kind##_h_##operand : \
	 type == 1U ? uml_arm64_gadget_floating_compare_##kind##_d_##operand : \
	 uml_arm64_gadget_floating_compare_##kind##_s_##operand)
	if (signaling)
		return zero ? UML_FLOATING_COMPARE_WIDTH(signaling, zero) :
			UML_FLOATING_COMPARE_WIDTH(signaling, register);
	return zero ? UML_FLOATING_COMPARE_WIDTH(quiet, zero) :
		UML_FLOATING_COMPARE_WIDTH(quiet, register);
#undef UML_FLOATING_COMPARE_WIDTH
}

[[nodiscard]] Gadget floating_convert_precision_gadget(
	unsigned source_type, unsigned destination_type) noexcept
{
	if (source_type == 3U)
		return destination_type == 0U ?
			uml_arm64_gadget_floating_convert_h_to_s :
			uml_arm64_gadget_floating_convert_h_to_d;
	if (source_type == 0U)
		return destination_type == 3U ?
			uml_arm64_gadget_floating_convert_s_to_h :
			uml_arm64_gadget_floating_convert_s_to_d;
	return destination_type == 3U ?
		uml_arm64_gadget_floating_convert_d_to_h :
		uml_arm64_gadget_floating_convert_d_to_s;
}

[[nodiscard]] Gadget floating_conditional_compare_gadget(
	unsigned condition, unsigned type, bool signaling) noexcept
{
#define UML_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, kind) \
	(type == 3U ? uml_arm64_gadget_floating_conditional_compare_##name##_##kind##_h : \
	 type == 1U ? uml_arm64_gadget_floating_conditional_compare_##name##_##kind##_d : \
	 uml_arm64_gadget_floating_conditional_compare_##name##_##kind##_s)
#define UML_FLOATING_CONDITIONAL_COMPARE_KIND(name) \
	(signaling ? UML_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, signaling) : \
	 UML_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, quiet))
	switch (condition) {
	case 0: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(eq);
	case 1: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(ne);
	case 2: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(hs);
	case 3: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(lo);
	case 4: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(mi);
	case 5: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(pl);
	case 6: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(vs);
	case 7: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(vc);
	case 8: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(hi);
	case 9: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(ls);
	case 10: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(ge);
	case 11: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(lt);
	case 12: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(gt);
	case 13: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(le);
	default: return floating_compare_gadget(type, signaling, false);
	}
#undef UML_FLOATING_CONDITIONAL_COMPARE_KIND
#undef UML_FLOATING_CONDITIONAL_COMPARE_WIDTH
}

[[nodiscard]] constexpr std::int64_t sign_extend(std::uint64_t value,
						 unsigned bits) noexcept
{
	const auto shift = 64U - bits;
	return static_cast<std::int64_t>(value << shift) >> shift;
}

[[nodiscard]] constexpr std::uintptr_t rcpc_memory_operand(
	std::uint32_t instruction, std::int64_t offset) noexcept
{
	return ((instruction >> 5) & 31U) |
		(static_cast<std::uintptr_t>(instruction & 31U) << 5) |
		(static_cast<std::uintptr_t>(static_cast<std::uint64_t>(offset)) << 10);
}

[[nodiscard]] constexpr std::uintptr_t simd_ordered_memory_operand(
	std::uint32_t instruction, std::int64_t offset = 0) noexcept
{
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 5) |
		(static_cast<std::uintptr_t>(static_cast<std::uint64_t>(offset)) << 10);
}

[[nodiscard]] constexpr std::uintptr_t simd_lut_operand(
	std::uint32_t instruction, unsigned shape, unsigned part) noexcept
{
	const auto vector_offset = [](unsigned index) {
		return offsetof(CpuState, vectors) +
			index * sizeof(VectorRegister);
	};
	const unsigned table = (instruction >> 5) & 31U;
	return vector_offset(instruction & 31U) |
		(static_cast<std::uintptr_t>(vector_offset(table)) << 11) |
		(static_cast<std::uintptr_t>(
			vector_offset((table + 1U) & 31U)) << 22) |
		(static_cast<std::uintptr_t>(
			vector_offset((instruction >> 16) & 31U)) << 33) |
		(static_cast<std::uintptr_t>(shape) << 44) |
		(static_cast<std::uintptr_t>(part) << 46);
}

[[nodiscard]] constexpr std::uintptr_t ordered_pair_operand(
	std::uint32_t instruction) noexcept
{
	return ((instruction >> 5) & 31U) |
		(static_cast<std::uintptr_t>(instruction & 31U) << 5) |
		(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 10);
}

[[nodiscard]] constexpr std::uintptr_t rcw_scalar_operand(
	std::uint32_t instruction, bool compare_swap) noexcept
{
	const unsigned first = (instruction >> 16) & 31U;
	const unsigned second = instruction & 31U;
	const unsigned source = compare_swap ? second : first;
	const unsigned result = compare_swap ? first : second;
	const unsigned base = (instruction >> 5) & 31U;
	const auto base_offset = base < 31U ?
		base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
	const auto source_offset = source < 31U ?
		source * sizeof(std::uint64_t) : 0U;
	const auto result_offset = result < 31U ?
		result * sizeof(std::uint64_t) : 0U;
	return base_offset |
		(static_cast<std::uintptr_t>(source_offset) << 16) |
		(static_cast<std::uintptr_t>(result_offset) << 32);
}

[[nodiscard]] constexpr std::uintptr_t rcw_pair_operand(
	std::uint32_t instruction) noexcept
{
	const unsigned base = (instruction >> 5) & 31U;
	const unsigned first = instruction & 31U;
	const unsigned second = (instruction >> 16) & 31U;
	const auto base_offset = base < 31U ?
		base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
	const auto first_offset = first < 31U ?
		first * sizeof(std::uint64_t) : 0U;
	const auto second_offset = second < 31U ?
		second * sizeof(std::uint64_t) : 0U;
	return base_offset |
		(static_cast<std::uintptr_t>(first_offset) << 16) |
		(static_cast<std::uintptr_t>(second_offset) << 32);
}

[[nodiscard]] constexpr std::uintptr_t mte_register_offset(
	unsigned index) noexcept
{
	static_assert(offsetof(CpuState, registers) == 0);
	static_assert(offsetof(CpuState, sp) <= 0xffffU);
	return index == 31U ? offsetof(CpuState, sp) :
		index * sizeof(std::uint64_t);
}

[[nodiscard]] constexpr std::uintptr_t mte_pointer_operand(
	std::uint32_t instruction) noexcept
{
	return mte_register_offset(instruction & 31U) |
		(mte_register_offset((instruction >> 5) & 31U) << 16) |
		(mte_register_offset((instruction >> 16) & 31U) << 32);
}

[[nodiscard]] constexpr std::uintptr_t cpa_pointer_operand(
	std::uint32_t instruction) noexcept
{
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
		(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16);
}

[[nodiscard]] constexpr std::uintptr_t arithmetic_register_offset(
	unsigned index) noexcept
{
	static_assert(offsetof(CpuState, registers) == 0);
	static_assert(offsetof(CpuState, gadget_zero) <= 0xffffU);
	return index == 31U ? offsetof(CpuState, gadget_zero) :
		index * sizeof(std::uint64_t);
}

[[nodiscard]] constexpr std::uintptr_t arithmetic_binary_operand(
	std::uint32_t instruction) noexcept
{
	return arithmetic_register_offset(instruction & 31U) |
		(arithmetic_register_offset((instruction >> 5) & 31U) << 16) |
		(arithmetic_register_offset((instruction >> 16) & 31U) << 32);
}

[[nodiscard]] constexpr std::uintptr_t mops_operand(
	std::uint32_t instruction) noexcept
{
	/* MOPS never treats register 31 as SP. The only valid use is SET/SETG
	 * source XZR, which translation maps to the immutable zero slot. */
	return arithmetic_register_offset(instruction & 31U) |
		(arithmetic_register_offset((instruction >> 5) & 31U) << 16) |
		(arithmetic_register_offset((instruction >> 16) & 31U) << 32);
}

[[nodiscard]] constexpr std::uintptr_t cpa_multiply_operand(
	std::uint32_t instruction) noexcept
{
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
		(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16) |
		(static_cast<std::uintptr_t>((instruction >> 10) & 31U) << 24);
}

[[nodiscard]] constexpr std::uintptr_t mte_tag_operand(
	std::uint32_t instruction) noexcept
{
	return mte_register_offset(instruction & 31U) |
		(mte_register_offset((instruction >> 5) & 31U) << 16) |
		(static_cast<std::uintptr_t>((instruction >> 10) & 15U) << 32) |
		(static_cast<std::uintptr_t>((instruction >> 16) & 63U) << 40);
}

enum class ProductionDecodeStart : std::uint8_t {
	branch_system,
	data_processing_immediate,
	data_processing_register,
	data_processing_pointer_auth_overlap,
	floating_simd,
	load_store,
	exception
};

/* Generate the architectural first-level decoder from A64's op0 field.
 * Fine-grained masks remain in their existing priority order inside each
 * family, so overlapping encodings retain exactly the audited behavior. */
[[nodiscard]] consteval std::array<ProductionDecodeStart, 256>
make_production_decode_starts()
{
	std::array<ProductionDecodeStart, 256> starts{};
	for (unsigned byte = 0; byte < starts.size(); ++byte) {
		const unsigned op0 = (byte >> 1) & 15U;
		switch (op0) {
		case 8:
		case 9:
			starts[byte] = ProductionDecodeStart::data_processing_immediate;
			break;
		case 5:
		case 13:
			starts[byte] = ProductionDecodeStart::data_processing_register;
			break;
		case 7:
		case 15:
			starts[byte] = ProductionDecodeStart::floating_simd;
			break;
		case 4:
		case 6:
		case 12:
		case 14:
			starts[byte] = ProductionDecodeStart::load_store;
			break;
		default:
			starts[byte] = ProductionDecodeStart::branch_system;
			break;
		}
		/* ADR/ADRP use immlo in high-byte bits 6:5, so all eight possible
		 * high bytes share this low-five-bit signature. */
		if ((byte & 0x1fU) == 0x10U)
			starts[byte] = ProductionDecodeStart::branch_system;
	}
	/* Exceptions share op0 with branches/system but sit at the tail of the
	 * detailed chain. Their distinct high byte can jump there directly. */
	starts[0xd4] = ProductionDecodeStart::exception;
	/* FEAT_PAuth_LR has two encodings inside data-processing space. Route
	 * those high-byte overlaps through the exact unavailability predicate,
	 * then resume ordinary data processing when it does not match. */
	starts[0xda] =
		ProductionDecodeStart::data_processing_pointer_auth_overlap;
	starts[0xf3] =
		ProductionDecodeStart::data_processing_pointer_auth_overlap;
	return starts;
}

inline constexpr auto production_decode_starts =
	make_production_decode_starts();

} // namespace

Program::Program(std::uint64_t guest_address, std::size_t instruction_count,
		 std::pmr::vector<std::uintptr_t> words,
		 bool accesses_fp_state) noexcept
	: guest_address_{guest_address}, instruction_count_{instruction_count},
	  words_{std::move(words)}, accesses_fp_state_{accesses_fp_state}
{
}

BuildResult Builder::build(
	std::uint64_t guest_address,
	std::span<const std::uint32_t> instructions,
	LinkResolver resolve_link, std::pmr::memory_resource *storage,
	IndirectLinkResolver resolve_indirect_link, BuildOptions options) const
{
	const ::uml::cpu::HostCapabilitiesScope capabilities_scope{
		options.host_capabilities};
	std::pmr::vector<std::uintptr_t> words{storage};
	words.reserve(std::min<std::size_t>(
		instructions.size() * 2 + 2, 34));
	std::size_t compiled = 0;
	bool program_accesses_fp_state = false;
	for (const auto instruction : instructions) {
		program_accesses_fp_state = program_accesses_fp_state ||
			uml::cpu::arm64::detail::
				instruction_accesses_fp_state(instruction);
		const auto address = guest_address + compiled * sizeof(instruction);
		if ((instruction & 0xffff'0000U) == 0) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else {
			switch (production_decode_starts[instruction >> 24]) {
			case ProductionDecodeStart::branch_system:
				goto decode_branch_system;
			case ProductionDecodeStart::data_processing_immediate:
				goto decode_data_processing_immediate;
			case ProductionDecodeStart::data_processing_register:
				/* Immediate and register families are interleaved in the
				 * audited fine decoder; share their first entry while still
				 * skipping the branch/system prefix. */
				goto decode_data_processing_immediate;
			case ProductionDecodeStart::data_processing_pointer_auth_overlap:
				if (pointer_auth_lr_unavailable(instruction))
					goto decode_branch_system;
				goto decode_data_processing_immediate;
			case ProductionDecodeStart::floating_simd:
				goto decode_floating_simd;
			case ProductionDecodeStart::load_store:
				goto decode_load_store_guard;
			case ProductionDecodeStart::exception:
				goto decode_exception;
			}
		}

decode_load_store_guard:
		if (const auto selected = gcs_gadget(instruction); selected) {
			emit(words, selected->entry);
			if (selected->has_operand)
				emit(words, selected->operand);
			emit(words, address);
			goto instruction_decoded;
		}
		goto decode_load_store;

decode_branch_system:
		if (const auto selected = gcs_gadget(instruction); selected) {
			emit(words, selected->entry);
			if (selected->has_operand)
				emit(words, selected->operand);
			emit(words, address);
			if (selected->always_traps) {
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if (pointer_auth_lr_unavailable(instruction)) {
			/* FEAT_PAuth_LR is not advertised by the static guest profile.
			 * Its non-HINT encodings are architecturally UNDEFINED. */
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0x7c00'0000U) == 0x1400'0000U) {
			const auto displacement = sign_extend(
				static_cast<std::uint64_t>(instruction & 0x03ff'ffffU) << 2,
				28);
			const auto target = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + displacement);
			const bool link = (instruction >> 31) != 0;
			if (resolve_link) {
				emit(words, link ? uml_arm64_gadget_branch_link_linked :
					uml_arm64_gadget_branch_linked);
				emit(words, reinterpret_cast<std::uintptr_t>(
					resolve_link(target)));
			} else {
				emit(words, link ? uml_arm64_gadget_branch_link :
					uml_arm64_gadget_branch);
				emit(words, target);
			}
			if (link)
				emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0xff00'0000U) == 0x5400'0000U) {
			const auto displacement = sign_extend(
				static_cast<std::uint64_t>((instruction >> 5) & 0x7ffffU) << 2,
				21);
			const auto target = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + displacement);
			const auto fallthrough = address + sizeof(instruction);
			if (resolve_link) {
				emit(words, conditional_branch_linked_gadget(
					instruction & 15U));
				emit(words, reinterpret_cast<std::uintptr_t>(
					resolve_link(target)));
				emit(words, reinterpret_cast<std::uintptr_t>(
					resolve_link(fallthrough)));
			} else {
				emit(words, conditional_branch_gadget(instruction & 15U));
				emit(words, target);
				emit(words, fallthrough);
			}
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0x7e00'0000U) == 0x7400'0000U) {
			const bool immediate = (instruction & 0x0100'0000U) != 0;
			const unsigned narrow_control = (instruction >> 14) & 3U;
			const unsigned cc = (instruction >> 21) & 7U;
			const bool valid_form = immediate ?
				(instruction & 0x0000'4000U) == 0 :
				(narrow_control == 0 ||
				 (((instruction >> 31) == 0) && narrow_control >= 2));
			if (!valid_form || cc == 4 || cc == 5) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const auto displacement = sign_extend(
				static_cast<std::uint64_t>(
					(instruction >> 5) & 0x1ffU) << 2,
				11);
			const auto target = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + displacement);
			const auto fallthrough = address + sizeof(instruction);
			const unsigned left = instruction & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			bool constant = false;
			bool take = false;
			if (immediate && left == 31) {
				constant = true;
				take = cmpbr_immediate_zero_result(
					cc, (instruction >> 15) & 63U);
			} else if (!immediate && left == right) {
				constant = true;
				take = cmpbr_equal_register_result(cc);
			}
			if (constant) {
				if (resolve_link) {
					emit(words, uml_arm64_gadget_branch_always_linked);
					emit(words, reinterpret_cast<std::uintptr_t>(
						resolve_link(take ? target : fallthrough)));
				} else {
					emit(words, uml_arm64_gadget_branch_always);
					emit(words, take ? target : fallthrough);
				}
			} else if (immediate) {
				emit(words, cmpbr_immediate_gadget(
					(instruction >> 31) != 0, cc));
				emit(words, left |
					(static_cast<std::uintptr_t>(
						(instruction >> 15) & 63U) << 8));
				emit(words, target);
				emit(words, fallthrough);
			} else {
				const unsigned width = narrow_control == 2 ? 8U :
					narrow_control == 3 ? 16U :
					(instruction >> 31) != 0 ? 64U : 32U;
				emit(words, cmpbr_register_gadget(width, cc,
					left == 31, right == 31));
				emit(words, left |
					(static_cast<std::uintptr_t>(right) << 8));
				emit(words, target);
				emit(words, fallthrough);
			}
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0x7e00'0000U) == 0x3400'0000U) {
			const auto displacement = sign_extend(
				static_cast<std::uint64_t>((instruction >> 5) & 0x7ffffU) << 2,
				21);
			const auto target = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + displacement);
			const auto fallthrough = address + sizeof(instruction);
			const unsigned source = instruction & 31U;
			const bool nonzero = (instruction & 0x0100'0000U) != 0;
			if (source == 31U) {
				const auto selected = nonzero ? fallthrough : target;
				if (resolve_link) {
					emit(words, uml_arm64_gadget_branch_always_linked);
					emit(words, reinterpret_cast<std::uintptr_t>(
						resolve_link(selected)));
				} else {
					emit(words, uml_arm64_gadget_branch_always);
					emit(words, selected);
				}
			} else {
				emit(words, compare_branch_gadget(
					(instruction >> 31) != 0, nonzero,
					static_cast<bool>(resolve_link)));
				emit(words, offsetof(CpuState, registers) + source * 8U);
				if (resolve_link) {
					emit(words, reinterpret_cast<std::uintptr_t>(
						resolve_link(target)));
					emit(words, reinterpret_cast<std::uintptr_t>(
						resolve_link(fallthrough)));
				} else {
					emit(words, target);
					emit(words, fallthrough);
				}
			}
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
			const auto target = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + displacement);
			const auto fallthrough = address + sizeof(instruction);
			const unsigned source = instruction & 31U;
			const bool nonzero = (instruction & 0x0100'0000U) != 0;
			if (source == 31U) {
				const auto selected = nonzero ? fallthrough : target;
				if (resolve_link) {
					emit(words, uml_arm64_gadget_branch_always_linked);
					emit(words, reinterpret_cast<std::uintptr_t>(
						resolve_link(selected)));
				} else {
					emit(words, uml_arm64_gadget_branch_always);
					emit(words, selected);
				}
			} else {
				emit(words, test_branch_gadget(nonzero,
					static_cast<bool>(resolve_link)));
				emit(words, offsetof(CpuState, registers) + source * 8U);
				emit(words, std::uintptr_t{1} << bit);
				if (resolve_link) {
					emit(words, reinterpret_cast<std::uintptr_t>(
						resolve_link(target)));
					emit(words, reinterpret_cast<std::uintptr_t>(
						resolve_link(fallthrough)));
				} else {
					emit(words, target);
					emit(words, fallthrough);
				}
			}
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0xfedf'f800U) == 0xd61f'0800U) {
			if (!options.enable_pointer_authentication) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const bool zero_modifier =
				(instruction & 0x0100'0000U) == 0;
			const unsigned modifier = instruction & 31U;
			const unsigned target = (instruction >> 5) & 31U;
			if (zero_modifier && modifier != 31U) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const bool link = (instruction & 0x0020'0000U) != 0;
			const bool key_b = (instruction & 0x400U) != 0;
			emit(words,
				link && options.enable_guarded_control_flow ?
					pointer_auth_guarded_call_gadget(
						key_b, zero_modifier,
						target == 31U) :
					pointer_auth_branch_gadget(
						link, key_b, zero_modifier,
						target == 31U));
			if (target != 31U || !zero_modifier) {
				const auto operand = target == 31U ?
					(modifier < 31U ?
						modifier * sizeof(std::uint64_t) :
						offsetof(CpuState, sp)) :
					zero_modifier ?
						target * sizeof(std::uint64_t) :
						pointer_auth_operand(target, modifier);
				emit(words, operand);
			}
			if (link)
				emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0xffff'fbffU) == 0xd65f'0bffU) {
			if (!options.enable_pointer_authentication) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const bool key_b = (instruction & 0x400U) != 0;
			emit(words, options.enable_guarded_control_flow ?
				pointer_auth_guarded_return_gadget(key_b) :
				pointer_auth_branch_gadget(
					false, key_b, false, false));
			emit(words, pointer_auth_operand(30, 31));
			if (options.enable_guarded_control_flow)
				emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0xffff'fc1fU) == 0xd61f'0000U ||
			   (instruction & 0xffff'fc1fU) == 0xd63f'0000U ||
			   (instruction & 0xffff'fc1fU) == 0xd65f'0000U) {
			const auto opcode = instruction & 0xffff'fc1fU;
			const unsigned source = (instruction >> 5) & 31U;
			const bool cached = static_cast<bool>(resolve_indirect_link);
			emit(words, opcode == 0xd61f'0000U ?
				(cached ? uml_arm64_gadget_branch_register_cached :
					uml_arm64_gadget_branch_register) :
				opcode == 0xd63f'0000U ?
				(cached ? uml_arm64_gadget_branch_link_register_cached :
					uml_arm64_gadget_branch_link_register) :
				(cached ? uml_arm64_gadget_return_cached :
					uml_arm64_gadget_return));
			emit(words, source == 31U ? offsetof(CpuState, gadget_zero) :
				static_cast<std::uintptr_t>(source * 8U));
			if (cached)
				emit(words, reinterpret_cast<std::uintptr_t>(
					resolve_indirect_link(address)));
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
			const unsigned destination = instruction & 31U;
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				emit(words, uml_arm64_gadget_pc_relative);
				emit(words, static_cast<std::uintptr_t>(destination * 8U));
				emit(words, value);
			}
		} else if (instruction == 0xd500'401fU) {
			emit(words, uml_arm64_gadget_cfinv);
		} else if (instruction == 0xd500'405fU) {
			emit(words, !options.force_software_flags &&
					host_capabilities().has(ArmFeature::flag_m2) ?
				uml_arm64_gadget_axflag_native :
				uml_arm64_gadget_axflag);
		} else if (instruction == 0xd500'403fU) {
			emit(words, !options.force_software_flags &&
					host_capabilities().has(ArmFeature::flag_m2) ?
				uml_arm64_gadget_xaflag_native :
				uml_arm64_gadget_xaflag);
		} else if ((instruction & 0xfff8'f01fU) == 0xd500'401fU) {
			const auto selected = pstate_immediate_gadget(instruction);
			if (!selected.valid) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			emit(words, selected.gadget);
			if (selected.fault_address)
				emit(words, address);
		} else if ((instruction & 0xffff'ffe0U) == 0xd53b'd040U ||
			   (instruction & 0xffff'ffe0U) == 0xd51b'd040U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'd060U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'00e0U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'0020U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'4400U ||
			   (instruction & 0xffff'ffe0U) == 0xd51b'4400U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'4420U ||
			   (instruction & 0xffff'ffe0U) == 0xd51b'4420U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'4440U ||
			   (instruction & 0xffff'ffe0U) == 0xd51b'4440U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'4200U ||
			   (instruction & 0xffff'ffe0U) == 0xd51b'4200U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'e000U ||
			   (instruction & 0xffff'ffe0U) == 0xd53b'e040U) {
			const auto selected = system_register_gadget(instruction);
			emit(words, selected.gadget);
			if (selected.operand)
				emit(words, static_cast<std::uintptr_t>(
					(instruction & 31U) * sizeof(std::uint64_t)));
			/* A register-sourced FPCR write may enable synchronous exception
			 * reporting. Supply its already-known fallthrough PC so the
			 * rare enabled case can leave the block immediately; ordinary
			 * control-only writes remain straight through.
			 */
			if ((instruction & 0xffff'ffe0U) == 0xd51b'4400U &&
			    (instruction & 31U) != 31U) {
				emit(words, address + sizeof(instruction));
			}
		} else if ((instruction & 0xffd0'0000U) == 0xd510'0000U ||
			   (instruction & 0xffd0'0000U) == 0xd550'0000U ||
			   (instruction & 0xfff8'0000U) == 0xd548'0000U) {
			/* Every implemented EL0 register is selected above. Unknown 64-bit
			 * registers plus FEAT_SYSREG128/SYSINSTR128 operations are
			 * inaccessible in the current userspace architectural profile. */
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0xfff8'0000U) == 0xd508'0000U ||
			   (instruction & 0xfff8'0000U) == 0xd528'0000U) {
			const auto operation = instruction & 0xffff'ffe0U;
			if (operation == 0xd50b'7420U) {
				const unsigned source = instruction & 31U;
				emit(words, source == 31U ?
					uml_arm64_gadget_cache_zero_z :
					uml_arm64_gadget_cache_zero_r);
				if (source != 31U)
					emit(words, static_cast<std::uintptr_t>(
						source * sizeof(std::uint64_t)));
				emit(words, address);
			} else if (operation == 0xd50b'7b20U) {
				/* Guest writes already update the coherent backing. */
				emit(words, uml_arm64_gadget_nop);
			} else if (operation == 0xd50b'7520U) {
				const unsigned source = instruction & 31U;
				emit(words, source == 31U ?
					uml_arm64_gadget_code_synchronize_z :
					uml_arm64_gadget_code_synchronize_r);
				if (source != 31U)
					emit(words, static_cast<std::uintptr_t>(
						source * sizeof(std::uint64_t)));
				emit(words, address + sizeof(instruction));
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0xffff'f0ffU) == 0xd503'305fU) {
			emit(words, uml_arm64_gadget_clear_exclusive);
		} else if ((instruction & 0xffff'f0ffU) == 0xd503'309fU ||
			   (instruction & 0xffff'f0ffU) == 0xd503'30bfU ||
			   (instruction & 0xffff'f0ffU) == 0xd503'30dfU ||
			   (instruction & 0xffff'f3ffU) == 0xd503'323fU ||
			   instruction == 0xd503'30ffU) {
			emit(words, memory_barrier_gadget(instruction));
		} else if ((instruction & 0xffff'f0ffU) == 0xd503'30ffU) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if (instruction == 0xd503'20ffU) {
			emit(words, options.enable_pointer_authentication ?
				uml_arm64_gadget_xpaclri : uml_arm64_gadget_nop);
		} else if (const auto pointer_auth =
			decode_pointer_auth_hint(instruction); pointer_auth) {
			if (!options.enable_pointer_authentication) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				emit(words, pointer_auth_gadgets[pointer_auth->operation]
					[pointer_auth->zero_modifier]);
				emit(words, pointer_auth_operand(
					pointer_auth->destination,
					pointer_auth->modifier));
			}
		} else if ((instruction & 0xffff'f01fU) == 0xd503'201fU) {
			emit(words, architectural_hint_gadget(instruction));
		} else if ((instruction & 0xffd8'f000U) == 0xd500'2000U ||
			   (instruction & 0xffff'ffe0U) == 0xd503'1000U ||
			   (instruction & 0xffff'ffe0U) == 0xd503'1020U) {
			emit(words, uml_arm64_gadget_nop);
		} else if (instruction == 0xd6bf'03e0U ||
			   instruction == 0xd69f'03e0U ||
			   instruction == 0xd69f'0bffU ||
			   instruction == 0xd69f'0fffU) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else {
			emit_fatal(words, address);
		}
		goto instruction_decoded;

decode_data_processing_immediate:
		if ((instruction & 0x1f80'0000U) == 0x1280'0000U) {
			const unsigned operation = (instruction >> 29) & 3U;
			const unsigned halfword = (instruction >> 21) & 3U;
			const bool is_64_bit = (instruction >> 31) != 0;
			const unsigned destination = instruction & 31U;
			if (operation == 1 ||
			    (!is_64_bit && halfword >= 2)) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else if (operation == 3U) {
				static constexpr Gadget keep[2][4]{
					{uml_arm64_gadget_move_wide_keep_w0,
					 uml_arm64_gadget_move_wide_keep_w1, nullptr, nullptr},
					{uml_arm64_gadget_move_wide_keep_x0,
					 uml_arm64_gadget_move_wide_keep_x1,
					 uml_arm64_gadget_move_wide_keep_x2,
					 uml_arm64_gadget_move_wide_keep_x3}};
				emit(words, keep[is_64_bit][halfword]);
				emit(words, destination |
					(static_cast<std::uintptr_t>(
						(instruction >> 5) & 0xffffU) << 8));
			} else {
				auto value = static_cast<std::uint64_t>(
					(instruction >> 5) & 0xffffU) << (halfword * 16U);
				if (operation == 0U)
					value = ~value;
				if (!is_64_bit)
					value &= 0xffff'ffffU;
				emit(words, uml_arm64_gadget_move_wide_constant);
				emit(words, destination * sizeof(std::uint64_t));
				emit(words, static_cast<std::uintptr_t>(value));
			}
		} else if ((instruction & 0x7ff0'0000U) == 0x11c0'0000U) {
			const unsigned destination = instruction & 31U;
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				const unsigned source = (instruction >> 5) & 31U;
				const unsigned operation =
					(instruction >> 18) & 3U;
				const unsigned encoded =
					(instruction >> 10) & 0xffU;
				const bool is_64_bit =
					(instruction & 0x8000'0000U) != 0;
				const auto zero = offsetof(CpuState, gadget_zero);
				const auto source_offset = source == 31U ? zero : source * 8U;
				/* UMAX #0 is an unconditional copy. */
				if (operation == 1U && encoded == 0U) {
					if (is_64_bit && destination == source) {
						emit(words, uml_arm64_gadget_nop);
					} else {
						emit(words, is_64_bit ?
							uml_arm64_gadget_integer_min_max_copy_x :
							uml_arm64_gadget_integer_min_max_copy_w);
						emit(words,
							static_cast<std::uintptr_t>(
								destination * 8U) |
							(static_cast<std::uintptr_t>(
								source_offset)
							 << 16));
					}
				} else {
					std::uint32_t immediate = encoded;
					if ((operation & 1U) == 0U &&
					    (encoded & 0x80U) != 0)
						immediate |= 0xffff'ff00U;
					emit(words, integer_min_max_gadget(
						operation, is_64_bit, true));
					emit(words,
						static_cast<std::uintptr_t>(
							destination * 8U) |
						(static_cast<std::uintptr_t>(
							source_offset) << 16) |
						(static_cast<std::uintptr_t>(
							immediate) << 32));
				}
			}
		} else if ((instruction & 0xffc0'c000U) == 0x9180'0000U ||
			   (instruction & 0xffc0'c000U) == 0xd180'0000U) {
			emit(words, mte_tag_add_sub_gadget(
				(instruction & 0x4000'0000U) != 0,
				((instruction >> 5) & 31U) == 31U,
				(instruction & 31U) == 31U));
			emit(words, mte_tag_operand(instruction));
		} else if ((instruction & 0x1f80'0000U) == 0x1180'0000U) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0x1f80'0000U) == 0x1100'0000U) {
			const bool set_flags = (instruction & 0x2000'0000U) != 0;
			const unsigned destination = instruction & 31U;
			const unsigned source = (instruction >> 5) & 31U;
			const auto immediate = static_cast<std::uintptr_t>(
				(instruction >> 10) & 0xfffU) <<
				((instruction & 0x0040'0000U) != 0 ? 12U : 0U);
			emit(words, add_sub_immediate_gadget(
				(instruction & 0x8000'0000U) != 0,
				(instruction & 0x4000'0000U) != 0, set_flags,
				source == 31U, destination == 31U));
			emit(words, destination |
				(static_cast<std::uintptr_t>(source) << 8) |
				(immediate << 16));
		} else if ((instruction & 0x1f20'0000U) == 0x0b00'0000U ||
			   (instruction & 0x1fe0'0000U) == 0x0b20'0000U) {
			const bool extended = (instruction & 0x0020'0000U) != 0;
			const unsigned shift = (instruction >> 10) &
				(extended ? 0x7U : 0x3fU);
			const unsigned shift_type = (instruction >> 22) & 0x3U;
			const bool is_64_bit = (instruction >> 31) != 0;
			if ((extended && shift > 4) ||
			    (!extended && (shift_type == 3 ||
				(!is_64_bit && shift >= 32)))) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const unsigned destination = instruction & 31U;
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			const bool set_flags = (instruction & 0x2000'0000U) != 0;
			const bool left_zero = !extended && left == 31U;
			const bool right_zero = right == 31U;
			const bool destination_discard = destination == 31U &&
				(!extended || set_flags);
			const unsigned destination_offset = destination == 31U ?
				offsetof(CpuState, sp) : destination * sizeof(std::uint64_t);
			const unsigned left_offset = left == 31U ?
				offsetof(CpuState, sp) : left * sizeof(std::uint64_t);
			const unsigned right_offset = right_zero ? 0U :
				right * sizeof(std::uint64_t);
			const unsigned transform = extended ? 0U :
				shift == 0U ? 0U : shift_type + 1U;
			emit(words, add_sub_register_gadget(is_64_bit,
				(instruction & 0x4000'0000U) != 0, set_flags, extended,
				transform, (instruction >> 13) & 7U, shift != 0U,
				left_zero, right_zero, destination_discard));
			emit(words, destination_offset |
				(static_cast<std::uintptr_t>(left_offset) << 16) |
				(static_cast<std::uintptr_t>(right_offset) << 32) |
				(static_cast<std::uintptr_t>(shift) << 48));
		} else if ((instruction & 0xffe0'fc00U) == 0x9ac0'1000U) {
			emit(words, mte_irg_gadget(
				((instruction >> 5) & 31U) == 31U,
				((instruction >> 16) & 31U) == 31U,
				(instruction & 31U) == 31U));
			emit(words, mte_pointer_operand(instruction));
			emit(words, reinterpret_cast<std::uintptr_t>(
				byte_popcount_table.data()));
		} else if ((instruction & 0xffe0'e000U) == 0x9a00'2000U ||
			   (instruction & 0xffe0'e000U) == 0xda00'2000U) {
			emit(words, cpa_pointer_gadget(
				(instruction & 0x4000'0000U) != 0,
				(instruction >> 10) & 7U,
				((instruction >> 5) & 31U) == 31U,
				((instruction >> 16) & 31U) == 31U,
				(instruction & 31U) == 31U));
			emit(words, cpa_pointer_operand(instruction));
		} else if ((instruction & 0xffe0'fc00U) == 0x9ac0'0000U ||
			   (instruction & 0xffe0'fc00U) == 0xbac0'0000U ||
			   (instruction & 0xffe0'fc00U) == 0x9ac0'1400U) {
			const auto opcode = instruction & 0xffe0'fc00U;
			const unsigned operation = opcode == 0x9ac0'1400U ? 0U :
				opcode == 0x9ac0'0000U ? 1U : 2U;
			emit(words, mte_pointer_gadget(operation,
				((instruction >> 5) & 31U) == 31U,
				((instruction >> 16) & 31U) == 31U,
				(instruction & 31U) == 31U));
			emit(words, mte_pointer_operand(instruction));
		} else if ((instruction & 0x1fe0'fc00U) == 0x1a00'0000U) {
			const unsigned destination = instruction & 31U;
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			const bool set_flags =
				(instruction & 0x2000'0000U) != 0;
			if (destination == 31U && !set_flags) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				emit(words, add_sub_carry_gadget(
					(instruction & 0x8000'0000U) != 0,
					(instruction & 0x4000'0000U) != 0,
					set_flags, left == 31U, right == 31U,
					left == right && left != 31U,
					destination == 31U));
				emit(words,
					arithmetic_binary_operand(instruction));
			}
		} else if ((instruction & 0xffe0'7c10U) == 0xba00'0400U) {
			const unsigned source = (instruction >> 5) & 31U;
			const auto shifted_mask =
				static_cast<std::uintptr_t>(instruction & 15U) << 28;
			if (shifted_mask == 0U) {
				emit(words, uml_arm64_gadget_nop);
			} else if (source == 31U) {
				emit(words, uml_arm64_gadget_rmif_zero);
				emit(words, shifted_mask);
			} else {
				emit(words, uml_arm64_gadget_rmif_fast);
				emit(words, static_cast<std::uintptr_t>(source * 8U));
				emit(words, static_cast<std::uintptr_t>(
					(instruction >> 15) & 63U));
				emit(words, shifted_mask);
			}
		} else if ((instruction & 0xffff'bc1fU) == 0x3a00'080dU) {
			const unsigned source = (instruction >> 5) & 31U;
			const auto source_offset = source == 31U ?
				offsetof(CpuState, gadget_zero) : source * 8U;
			const bool native = !options.force_software_flags &&
				host_capabilities().has(ArmFeature::flag_m);
			if ((instruction & 0x4000U) != 0)
				emit(words, native ? uml_arm64_gadget_setf_native_16 :
					uml_arm64_gadget_setf_fast_16);
			else
				emit(words, native ? uml_arm64_gadget_setf_native_8 :
					uml_arm64_gadget_setf_fast_8);
			emit(words, source_offset);
		} else if ((instruction & 0xffe0'0400U) == 0xba00'0400U ||
			   (instruction & 0xffff'0800U) == 0x3a00'0800U) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state}, true, address, instruction};
		} else if ((instruction & 0xffe0'fc00U) == 0x9ac0'3000U) {
			if (!options.enable_pointer_authentication) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const unsigned destination = instruction & 31U;
			const unsigned source = (instruction >> 5) & 31U;
			const unsigned modifier = (instruction >> 16) & 31U;
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				emit(words, source == 31U ?
					uml_arm64_gadget_pauth_pacga_z :
					uml_arm64_gadget_pauth_pacga_r);
				emit(words, pointer_auth_generic_operand(
					destination, source, modifier));
			}
		} else if ((instruction & 0x7fe0'f000U) == 0x1ac0'6000U) {
			const unsigned destination = instruction & 31U;
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				const unsigned left = (instruction >> 5) & 31U;
				const unsigned right = (instruction >> 16) & 31U;
				const unsigned operation =
					(instruction >> 10) & 3U;
				const bool is_64_bit =
					(instruction & 0x8000'0000U) != 0;
				const auto zero = offsetof(CpuState, gadget_zero);
				const auto left_offset = left == 31U ? zero : left * 8U;
				const auto right_offset = right == 31U ? zero : right * 8U;
				std::optional<unsigned> copy_source;
				if (left == right)
					copy_source = left;
				else if (operation == 1U) {
					if (left == 31U)
						copy_source = right;
					else if (right == 31U)
						copy_source = left;
				}
				else if (operation == 3U &&
					 (left == 31U || right == 31U))
					copy_source = 31U;
				if (copy_source) {
					const auto source_offset =
						*copy_source == 31U ? zero :
							*copy_source * 8U;
					if (is_64_bit &&
					    destination == *copy_source) {
						emit(words,
							uml_arm64_gadget_nop);
					} else {
						emit(words, is_64_bit ?
							uml_arm64_gadget_integer_min_max_copy_x :
							uml_arm64_gadget_integer_min_max_copy_w);
						emit(words,
							static_cast<std::uintptr_t>(
								destination * 8U) |
							(static_cast<std::uintptr_t>(
								source_offset)
							 << 16));
					}
				} else {
					emit(words, integer_min_max_gadget(
						operation, is_64_bit, false));
					emit(words,
						static_cast<std::uintptr_t>(
							destination * 8U) |
						(static_cast<std::uintptr_t>(
							left_offset) << 16) |
						(static_cast<std::uintptr_t>(
							right_offset) << 32));
				}
			}
		} else if ((instruction & 0x5fe0'f000U) == 0x1ac0'6000U) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state}, true, address, instruction};
		} else if ((instruction & 0xffe0'8000U) == 0x9b60'0000U ||
			   (instruction & 0xffe0'8000U) == 0x9b60'8000U) {
			emit(words, cpa_multiply_gadget(
				(instruction & 0x0000'8000U) != 0,
				((instruction >> 5) & 31U) == 31U,
				((instruction >> 16) & 31U) == 31U,
				((instruction >> 10) & 31U) == 31U,
				(instruction & 31U) == 31U));
			emit(words, cpa_multiply_operand(instruction));
		} else if ((instruction & 0x1f00'0000U) == 0x1b00'0000U) {
			const bool is_64_bit = (instruction >> 31) != 0;
			const auto operation = (instruction >> 21) & 7U;
			const bool valid = ((instruction >> 29) & 3U) == 0 &&
				(operation == 0 || (is_64_bit && (operation == 1 ||
				 operation == 2 || operation == 5 || operation == 6))) &&
				((instruction & 0x0000'8000U) == 0 ||
				 operation == 0 || operation == 1 || operation == 5) &&
				((operation != 2 && operation != 6) ||
				 ((instruction >> 10) & 0x1fU) == 0x1fU);
			if (valid) {
				const unsigned destination = instruction & 31U;
				if (destination == 31U) {
					emit(words, uml_arm64_gadget_nop);
				} else {
					const unsigned left =
						(instruction >> 5) & 31U;
					const unsigned addend =
						(instruction >> 10) & 31U;
					const unsigned right =
						(instruction >> 16) & 31U;
					emit(words, multiply_add_gadget(operation,
						(instruction & 0x8000U) != 0,
						is_64_bit, left == 31U,
						right == 31U, addend == 31U));
					emit(words,
						static_cast<std::uintptr_t>(
							destination * 8U) |
						(static_cast<std::uintptr_t>(
							left * 8U) << 16) |
						(static_cast<std::uintptr_t>(
							right * 8U) << 32) |
						(static_cast<std::uintptr_t>(
							addend * 8U) << 48));
				}
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
		} else if ((instruction & 0x7fe0'f000U) == 0x1ac0'2000U) {
			const unsigned destination = instruction & 31U;
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				emit(words, variable_shift_gadget(
					(instruction & 0x8000'0000U) != 0,
					(instruction >> 10) & 3U,
					left == 31U, right == 31U,
					left == right && left != 31U));
				emit(words,
					arithmetic_binary_operand(instruction));
			}
		} else if ((instruction & 0x7fe0'f800U) == 0x1ac0'0800U) {
			const unsigned destination = instruction & 31U;
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				emit(words, divide_gadget(
					(instruction & 0x8000'0000U) != 0,
					(instruction & 0x400U) != 0,
					left == 31U, right == 31U,
					left == right && left != 31U));
				emit(words,
					arithmetic_binary_operand(instruction));
			}
		} else if ((instruction & 0x7fe0'e000U) == 0x1ac0'4000U) {
			const unsigned size = (instruction >> 10) & 3U;
			if (((instruction >> 31) != 0) == (size == 3U)) {
				const unsigned destination = instruction & 31U;
				const unsigned left = (instruction >> 5) & 31U;
				const unsigned right = (instruction >> 16) & 31U;
				if (destination == 31U) {
					emit(words, uml_arm64_gadget_nop);
				} else {
					emit(words, crc32_gadget(
						(instruction >> 10) & 7U,
						left == 31U, right == 31U,
						left == right && left != 31U,
						!options.force_software_checksum &&
							host_capabilities().crc32));
					if (left != 31U)
						emit(words, offsetof(CpuState, registers) +
							left * 8U);
					if (right != 31U && right != left)
						emit(words, offsetof(CpuState, registers) +
							right * 8U);
					emit(words, offsetof(CpuState, registers) +
						destination * 8U);
				}
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0x5fe0'0000U) == 0x1ac0'0000U) {
			/* Every allocated data-processing (2 source) member is selected
			 * above. The remaining opcode combinations are reserved.
			 */
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0x1f80'0000U) == 0x1380'0000U) {
			const bool is_64_bit = (instruction >> 31) != 0;
			const unsigned shift = (instruction >> 10) & 0x3fU;
			if (((instruction >> 29) & 3U) == 0U &&
			    (instruction & 0x0020'0000U) == 0U &&
			    ((instruction >> 22) & 1U) ==
				    static_cast<unsigned>(is_64_bit) &&
			    (is_64_bit || shift < 32U)) {
				const unsigned destination = instruction & 31U;
				const unsigned left = (instruction >> 5) & 31U;
				const unsigned right = (instruction >> 16) & 31U;
				emit(words, extract_register_gadget(is_64_bit,
					shift != 0U, left == 31U, right == 31U,
					destination == 31U));
				emit(words, destination |
					(static_cast<std::uintptr_t>(left) << 8) |
					(static_cast<std::uintptr_t>(right) << 16) |
					(static_cast<std::uintptr_t>(shift) << 24));
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0x1f00'0000U) == 0x0a00'0000U) {
			const bool is_64_bit = (instruction >> 31) != 0;
			const unsigned shift = (instruction >> 10) & 0x3fU;
			if (!is_64_bit && shift >= 32) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const unsigned destination = instruction & 31U;
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			const unsigned operation = (instruction >> 29) & 3U;
			if (destination == 31U && operation != 3U) {
				/* A non-flag-setting write to XZR/WZR has no effect. */
				emit(words, uml_arm64_gadget_nop);
			} else {
				const unsigned shift_kind = shift == 0U ? 0U :
					((instruction >> 22) & 3U) + 1U;
				emit(words, logical_shifted_gadget(is_64_bit,
					operation,
					(instruction & 0x0020'0000U) != 0,
					shift_kind, left == 31U, right == 31U,
					destination == 31U));
				emit(words, destination |
					(static_cast<std::uintptr_t>(left) << 8) |
					(static_cast<std::uintptr_t>(right) << 16) |
					(static_cast<std::uintptr_t>(shift) << 24));
			}
		} else if ((instruction & 0x3fe0'0000U) == 0x1a80'0000U) {
			if ((instruction & 0x0000'0800U) != 0) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const unsigned destination = instruction & 31U;
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				const unsigned left = (instruction >> 5) & 31U;
				const unsigned right = (instruction >> 16) & 31U;
				const unsigned operation = ((instruction >> 29) & 2U) |
					((instruction >> 10) & 1U);
				const unsigned condition =
					(instruction >> 12) & 15U;
				const bool unconditional_copy =
					condition >= 14U ||
					(operation == 0U && left == right);
				const auto zero = offsetof(CpuState, gadget_zero);
				const auto left_offset = left == 31U ? zero : left * 8U;
				const auto right_offset = right == 31U ? zero : right * 8U;
				emit(words, conditional_select_gadget(
					unconditional_copy ? 14U : condition,
					operation,
					(instruction & 0x8000'0000U) != 0));
				emit(words,
					static_cast<std::uintptr_t>(destination * 8U) |
					(static_cast<std::uintptr_t>(left_offset) << 16) |
					(static_cast<std::uintptr_t>(
						unconditional_copy ? 0U :
							right_offset)
					 << 32));
			}
		} else if ((instruction & 0x3fe0'0000U) == 0x3a40'0000U) {
			if ((instruction & 0x0000'0410U) != 0) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const bool immediate = (instruction & 0x800U) != 0;
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			const auto zero = offsetof(CpuState, gadget_zero);
			const auto left_operand = left == 31U ? zero : left * 8U;
			const auto right_operand = immediate ? right :
				right == 31U ? zero : right * 8U;
			emit(words, conditional_compare_gadget(
				(instruction >> 12) & 15U,
				(instruction & 0x4000'0000U) != 0,
				(instruction & 0x8000'0000U) != 0, immediate));
			emit(words, static_cast<std::uintptr_t>(left_operand) |
				(static_cast<std::uintptr_t>(right_operand) << 16) |
				(static_cast<std::uintptr_t>(instruction & 15U) << 60));
		} else if ((instruction & 0x1f80'0000U) == 0x1200'0000U) {
			const bool is_64_bit = (instruction >> 31) != 0;
			const auto immediate = detail::decode_logical_immediate(
				is_64_bit, (instruction >> 22) & 1U,
				(instruction >> 16) & 0x3fU,
				(instruction >> 10) & 0x3fU);
			if (!immediate) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const unsigned destination = instruction & 31U;
			const unsigned source = (instruction >> 5) & 31U;
			emit(words, logical_immediate_gadget(is_64_bit,
				(instruction >> 29) & 3U, source == 31U,
				destination == 31U));
			emit(words, destination |
				(static_cast<std::uintptr_t>(source) << 8));
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
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const unsigned destination = instruction & 31U;
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				const unsigned source = (instruction >> 5) & 31U;
				emit(words, bitfield_gadget(is_64_bit, operation,
					rotate != 0U, source == 31U));
				emit(words,
					static_cast<std::uintptr_t>(destination * 8U) |
					(static_cast<std::uintptr_t>(source * 8U) << 16) |
					(static_cast<std::uintptr_t>(rotate) << 32) |
					(static_cast<std::uintptr_t>(imms) << 40));
				emit(words, masks->write);
				emit(words, masks->test);
			}
		} else if ((instruction & 0xffff'c000U) == 0xdac1'0000U) {
			if (!options.enable_pointer_authentication) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const unsigned encoded_operation = (instruction >> 10) & 15U;
			const bool zero_modifier = encoded_operation >= 8U;
			const unsigned modifier = (instruction >> 5) & 31U;
			const unsigned destination = instruction & 31U;
			/* PAC*Z/AUT*Z reserve every encoding whose nominal Rn field is
			 * not 11111. Reject it before applying the XZR-destination fast
			 * path so malformed guest code cannot hide invalid state. */
			if (zero_modifier && modifier != 31U) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				emit(words, pointer_auth_gadgets[encoded_operation & 7U]
					[zero_modifier]);
				emit(words, zero_modifier ?
					destination * sizeof(std::uint64_t) :
					pointer_auth_operand(destination, modifier));
			}
		} else if ((instruction & 0xffff'f800U) == 0xdac1'4000U) {
			if (!options.enable_pointer_authentication) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			/* XPACI/XPACD require Rn==11111. D and Rd are the only
			 * variable fields; discard-to-XZR is side-effect free. */
			if (((instruction >> 5) & 31U) != 31U) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			if ((instruction & 31U) == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				emit(words, (instruction & 0x400U) != 0 ?
					uml_arm64_gadget_xpacd : uml_arm64_gadget_xpaci);
				emit(words, static_cast<std::uintptr_t>(
					(instruction & 31U) * sizeof(std::uint64_t)));
			}
		} else if ((instruction & 0x7fff'8000U) == 0x5ac0'0000U) {
			const bool is_64_bit = (instruction >> 31) != 0;
			const auto operation = (instruction >> 10) & 0x1fU;
			if (operation > 8 || (!is_64_bit && operation == 3)) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const unsigned destination = instruction & 31U;
			const unsigned source = (instruction >> 5) & 31U;
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				emit(words, bit_reverse_count_gadget(
					is_64_bit, operation));
				emit(words, source == 31U ?
					offsetof(CpuState, gadget_zero) :
					offsetof(CpuState, registers) +
						source * sizeof(std::uint64_t));
				emit(words, offsetof(CpuState, registers) +
					destination * sizeof(std::uint64_t));
			}
		} else {
			emit_fatal(words, address);
		}
		goto instruction_decoded;

decode_floating_simd:
		if ((instruction & 0xffff'fc00U) == 0x1e7e'0000U) {
			if (!host_capabilities().has(ArmFeature::jscvt)) {
				emit(words,
					uml_arm64_gadget_floating_javascript_convert_software_fallback);
			} else {
				emit(words, (instruction & 31U) == 31U ?
					uml_arm64_gadget_floating_javascript_convert_discard :
					uml_arm64_gadget_floating_javascript_convert);
			}
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>(
					(instruction >> 5) & 31U) << 8));
		} else if (const auto integer_to_floating =
				detail::decode_integer_to_floating(instruction);
			   integer_to_floating.recognized) {
			if (!integer_to_floating.valid) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, integer_to_floating_gadget(integer_to_floating,
				integer_to_floating.source_gpr &&
				((instruction >> 5) & 31U) == 31U));
			emit(words, detail::integer_to_floating_operand(
				instruction, integer_to_floating));
		} else if (const auto floating_to_integer =
				detail::decode_floating_to_integer(instruction);
			   floating_to_integer.recognized) {
			if (!floating_to_integer.valid) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, floating_to_integer_gadget(floating_to_integer,
				floating_to_integer.destination_gpr &&
				(instruction & 31U) == 31U));
			emit(words, detail::floating_to_integer_operand(
				instruction, floating_to_integer));
		} else if ((instruction & 0xff20'fc00U) == 0x1e20'2000U) {
			const auto type = (instruction >> 22) & 3U;
			const bool zero = (instruction & 8U) != 0U;
			if (type == 2U || (instruction & 7U) != 0U ||
			    (zero && ((instruction >> 16) & 31U) != 0U)) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, floating_compare_gadget(
				type, (instruction & 16U) != 0U, zero));
			emit(words,
				(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
				(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16));
		} else if ((instruction & 0xff20'0c00U) == 0x1e20'0400U) {
			const auto type = (instruction >> 22) & 3U;
			if (type == 2U) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, floating_conditional_compare_gadget(
				(instruction >> 12) & 15U, type, (instruction & 16U) != 0U));
			emit(words, (instruction & 15U) |
				(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
				(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16));
		} else if (const auto fp8 = detail::decode_fp8(instruction);
			   fp8.recognized) {
			if (!fp8.valid) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			const auto selected = fp8_gadget(fp8);
			emit(words, selected);
			if (selected == uml_arm64_gadget_fp8_software)
				emit(words, reinterpret_cast<std::uintptr_t>(
					software::fp8_software_function_for(fp8)));
			emit(words, software::fp8_software_operand(fp8));
		} else if (const auto bfloat =
				detail::decode_bfloat_conversion(instruction);
			   bfloat.recognized) {
			emit(words, bfloat_conversion_gadget(bfloat));
			constexpr auto vector_base = offsetof(CpuState, vectors);
			constexpr auto vector_size = sizeof(VectorRegister);
			static_assert(vector_base + 31U * vector_size <= 0xffffU);
			const auto destination = vector_base +
				(instruction & 31U) * vector_size;
			const auto source = vector_base +
				((instruction >> 5) & 31U) * vector_size;
			emit(words, destination |
				(static_cast<std::uintptr_t>(source) << 16U) |
				(static_cast<std::uintptr_t>(bfloat.scalar) << 32U) |
				(static_cast<std::uintptr_t>(bfloat.upper) << 33U));
		} else if (const auto bfloat_dot = detail::decode_bfloat_dot(instruction);
			   bfloat_dot.recognized) {
			emit(words, bfloat_dot_gadget(bfloat_dot));
			emit(words, simd_ternary_operand(
				instruction & 31U, (instruction >> 5) & 31U,
				(instruction >> 16) & 31U,
				static_cast<unsigned>(bfloat_dot.by_element) |
					(static_cast<unsigned>(bfloat_dot.wide) << 1U) |
					(bfloat_dot.index << 2U)));
		} else if (detail::decode_bfloat_matrix_multiply(instruction)) {
			emit(words, bfloat_matrix_gadget());
			emit(words, simd_ternary_operand(
				instruction & 31U, (instruction >> 5) & 31U,
				(instruction >> 16) & 31U));
		} else if (const auto bfloat_multiply =
				detail::decode_bfloat_multiply_long(instruction);
			   bfloat_multiply.recognized) {
			emit(words, bfloat_multiply_long_gadget(bfloat_multiply));
			emit(words, simd_ternary_operand(
				instruction & 31U, (instruction >> 5) & 31U,
				(instruction >> 16) &
					(bfloat_multiply.by_element ? 15U : 31U),
				static_cast<unsigned>(bfloat_multiply.top) |
					(static_cast<unsigned>(
						bfloat_multiply.by_element) << 1U) |
					(bfloat_multiply.index << 2U)));
		} else if (((instruction & 0xbf20'fc00U) ==
				    0x2e00'fc00U &&
			    ((((instruction >> 22) & 3U) == 0U) ||
			     (((instruction >> 22) & 3U) == 2U))) ||
			   ((instruction & 0xbf20'fc00U) ==
				    0x2e00'ec00U &&
			    ((((instruction & 0x4000'0000U) == 0U) &&
			      (((instruction >> 22) & 3U) == 1U)) ||
			     (((instruction & 0x4000'0000U) != 0U) &&
			      (((instruction >> 22) & 3U) == 3U))))) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state}, true, address, instruction};
		} else if (const auto half_fused =
				detail::decode_half_fused_long(instruction);
			   half_fused.recognized) {
			emit(words, half_fused_long_gadget(half_fused));
			emit(words, simd_ternary_operand(
				instruction & 31U, (instruction >> 5) & 31U,
				(instruction >> 16) &
					(half_fused.by_element ? 15U : 31U),
				static_cast<unsigned>(half_fused.by_element) |
					(static_cast<unsigned>(half_fused.subtract)
						<< 1U) |
					(static_cast<unsigned>(half_fused.upper)
						<< 2U) |
					(static_cast<unsigned>(half_fused.wide)
						<< 3U) |
					(half_fused.index << 4U)));
		} else if (detail::reserved_half_fused_long(instruction)) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state}, true, address, instruction};
		} else if (const auto half_matrix = detail::decode_half_matrix(instruction);
			   half_matrix.recognized) {
			if (!half_matrix.valid) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, half_matrix_gadget(half_matrix));
			emit(words, simd_ternary_operand(
				instruction & 31U, (instruction >> 5) & 31U,
				(instruction >> 16) & 31U,
				static_cast<unsigned>(half_matrix.widening)));
		} else if (const auto half_dot = detail::decode_half_dot(instruction);
			   half_dot.recognized) {
			emit(words, half_dot_gadget(half_dot));
			emit(words, simd_ternary_operand(
				instruction & 31U, (instruction >> 5) & 31U,
				(instruction >> 16) & 31U,
				static_cast<unsigned>(half_dot.by_element) |
					(static_cast<unsigned>(half_dot.wide) << 1U) |
					(half_dot.index << 2U)));
			} else if (detail::reserved_half_dot_scalar_class(instruction)) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state}, true, address, instruction};
		} else if ((instruction & 0xff20'7c00U) == 0x1e20'4000U &&
			   ((instruction >> 15) & 0x3fU) >= 4U &&
			   ((instruction >> 15) & 0x3fU) <= 7U) {
			const auto source_type = (instruction >> 22) & 3U;
			const auto destination_type = (instruction >> 15) & 3U;
			if (source_type == 2U || destination_type == 2U ||
			    source_type == destination_type) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, floating_convert_precision_gadget(
				source_type, destination_type));
			emit(words, simd_unary_operand(instruction & 31U,
				(instruction >> 5) & 31U));
		} else if ((instruction & 0xff20'1fe0U) == 0x1e20'1000U) {
			const auto type = (instruction >> 22) & 3U;
			if (type == 2U) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			const auto immediate = static_cast<std::uint8_t>(
				(instruction >> 13) & 0xffU);
			const unsigned bitsize = type == 3U ? 16U : type == 0U ? 32U : 64U;
			const auto bits = expand_vfp_immediate(immediate, bitsize);
			emit(words, uml_arm64_gadget_floating_move_immediate);
			emit(words, instruction & 31U);
			emit(words, bits);
		} else if (const auto floating_complex =
				detail::decode_floating_complex(instruction);
			   floating_complex.recognized) {
			const auto gadget = floating_complex.valid ?
				floating_complex_gadget(floating_complex) : nullptr;
			if (gadget == nullptr) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, gadget);
			emit(words, floating_complex_operand(
				instruction, floating_complex));
		} else if (const auto floating_scale =
				detail::decode_floating_scale(instruction);
			   floating_scale.recognized) {
			if (!floating_scale.valid) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, floating_scale_gadget(floating_scale));
			emit(words, detail::floating_scale_operand(
				instruction, floating_scale));
		} else if (const auto floating_reciprocal =
				detail::decode_floating_reciprocal(instruction);
			   floating_reciprocal.recognized) {
			const auto gadget = floating_reciprocal.valid ?
				floating_reciprocal_gadget(floating_reciprocal) : nullptr;
			if (gadget == nullptr) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, gadget);
			emit(words, detail::floating_reciprocal_operand(
				instruction, floating_reciprocal));
		} else if (const auto floating_horizontal =
				detail::decode_floating_horizontal(instruction);
			   floating_horizontal.recognized) {
			const auto gadget = floating_horizontal.valid ?
				floating_horizontal_gadget(floating_horizontal) : nullptr;
			if (gadget == nullptr) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, gadget);
			const bool absolute_software =
				(floating_horizontal.operation ==
					detail::FloatingHorizontalOperation::
						absolute_maximum ||
				 floating_horizontal.operation ==
					detail::FloatingHorizontalOperation::
						absolute_minimum) &&
				!host_has_faminmax();
			const bool fp_controls_checked =
				!host_has_afp() &&
				(floating_horizontal.operation ==
						detail::FloatingHorizontalOperation::
							absolute_difference ||
				 floating_horizontal.operation ==
						detail::FloatingHorizontalOperation::add ||
				 detail::floating_horizontal_uses_ah(
						floating_horizontal));
			if (absolute_software) {
				emit(words, detail::floating_horizontal_operand(
					instruction, floating_horizontal));
			} else {
				const auto vector_offset = [](unsigned index) {
					return offsetof(CpuState, vectors) +
						index * sizeof(
							uml::cpu::arm64::
								VectorRegister);
				};
				emit(words, vector_offset(instruction & 31U));
				emit(words, vector_offset(
					(instruction >> 5) & 31U));
				const bool uses_second =
					floating_horizontal.form ==
						detail::FloatingHorizontalForm::
							scalar_binary ||
				    floating_horizontal.form ==
						detail::FloatingHorizontalForm::
							vector_binary ||
				    floating_horizontal.form ==
						detail::FloatingHorizontalForm::
							vector_pairwise;
				if (uses_second)
					emit(words, vector_offset(
						(instruction >> 16) & 31U));
				if (fp_controls_checked)
					emit(words,
						detail::floating_horizontal_operand(
							instruction,
							floating_horizontal));
			}
		} else if (const auto floating_round =
				detail::decode_floating_round(instruction);
			   floating_round.recognized) {
			if (!floating_round.valid) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, floating_round_gadget(floating_round));
			emit(words, detail::is_sized_round(floating_round.operation) ?
				detail::sized_floating_round_operand(
					instruction, floating_round) :
				detail::floating_round_operand(instruction));
		} else if (const auto fused_floating =
				detail::decode_fused_floating_arithmetic(instruction);
			   fused_floating.recognized) {
			const auto gadget = fused_floating.valid ?
				fused_floating_arithmetic_gadget(fused_floating) : nullptr;
			if (gadget == nullptr) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, gadget);
			const auto vector_offset = [](unsigned index) {
				return offsetof(CpuState, vectors) +
					index * sizeof(VectorRegister);
			};
			emit(words, vector_offset(instruction & 31U));
			emit(words, vector_offset((instruction >> 5) & 31U));
			emit(words, vector_offset((instruction >> 16) & 31U));
			if (fused_floating.scalar)
				emit(words, vector_offset(
					(instruction >> 10) & 31U));
		} else if (const auto basic_floating =
				detail::decode_basic_floating_arithmetic(instruction);
			   basic_floating.recognized) {
			const auto gadget = basic_floating.valid ?
				basic_floating_arithmetic_gadget(basic_floating) : nullptr;
			if (gadget == nullptr) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, gadget);
			if (basic_floating.scalar) {
				emit(words, detail::basic_floating_operand(instruction));
			} else {
				const auto vector_offset = [](unsigned index) {
					return offsetof(CpuState, vectors) +
						index * sizeof(VectorRegister);
				};
				emit(words,
					vector_offset(instruction & 31U) |
					(static_cast<std::uintptr_t>(vector_offset(
						(instruction >> 5) & 31U)) << 16) |
					(static_cast<std::uintptr_t>(vector_offset(
						(instruction >> 16) & 31U)) << 32));
			}
		} else if ((instruction & 0xff20'7c00U) == 0x1e20'4000U ||
			   (instruction & 0xff20'0c00U) == 0x1e20'0800U) {
			const auto type = (instruction >> 22) & 3U;
			const bool one_source =
				(instruction & 0xff20'7c00U) == 0x1e20'4000U;
			const auto operation = one_source ?
				(instruction >> 15) & 0x3fU :
				(instruction >> 12) & 0xfU;
			if (type == 2U) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			const bool native =
				(type <= 1U && (one_source ? operation <= 3U :
					operation <= 3U || operation == 8U)) ||
				(type == 3U && one_source &&
					(operation <= 2U));
			if (native) {
				emit(words, floating_arithmetic_gadget(
					type, one_source, operation));
				emit(words, (instruction & 31U) |
					(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
					(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16));
			} else {
				emit_fatal(words, address);
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
		} else if ((instruction & 0xbf20'fc00U) == 0x0e20'e000U) {
			const auto gadget = simd_pmull_gadget(
				(instruction & 0x4000'0000U) != 0,
				(instruction >> 22) & 3U,
				options.force_software_pmull);
			if (gadget == nullptr) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, gadget);
			constexpr auto vector_base = offsetof(CpuState, vectors);
			constexpr auto vector_size =
				sizeof(uml::cpu::arm64::VectorRegister);
			static_assert(vector_base + 31U * vector_size <= 0xffffU);
			const auto destination = vector_base +
				(instruction & 31U) * vector_size;
			const auto left = vector_base +
				((instruction >> 5) & 31U) * vector_size;
			const auto right = vector_base +
				((instruction >> 16) & 31U) * vector_size;
			emit(words, destination | (left << 16) | (right << 32));
		} else if ((instruction & 0xffe0'8000U) == 0xce00'0000U ||
			   (instruction & 0xffe0'8000U) == 0xce20'0000U) {
			const bool native =
				!options.force_software_sha3 && host_has_sha3();
			const bool bit_clear =
				(instruction & 0x0020'0000U) != 0;
			emit(words, bit_clear ?
				(native ? uml_arm64_gadget_simd_bcax_native :
				 uml_arm64_gadget_simd_bcax_soft) :
				(native ? uml_arm64_gadget_simd_eor3_native :
				 uml_arm64_gadget_simd_eor3_soft));
			const auto vector_offset = [](unsigned index) {
				return offsetof(CpuState, vectors) +
					index * sizeof(
						uml::cpu::arm64::VectorRegister);
			};
			emit(words, vector_offset(instruction & 31U));
			emit(words, vector_offset((instruction >> 5) & 31U));
			emit(words, vector_offset((instruction >> 16) & 31U));
			emit(words, vector_offset((instruction >> 10) & 31U));
		} else if ((instruction & 0xffe0'fc00U) == 0xce60'8c00U) {
			emit(words, !options.force_software_sha3 &&
					host_has_sha3() ?
				uml_arm64_gadget_simd_rax1_native :
				uml_arm64_gadget_simd_rax1_soft);
			const auto vector_offset = [](unsigned index) {
				return offsetof(CpuState, vectors) +
					index * sizeof(
						uml::cpu::arm64::VectorRegister);
			};
			emit(words, vector_offset(instruction & 31U));
			emit(words, vector_offset((instruction >> 5) & 31U));
			emit(words, vector_offset((instruction >> 16) & 31U));
		} else if ((instruction & 0xffe0'0000U) == 0xce80'0000U) {
			emit(words, simd_xar_gadget(
				(instruction >> 10) & 63U,
				!options.force_software_sha3 &&
					host_has_sha3()));
			const auto vector_offset = [](unsigned index) {
				return offsetof(CpuState, vectors) +
					index * sizeof(
						uml::cpu::arm64::VectorRegister);
			};
			emit(words, vector_offset(instruction & 31U));
			emit(words, vector_offset((instruction >> 5) & 31U));
			emit(words, vector_offset((instruction >> 16) & 31U));
		} else if ((instruction & 0xffff'8c00U) == 0x4e28'0800U) {
			const auto gadget = simd_aes_gadget(
				(instruction >> 12) & 7U,
				options.force_software_aes);
			if (gadget == nullptr) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
                                return {Program{guest_address, compiled,
                                                std::move(words),
                                                program_accesses_fp_state},
                                        true, address, instruction};
                        }
                        emit(words, gadget);
                        const auto vector_offset = [](unsigned index) {
                          return offsetof(CpuState, vectors) +
                                 index *
                                     sizeof(
                                         uml::cpu::arm64::VectorRegister);
                        };
                        emit(words, vector_offset(instruction & 31U));
                        emit(words, vector_offset((instruction >> 5) & 31U));
                } else if ((instruction & 0xffe0'8c00U) == 0x5e00'0000U) {
                  const auto gadget = simd_sha_three_gadget(
                      (instruction >> 12) & 7U,
                      options.force_software_fixed_transforms);
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U));
                  emit(words, vector_offset((instruction >> 5) & 31U));
                  emit(words, vector_offset((instruction >> 16) & 31U));
                } else if ((instruction & 0xffff'8c00U) == 0x5e28'0800U) {
                  const auto gadget = simd_sha_two_gadget(
                      (instruction >> 12) & 3U,
                      options.force_software_fixed_transforms);
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U));
                  emit(words, vector_offset((instruction >> 5) & 31U));
                } else if ((instruction & 0xffe0'fc00U) == 0xce60'8000U ||
                           (instruction & 0xffe0'fc00U) == 0xce60'8400U ||
                           (instruction & 0xffe0'fc00U) == 0xce60'8800U) {
                  emit(words, simd_sha512_three_gadget(
                                  instruction,
                                  options.force_software_fixed_transforms));
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U));
                  emit(words, vector_offset((instruction >> 5) & 31U));
                  emit(words, vector_offset((instruction >> 16) & 31U));
                } else if ((instruction & 0xffff'fc00U) == 0xcec0'8000U) {
                  emit(words, !options.force_software_fixed_transforms &&
                                      host_has_sha512()
                                  ? uml_arm64_gadget_simd_sha512su0
                                  : uml_arm64_gadget_simd_sha512su0_soft);
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U));
                  emit(words, vector_offset((instruction >> 5) & 31U));
                } else if ((instruction & 0xffe0'fc00U) == 0xce60'c000U ||
                           (instruction & 0xffe0'fc00U) == 0xce60'c400U ||
                           (instruction & 0xffe0'fc00U) == 0xce60'c800U) {
                  const auto operation = instruction & 0xffe0'fc00U;
                  const auto gadget =
                      operation == 0xce60'c000U
                          ? (!options.force_software_fixed_transforms &&
                                     host_has_sm3()
                                 ? uml_arm64_gadget_simd_sm3partw1
                                 : uml_arm64_gadget_simd_sm3partw1_soft)
                      : operation == 0xce60'c400U
                          ? (!options.force_software_fixed_transforms &&
                                     host_has_sm3()
                                 ? uml_arm64_gadget_simd_sm3partw2
                                 : uml_arm64_gadget_simd_sm3partw2_soft)
                          : (!options.force_software_fixed_transforms &&
                                     host_has_sm4()
                                 ? uml_arm64_gadget_simd_sm4ekey
                                 : uml_arm64_gadget_simd_sm4ekey_soft);
                  emit(words, gadget);
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U));
                  emit(words, vector_offset((instruction >> 5) & 31U));
                  emit(words, vector_offset((instruction >> 16) & 31U));
                } else if ((instruction & 0xffe0'8000U) == 0xce40'0000U) {
                  emit(words, !options.force_software_fixed_transforms &&
                                      host_has_sm3()
                                  ? uml_arm64_gadget_simd_sm3ss1
                                  : uml_arm64_gadget_simd_sm3ss1_soft);
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U));
                  emit(words, vector_offset((instruction >> 5) & 31U));
                  emit(words, vector_offset((instruction >> 16) & 31U));
                  emit(words, vector_offset((instruction >> 10) & 31U));
                } else if ((instruction & 0xffe0'c000U) == 0xce40'8000U) {
                  emit(words,
                       simd_sm3tt_gadget(
                           (instruction >> 10) & 3U, (instruction >> 12) & 3U,
                           options.force_software_fixed_transforms));
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U));
                  emit(words, vector_offset((instruction >> 5) & 31U));
                  emit(words, vector_offset((instruction >> 16) & 31U));
                } else if ((instruction & 0xffff'fc00U) == 0xcec0'8400U) {
                  emit(words, !options.force_software_fixed_transforms &&
                                      host_has_sm4()
                                  ? uml_arm64_gadget_simd_sm4e
                                  : uml_arm64_gadget_simd_sm4e_soft);
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U));
                  emit(words, vector_offset((instruction >> 5) & 31U));
                } else if ((instruction & 0xffe0'fc00U) == 0xce60'cc00U ||
                           (instruction & 0xffe0'8000U) == 0xce60'0000U ||
                           (instruction & 0xffff'fc00U) == 0xcec0'8800U ||
                           (instruction & 0xffff'fc00U) == 0xcec0'8c00U) {
                  emit(words, uml_arm64_gadget_illegal);
                  emit(words, address);
                  ++compiled;
                  return {Program{guest_address, compiled, std::move(words),
                                  program_accesses_fp_state},
                          true, address, instruction};
                } else if ((instruction & 0xbf60'fc00U) == 0x0e40'1c00U ||
                           (instruction & 0xff60'fc00U) == 0x5e40'1c00U ||
                           (instruction & 0xbf20'fc00U) == 0x0e20'dc00U ||
                           (instruction & 0xff20'fc00U) == 0x5e20'dc00U) {
                  const bool scalar = (instruction & 0x1000'0000U) != 0;
                  const bool q = (instruction & 0x4000'0000U) != 0;
                  const bool half =
                      (instruction & 0x00e0'0000U) == 0x0040'0000U;
                  const auto gadget = simd_fmulx_gadget(
                      scalar, q, half,
                      !half && (instruction & 0x0040'0000U) != 0);
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>((instruction >> 16) &
                                                       31U)
                           << 16));
                } else if ((instruction & 0x9f20'fc00U) == 0x0e00'9400U ||
                           (instruction & 0xbf20'fc00U) == 0x0e00'9c00U ||
                           (instruction & 0xdf20'fc00U) == 0x5e00'9400U ||
                           (instruction & 0xff20'fc00U) == 0x5e00'9c00U) {
                  const bool scalar = (instruction & 0x1000'0000U) != 0U;
                  const bool ordinary =
                      (instruction & 0x9f20'fc00U) == 0x0e00'9400U;
                  const auto signedness =
                      ordinary ? ((instruction & 0x2000'0000U) != 0
                                      ? SimdDotSignedness::unsigned_unsigned
                                      : SimdDotSignedness::signed_signed)
                               : SimdDotSignedness::unsigned_signed;
                  const auto gadget =
                      !scalar && ((instruction >> 22) & 3U) == 2U
                          ? simd_dot_vector_gadget(
                                signedness, (instruction & 0x4000'0000U) != 0,
                                ordinary ? options.force_software_dotprod
                                         : options.force_software_i8mm)
                          : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(uml::cpu::arm64::VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U));
                  emit(words, vector_offset((instruction >> 5) & 31U));
                  emit(words, vector_offset((instruction >> 16) & 31U));
                } else if ((instruction & 0x9f20'f400U) == 0x0e00'a400U) {
                  const bool q = (instruction & 0x4000'0000U) != 0;
                  const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
                  const unsigned size = (instruction >> 22) & 3U;
                  const bool alternate = (instruction & 0x800U) != 0;
                  if (!q || size != 2U || (unsigned_group && alternate)) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  const bool mixed = alternate;
                  const auto signedness =
                      mixed            ? SimdDotSignedness::unsigned_signed
                      : unsigned_group ? SimdDotSignedness::unsigned_unsigned
                                       : SimdDotSignedness::signed_signed;
                  emit(words, simd_matrix_gadget(signedness,
                                                 options.force_software_i8mm));
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U));
                  emit(words, vector_offset((instruction >> 5) & 31U));
                  emit(words, vector_offset((instruction >> 16) & 31U));
                } else if ((instruction & 0xbf20'f400U) == 0x2e00'8400U ||
                           (instruction & 0xff20'f400U) == 0x7e00'8400U) {
                  const bool scalar = (instruction & 0x1000'0000U) != 0;
                  const auto gadget = simd_rdm_three_same_gadget(
                      (instruction & 0x800U) != 0, scalar,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  if (!host_has_rdm()) {
                    emit(words, uml_arm64_gadget_simd_rdm_software);
                    emit(words, instruction);
                  } else {
                    emit(words, gadget);
                    const auto vector_offset = [](unsigned index) {
                      return offsetof(CpuState, vectors) +
                             index * sizeof(VectorRegister);
                    };
                    emit(words, vector_offset(instruction & 31U));
                    emit(words, vector_offset((instruction >> 5) & 31U));
                    emit(words, vector_offset((instruction >> 16) & 31U));
                  }
                } else if (const auto floating_conversion =
                               detail::decode_simd_floating_precision_vector(
                                   instruction);
                           floating_conversion.recognized) {
                  if (!floating_conversion.valid) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, simd_floating_precision_vector_gadget(
                                  floating_conversion));
                  emit(words, detail::simd_floating_precision_vector_operand(
                                  instruction, floating_conversion));
                } else if (const auto floating_compare =
                               detail::decode_simd_floating_compare(
                                   instruction);
                           floating_compare.recognized) {
                  if (!floating_compare.valid) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, simd_floating_compare_gadget(floating_compare));
                  emit(words, detail::simd_floating_compare_operand(
                                  instruction, floating_compare));
                } else if ((((instruction & 0x9f20'0400U) == 0x0e20'0400U) ||
                            ((instruction & 0xdf20'0400U) == 0x5e20'0400U)) &&
                           ((instruction >> 11) & 0x1fU) < 24U &&
                           ((instruction >> 11) & 0x1fU) != 3U) {
                  /* Integer three-same operations are fully validated and
                   * selected while translating. Their native gadgets contain no
                   * opcode or arrangement branches on the valid hot path. */
                  const bool scalar = (instruction & 0x1000'0000U) != 0;
                  const auto gadget = simd_three_same_gadget(
                      (instruction >> 11) & 0x1fU,
                      (instruction & 0x2000'0000U) != 0, scalar,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U) |
                                  (static_cast<std::uintptr_t>(
                                       vector_offset((instruction >> 5) & 31U))
                                   << 16) |
                                  (static_cast<std::uintptr_t>(
                                       vector_offset((instruction >> 16) & 31U))
                                   << 32));
                } else if (((instruction & 0xbf20'fc00U) == 0x0e00'1800U ||
                            (instruction & 0xbf20'fc00U) == 0x0e00'2800U ||
                            (instruction & 0xbf20'fc00U) == 0x0e00'3800U ||
                            (instruction & 0xbf20'fc00U) == 0x0e00'5800U ||
                            (instruction & 0xbf20'fc00U) == 0x0e00'6800U ||
                            (instruction & 0xbf20'fc00U) == 0x0e00'7800U)) {
                  const auto gadget =
                      simd_permute_gadget((instruction >> 12) & 7U,
                                          (instruction & 0x4000'0000U) != 0,
                                          (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U) |
                                  (static_cast<std::uintptr_t>(
                                       vector_offset((instruction >> 5) & 31U))
                                   << 16) |
                                  (static_cast<std::uintptr_t>(
                                       vector_offset((instruction >> 16) & 31U))
                                   << 32));
                } else if ((instruction & 0xff20'8c00U) == 0x4e00'0000U &&
                           (instruction & 0x00c0'0000U) != 0U) {
                  /* FEAT_LUT allocation validity and the native instruction
                   * shape are resolved once here. Native gadgets consequently
                   * have no valid-path operation, element-size, or immediate
                   * branches. */
                  const unsigned op2 = (instruction >> 22) & 3U;
                  const unsigned len = (instruction >> 13) & 3U;
                  const unsigned op = (instruction >> 12) & 1U;
                  unsigned shape{};
                  unsigned part{};
                  bool valid = true;
                  if (op2 == 1U) {
                    valid = op != 0U || (len & 1U) != 0U;
                    shape = op != 0U ? 3U : 2U;
                    part = op != 0U ? len : len >> 1;
                  } else if (op2 == 2U) {
                    valid = op != 0U;
                    shape = 0U;
                    part = len;
                  } else if (op2 == 3U) {
                    shape = 1U;
                    part = (len << 1) | op;
                  } else {
                    valid = false;
                  }
                  const auto native =
                      valid ? simd_lut_native_gadget(shape, part) : nullptr;
                  if (native == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, host_has_lut()
                                  ? native
                                  : uml_arm64_gadget_simd_lut_software);
                  emit(words, simd_lut_operand(instruction, shape, part));
                } else if ((instruction & 0xbfe0'8c00U) == 0x0e00'0000U) {
                  const unsigned count = ((instruction >> 13) & 3U) + 1U;
                  emit(words, simd_table_lookup_gadget(
                                  (instruction & 0x1000U) != 0, count,
                                  (instruction & 0x4000'0000U) != 0));
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  const unsigned first = (instruction >> 5) & 31U;
                  emit(words,
                       vector_offset(instruction & 31U) |
                           (static_cast<std::uintptr_t>(
                                vector_offset((instruction >> 16) & 31U))
                            << 16) |
                           (static_cast<std::uintptr_t>(vector_offset(first))
                            << 32) |
                           (static_cast<std::uintptr_t>(
                                vector_offset((first + 1U) & 31U))
                            << 48));
                  if (count > 2U)
                    emit(words, vector_offset((first + 2U) & 31U) |
                                    (static_cast<std::uintptr_t>(
                                         vector_offset((first + 3U) & 31U))
                                     << 16));
                } else if ((instruction & 0x9ff8'0400U) == 0x0f00'0400U) {
                  /* This allocation overlaps the broad shift-immediate masks
                   * below in cmode bits, so it must be selected first. */
                  const auto decoded =
                      detail::decode_simd_modified_immediate(instruction);
                  if (decoded) {
                    emit(words, simd_modified_immediate_gadget(
                                    decoded->action,
                                    (instruction & 0x4000'0000U) != 0));
                    emit(words, instruction & 31U);
                    emit(words, decoded->pattern);
                  } else {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                  }
                } else if ((instruction & 0x9f80'fc00U) == 0x0f00'a400U) {
                  const auto encoded = (instruction >> 16) & 0x7fU;
                  const auto source_bits = std::bit_floor(encoded);
                  const auto gadget = simd_shift_left_long_gadget(
                      source_bits, (instruction & 0x4000'0000U) != 0,
                      (instruction & 0x2000'0000U) != 0);
                  if (gadget == nullptr ||
                      encoded - source_bits >= source_bits) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(encoded - source_bits)
                           << 16));
                } else if ((((instruction & 0x9f20'0c00U) == 0x0e20'0000U) ||
                            ((instruction & 0xdf20'0c00U) == 0x5e20'0000U)) &&
                           ((instruction >> 12) & 15U) != 4U &&
                           ((instruction >> 12) & 15U) != 6U) {
                  const bool scalar = (instruction & 0x1000'0000U) != 0;
                  const auto gadget = simd_widening_gadget(
                      (instruction >> 12) & 15U,
                      (instruction & 0x2000'0000U) != 0, scalar,
                      !scalar && (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U));
                  emit(words, vector_offset((instruction >> 5) & 31U));
                  emit(words, vector_offset((instruction >> 16) & 31U));
                } else if ((instruction & 0xbfff'fc00U) == 0x0ef8'f800U ||
                           (instruction & 0xbfff'fc00U) == 0x2ef8'f800U ||
                           (instruction & 0xbf3f'fc00U) == 0x0e20'f800U ||
                           (instruction & 0xbf3f'fc00U) == 0x2e20'f800U) {
                  const auto half_operation = instruction & 0xbfff'fc00U;
                  const bool half = half_operation == 0x0ef8'f800U ||
                                    half_operation == 0x2ef8'f800U;
                  const bool negate =
                      half ? half_operation == 0x2ef8'f800U
                           : (instruction & 0xbf3f'fc00U) == 0x2e20'f800U;
                  const bool allocated =
                      half || (instruction & 0x0080'0000U) != 0U;
                  const auto gadget =
                      allocated
                          ? simd_floating_sign_gadget(
                                negate, half,
                                !half && (instruction & 0x0040'0000U) != 0U,
                                (instruction & 0x4000'0000U) != 0U)
                          : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8));
                } else if ((instruction & 0xbf20'fc00U) == 0x0e00'0400U ||
                           (instruction & 0xff20'fc00U) == 0x5e00'0400U) {
                  const auto lane =
                      detail::decode_simd_lane_index((instruction >> 16) & 31U);
                  const auto gadget =
                      lane ? simd_duplicate_element_gadget(
                                 (instruction & 0x1000'0000U) != 0,
                                 (instruction & 0x4000'0000U) != 0,
                                 lane->element_shift)
                           : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(lane->offset) << 16));
                } else if ((instruction & 0x9f00'0400U) == 0x0f00'0000U &&
                           (((instruction >> 12) & 15U) == 14U ||
                            (((instruction >> 12) & 15U) == 15U &&
                             (instruction & 0x2000'0000U) == 0 &&
                             (((instruction >> 22) & 3U) == 0U ||
                              ((instruction >> 22) & 3U) == 2U)))) {
                  const unsigned operation = (instruction >> 12) & 15U;
                  const unsigned size = (instruction >> 22) & 3U;
                  const auto element =
                      decode_simd_dot_indexed_element(instruction);
                  const auto signedness =
                      operation == 14U
                          ? ((instruction & 0x2000'0000U) != 0
                                 ? SimdDotSignedness::unsigned_unsigned
                                 : SimdDotSignedness::signed_signed)
                      : size == 0U ? SimdDotSignedness::signed_unsigned
                                   : SimdDotSignedness::unsigned_signed;
                  const auto gadget =
                      (operation != 14U || size == 2U)
                          ? simd_dot_indexed_gadget(
                                signedness, (instruction & 0x4000'0000U) != 0,
                                element.lane_index,
                                operation == 14U
                                    ? options.force_software_dotprod
                                    : options.force_software_i8mm)
                          : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(uml::cpu::arm64::VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U));
                  emit(words, vector_offset((instruction >> 5) & 31U));
                  emit(words, vector_offset(element.register_index));
                } else if (((instruction & 0x9f00'0400U) == 0x0f00'0000U ||
                            (instruction & 0xdf00'0400U) == 0x5f00'0000U) &&
                           ((((instruction >> 12) & 15U) == 1U &&
                             (instruction & 0x2000'0000U) == 0) ||
                            (((instruction >> 12) & 15U) == 5U &&
                             (instruction & 0x2000'0000U) == 0) ||
                            ((instruction >> 12) & 15U) == 9U)) {
                  /* All decode choices are fixed here.  AFP hosts select a
                   * branch-free native gadget; older hosts select a checked
                   * gadget whose AH=0 path remains native. */
                  const auto element =
                      decode_simd_indexed_floating_element(instruction);
                  const auto gadget =
                      element
                          ? simd_indexed_floating_gadget(
                                (instruction >> 12) & 15U,
                                (instruction & 0x2000'0000U) != 0,
                                (instruction & 0x1000'0000U) != 0,
                                (instruction & 0x4000'0000U) != 0,
                                (instruction >> 22) & 3U, element->lane_index)
                          : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U) |
                                  (static_cast<std::uintptr_t>(
                                       vector_offset((instruction >> 5) & 31U))
                                   << 16) |
                                  (static_cast<std::uintptr_t>(
                                       vector_offset(element->register_index))
                                   << 32));
                  if (!host_has_afp())
                    emit(words, instruction);
                } else if ((((instruction & 0x9f00'0400U) == 0x0f00'0000U) ||
                            ((instruction & 0xdf00'0400U) == 0x5f00'0000U)) &&
                           is_simd_indexed_integer_group(
                               (instruction >> 12) & 15U,
                               (instruction & 0x2000'0000U) != 0,
                               (instruction & 0x1000'0000U) != 0)) {
                  /* Integer by-element choices, including the lane and
                   * restricted element-register field, are resolved once while
                   * translating. */
                  const auto element = decode_simd_indexed_element(instruction);
                  const bool rdm = (instruction & 0x2000'0000U) != 0 &&
                                   (((instruction >> 12) & 15U) == 13U ||
                                    ((instruction >> 12) & 15U) == 15U);
                  const auto gadget =
                      element
                          ? simd_indexed_integer_gadget(
                                (instruction >> 12) & 15U,
                                (instruction & 0x2000'0000U) != 0,
                                (instruction & 0x1000'0000U) != 0,
                                (instruction & 0x4000'0000U) != 0,
                                (instruction >> 22) & 3U, element->lane_index)
                          : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  if (rdm && !host_has_rdm()) {
                    emit(words, uml_arm64_gadget_simd_rdm_software);
                    emit(words, instruction);
                  } else {
                    const auto vector_offset = [](unsigned index) {
                      return offsetof(CpuState, vectors) +
                             index * sizeof(VectorRegister);
                    };
                    emit(words, gadget);
                    emit(words, vector_offset(instruction & 31U));
                    emit(words, vector_offset((instruction >> 5) & 31U));
                    emit(words, vector_offset(element->register_index));
                  }
                } else if ((instruction & 0xdf00'0400U) == 0x5f00'0000U &&
                           is_simd_indexed_scalar_reserved(
                               (instruction >> 12) & 15U,
                               (instruction & 0x2000'0000U) != 0,
                               (instruction >> 22) & 3U)) {
                  /* Reject scalar indexed holes at translation time, before any
                   * architectural register or FP state is observed. */
                  emit(words, uml_arm64_gadget_illegal);
                  emit(words, address);
                  ++compiled;
                  return {Program{guest_address, compiled, std::move(words),
                                  program_accesses_fp_state},
                          true, address, instruction};
                } else if ((((instruction & 0x9f00'0400U) == 0x0f00'0400U) ||
                            ((instruction & 0xdf00'0400U) == 0x5f00'0400U)) &&
                           ((((instruction >> 11) & 31U) <= 8U) &&
                            (((instruction >> 11) & 1U) == 0U))) {
                  const auto decoded =
                      decode_simd_immediate_shift(instruction, false);
                  const unsigned opcode = (instruction >> 11) & 31U;
                  const auto gadget =
                      decoded ? simd_shift_right_immediate_gadget(
                                    opcode, (instruction & 0x2000'0000U) != 0,
                                    decoded->shape)
                              : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(decoded->shift) << 16));
                  if (opcode == 8U)
                    emit(words, simd_shift_insert_mask(decoded->lane_bits,
                                                       decoded->shift, false));
                } else if ((instruction & 0x9f00'fc00U) == 0x0f00'5400U ||
                           (instruction & 0xdf00'fc00U) == 0x5f00'5400U) {
                  const auto decoded =
                      decode_simd_immediate_shift(instruction, true);
                  const bool insert = (instruction & 0x2000'0000U) != 0;
                  const auto gadget =
                      decoded ? simd_shift_left_immediate_gadget(insert,
                                                                 decoded->shape)
                              : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(decoded->shift) << 16));
                  if (insert)
                    emit(words, simd_shift_insert_mask(decoded->lane_bits,
                                                       decoded->shift, true));
                } else if ((instruction & 0x9f00'fc00U) == 0x0f00'7400U ||
                           (instruction & 0xdf00'fc00U) == 0x5f00'7400U ||
                           (instruction & 0x9f00'fc00U) == 0x0f00'6400U ||
                           (instruction & 0xdf00'fc00U) == 0x5f00'6400U) {
                  const auto decoded =
                      decode_simd_saturating_shift_left(instruction);
                  const bool signed_to_unsigned =
                      ((instruction >> 11) & 31U) == 12U;
                  const bool valid =
                      !signed_to_unsigned || (instruction & 0x2000'0000U) != 0;
                  const auto operation =
                      signed_to_unsigned
                          ? SimdSaturatingShiftLeft::signed_to_unsigned
                      : (instruction & 0x2000'0000U) != 0
                          ? SimdSaturatingShiftLeft::unsigned_to_unsigned
                          : SimdSaturatingShiftLeft::signed_to_signed;
                  const auto gadget =
                      decoded && valid
                          ? simd_saturating_shift_left_gadget(
                                operation, decoded->shape, decoded->shift)
                          : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (signed_to_unsigned
                               ? 0U
                               : static_cast<std::uintptr_t>(decoded->shift)
                                     << 16));
                } else if ((((instruction & 0x9f00'0400U) == 0x0f00'0400U) ||
                            ((instruction & 0xdf00'0400U) == 0x5f00'0400U)) &&
                           ((instruction >> 11) & 31U) >= 16U &&
                           ((instruction >> 11) & 31U) <= 19U &&
                           !(((instruction >> 11) & 31U) <= 17U &&
                             (instruction & 0x2000'0000U) == 0 &&
                             (instruction & 0x1000'0000U) == 0)) {
                  const unsigned opcode = (instruction >> 11) & 31U;
                  const unsigned encoded = (instruction >> 16) & 0x7fU;
                  const unsigned source_bits =
                      encoded >= 32U && encoded < 64U   ? 64U
                      : encoded >= 16U && encoded < 32U ? 32U
                      : encoded >= 8U && encoded < 16U  ? 16U
                                                        : 0U;
                  const unsigned shift = source_bits - encoded;
                  const bool scalar = (instruction & 0x1000'0000U) != 0;
                  const auto gadget =
                      shift != 0U && shift <= source_bits / 2U
                          ? simd_saturating_narrow_gadget(
                                opcode, (instruction & 0x2000'0000U) != 0,
                                source_bits, scalar,
                                !scalar && (instruction & 0x4000'0000U) != 0)
                          : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(shift) << 16));
                } else if ((instruction & 0x9f20'fc00U) == 0x0e20'1c00U) {
                  const auto operation =
                      ((instruction >> 22) & 3U) | ((instruction >> 27) & 4U);
                  const bool q = (instruction & 0x4000'0000U) != 0;
                  const unsigned destination = instruction & 31U;
                  const unsigned left = (instruction >> 5) & 31U;
                  const unsigned right = (instruction >> 16) & 31U;
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  if (operation == 2U && left == right && q &&
                      destination == left) {
                    emit(words, uml_arm64_gadget_nop);
                  } else if (operation == 2U && left == right) {
                    emit(words, simd_move_gadget(q));
                    emit(words, vector_offset(destination));
                    emit(words, vector_offset(left));
                  } else {
                    emit(words, simd_bitwise_gadget(operation, q));
                    emit(words, vector_offset(destination));
                    emit(words, vector_offset(left));
                    emit(words, vector_offset(right));
                  }
                } else if ((instruction & 0xbfe0'8400U) == 0x2e00'0000U) {
                  const bool q = (instruction & 0x4000'0000U) != 0;
                  const unsigned offset = (instruction >> 11) & 15U;
                  if (offset < (q ? 16U : 8U)) {
                    emit(words, simd_extract_gadget(q, offset));
                    emit(words, (instruction & 31U) |
                                    (static_cast<std::uintptr_t>(
                                         (instruction >> 5) & 31U)
                                     << 8) |
                                    (static_cast<std::uintptr_t>(
                                         (instruction >> 16) & 31U)
                                     << 16));
                  } else {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                } else if ((instruction & 0x9f30'0c00U) == 0x0e30'0800U &&
                           (((instruction >> 12) & 31U) == 3U ||
                            ((instruction >> 12) & 31U) == 10U ||
                            ((instruction >> 12) & 31U) == 26U ||
                            ((instruction >> 12) & 31U) == 27U)) {
                  const auto gadget = simd_integer_reduce_gadget(
                      (instruction >> 12) & 31U,
                      (instruction & 0x2000'0000U) != 0,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8));
                } else if ((instruction & 0x9fbf'fc00U) == 0x0ea1'c800U) {
                  const auto gadget = simd_unsigned_estimate_gadget(
                      (instruction & 0x2000'0000U) != 0,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction & 0x0040'0000U) != 0);
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(words, simd_unary_operand(instruction & 31U,
                                                 (instruction >> 5) & 31U));
                } else if ((((instruction & 0x9f30'0c00U) == 0x0e20'0800U) ||
                            ((instruction & 0xdf30'0c00U) == 0x5e20'0800U)) &&
                           (((instruction >> 12) & 31U) == 2U ||
                            ((instruction >> 12) & 31U) == 3U ||
                            ((instruction >> 12) & 31U) == 4U ||
                            ((instruction >> 12) & 31U) == 6U ||
                            ((instruction >> 12) & 31U) == 7U ||
                            ((instruction >> 12) & 31U) == 11U ||
                            ((instruction >> 12) & 31U) == 18U ||
                            ((instruction >> 12) & 31U) == 19U ||
                            ((instruction >> 12) & 31U) == 20U)) {
                  const auto gadget = simd_integer_unary_gadget(
                      (instruction >> 12) & 31U,
                      (instruction & 0x2000'0000U) != 0,
                      (instruction & 0x1000'0000U) != 0,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(words, simd_unary_operand(instruction & 31U,
                                                 (instruction >> 5) & 31U));
                } else if ((instruction & 0x9f20'fc00U) == 0x0e20'0800U ||
                           (instruction & 0x9f20'fc00U) == 0x0e20'1800U) {
                  const bool reverse16 = (instruction & 0x0000'1000U) != 0;
                  const bool alternate = (instruction & 0x2000'0000U) != 0;
                  const unsigned block_bits = reverse16   ? 16U
                                              : alternate ? 32U
                                                          : 64U;
                  const auto gadget =
                      reverse16 && alternate
                          ? nullptr
                          : simd_reverse_native_gadget(
                                block_bits, (instruction & 0x4000'0000U) != 0,
                                (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(words, simd_unary_operand(instruction & 31U,
                                                 (instruction >> 5) & 31U));
                } else if ((instruction & 0x9f20'fc00U) == 0x0e20'5800U) {
                  const bool alternate = (instruction & 0x2000'0000U) != 0;
                  const unsigned size = (instruction >> 22) & 3U;
                  const unsigned operation = !alternate && size == 0U  ? 0U
                                             : alternate && size == 0U ? 1U
                                             : alternate && size == 1U ? 2U
                                                                       : 3U;
                  if (operation == 3U) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words,
                       simd_byte_unary_gadget(
                           operation, (instruction & 0x4000'0000U) != 0));
                  emit(words, simd_unary_operand(instruction & 31U,
                                                 (instruction >> 5) & 31U));
                } else if ((instruction & 0xdf20'0c00U) == 0x5e20'0800U &&
                           (((instruction >> 12) & 31U) == 0U ||
                            ((instruction >> 12) & 31U) == 1U ||
                            ((instruction >> 12) & 31U) == 5U)) {
                  /* The reverse and byte-unary opcode slots are vector-only. */
                  emit(words, uml_arm64_gadget_illegal);
                  emit(words, address);
                  ++compiled;
                  return {Program{guest_address, compiled, std::move(words),
                                  program_accesses_fp_state},
                          true, address, instruction};
                } else if ((instruction & 0xffff'fc00U) == 0x5ef1'b800U) {
                  emit(words, uml_arm64_gadget_simd_addp_scalar);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8));
                } else if ((instruction & 0xbf00'fc00U) == 0x0f00'8400U ||
                           (instruction & 0xbf00'fc00U) == 0x0f00'8c00U) {
                  const unsigned encoded = (instruction >> 16) & 0x7fU;
                  const unsigned source_bits =
                      encoded >= 32U && encoded < 64U   ? 64U
                      : encoded >= 16U && encoded < 32U ? 32U
                      : encoded >= 8U && encoded < 16U  ? 16U
                                                        : 0U;
                  const unsigned shift = source_bits - encoded;
                  const auto gadget =
                      shift != 0U && shift <= source_bits / 2U
                          ? simd_shift_narrow_gadget(
                                source_bits, (instruction & 0x4000'0000U) != 0,
                                (instruction & 0x800U) != 0)
                          : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(shift) << 16));
                } else if (((instruction & 0xdf00'0400U) == 0x5f00'0400U ||
                            ((instruction & 0x9f00'0400U) == 0x0f00'0400U &&
                             ((instruction >> 16) & 0x7fU) >= 8U)) &&
                           (((instruction >> 11) & 31U) < 20U ||
                            (((instruction >> 11) & 31U) >= 21U &&
                             ((instruction >> 11) & 31U) <= 27U) ||
                            ((instruction >> 11) & 31U) == 29U ||
                            ((instruction >> 11) & 31U) == 30U ||
                            (((instruction & 0x1000'0000U) != 0) &&
                             ((instruction >> 11) & 31U) == 20U))) {
                  /* Every allocated operation in this part of the
                   * immediate-shift class was selected above. Resolve its
                   * reserved holes once during translation instead of reaching
                   * the fatal fallback. */
                  emit(words, uml_arm64_gadget_illegal);
                  emit(words, address);
                  ++compiled;
                  return {Program{guest_address, compiled, std::move(words),
                                  program_accesses_fp_state},
                          true, address, instruction};
                } else if ((instruction & 0x9f20'fc00U) == 0x0e20'4000U ||
                           (instruction & 0x9f20'fc00U) == 0x0e20'6000U) {
                  const auto gadget = simd_high_narrow_gadget(
                      ((instruction >> 11) & 31U) == 12U,
                      (instruction & 0x2000'0000U) != 0,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  const auto vector_offset = [](unsigned index) {
                    return offsetof(CpuState, vectors) +
                           index * sizeof(VectorRegister);
                  };
                  emit(words, vector_offset(instruction & 31U));
                  emit(words, vector_offset((instruction >> 5) & 31U));
                  emit(words, vector_offset((instruction >> 16) & 31U));
                } else if (((instruction & 0x9f3f'fc00U) == 0x0e20'8800U ||
                            (instruction & 0x9f3f'fc00U) == 0x0e20'9800U ||
                            (instruction & 0x9f3f'fc00U) == 0x0e20'a800U ||
                            (instruction & 0x9f3f'fc00U) == 0x1e20'8800U ||
                            (instruction & 0x9f3f'fc00U) == 0x1e20'9800U ||
                            (instruction & 0x9f3f'fc00U) == 0x1e20'a800U)) {
                  const auto gadget = simd_compare_zero_native_gadget(
                      (instruction >> 12) & 31U,
                      (instruction & 0x2000'0000U) != 0,
                      (instruction & 0x1000'0000U) != 0,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(words, simd_unary_operand(instruction & 31U,
                                                 (instruction >> 5) & 31U));
                } else if ((instruction & 0xbfe0'fc00U) == 0x0e00'3c00U ||
                           (instruction & 0xbfe0'fc00U) == 0x0e00'2c00U) {
                  const auto lane =
                      detail::decode_simd_lane_index((instruction >> 16) & 31U);
                  const auto gadget =
                      lane
                          ? simd_lane_to_gpr_gadget(
                                (instruction & 0x1000U) == 0,
                                (instruction & 0x4000'0000U) != 0,
                                lane->element_shift, (instruction & 31U) == 31U)
                          : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(lane->offset) << 16));
                } else if ((instruction & 0xffe0'fc00U) == 0x4e00'1c00U) {
                  const auto lane =
                      detail::decode_simd_lane_index((instruction >> 16) & 31U);
                  const auto gadget =
                      lane ? simd_insert_general_gadget(
                                 lane->element_shift,
                                 ((instruction >> 5) & 31U) == 31U)
                           : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(lane->offset) << 16));
                } else if ((instruction & 0xffe0'8400U) == 0x6e00'0400U) {
                  const auto lane =
                      detail::decode_simd_lane_index((instruction >> 16) & 31U);
                  const auto gadget =
                      lane ? simd_insert_element_gadget(lane->element_shift)
                           : nullptr;
                  if (gadget == nullptr) {
                    emit(words, uml_arm64_gadget_illegal);
                    emit(words, address);
                    ++compiled;
                    return {Program{guest_address, compiled, std::move(words),
                                    program_accesses_fp_state},
                            true, address, instruction};
                  }
                  const auto source_offset =
                      ((instruction >> 11) & 15U) & ~(lane->element_size - 1U);
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(lane->offset) << 16) |
                          (static_cast<std::uintptr_t>(source_offset) << 24));
                } else if ((instruction & 0xffff'fc00U) == 0x1ee6'0000U ||
                           (instruction & 0xffff'fc00U) == 0x9ee6'0000U ||
                           (instruction & 0xffff'fc00U) == 0x1ee7'0000U ||
                           (instruction & 0xffff'fc00U) == 0x9ee7'0000U ||
                           (instruction & 0xffff'fc00U) == 0x9e66'0000U ||
                           (instruction & 0xffff'fc00U) == 0x9e67'0000U ||
                           (instruction & 0xffff'fc00U) == 0x1e26'0000U ||
                           (instruction & 0xffff'fc00U) == 0x1e27'0000U ||
                           (instruction & 0xffff'fc00U) == 0x9eae'0000U ||
                           (instruction & 0xffff'fc00U) == 0x9eaf'0000U) {
                  const auto opcode = instruction & 0xffff'fc00U;
                  const bool to_gpr =
                      opcode == 0x1ee6'0000U || opcode == 0x9ee6'0000U ||
                      opcode == 0x1e26'0000U || opcode == 0x9e66'0000U ||
                      opcode == 0x9eae'0000U;
                  if (to_gpr && (instruction & 31U) == 31U) {
                    emit(words, uml_arm64_gadget_nop);
                  } else {
                    const bool zero_source = ((instruction >> 5) & 31U) == 31U;
                    Gadget move;
                    if (opcode == 0x1ee6'0000U || opcode == 0x9ee6'0000U)
                      move = uml_arm64_gadget_vector_to_gpr_16_low;
                    else if (opcode == 0x1ee7'0000U || opcode == 0x9ee7'0000U)
                      move = zero_source
                                 ? uml_arm64_gadget_gpr_to_vector_16_low_zero
                                 : uml_arm64_gadget_gpr_to_vector_16_low;
                    else if (opcode == 0x9e66'0000U)
                      move = uml_arm64_gadget_vector_to_gpr_64_low;
                    else if (opcode == 0x9e67'0000U)
                      move = zero_source
                                 ? uml_arm64_gadget_gpr_to_vector_64_low_zero
                                 : uml_arm64_gadget_gpr_to_vector_64_low;
                    else if (opcode == 0x1e26'0000U)
                      move = uml_arm64_gadget_vector_to_gpr_32_low;
                    else if (opcode == 0x1e27'0000U)
                      move = zero_source
                                 ? uml_arm64_gadget_gpr_to_vector_32_low_zero
                                 : uml_arm64_gadget_gpr_to_vector_32_low;
                    else if (opcode == 0x9eae'0000U)
                      move = uml_arm64_gadget_vector_to_gpr_64_high;
                    else
                      move = zero_source
                                 ? uml_arm64_gadget_gpr_to_vector_64_high_zero
                                 : uml_arm64_gadget_gpr_to_vector_64_high;
                    emit(words, move);
                    emit(words,
                         (instruction & 31U) | (static_cast<std::uintptr_t>(
                                                    (instruction >> 5) & 31U)
                                                << 8));
                  }
                } else if ((instruction & 0xff20'0c00U) == 0x1e20'0c00U &&
                           ((instruction >> 22) & 3U) != 2U) {
                  emit(words, floating_select_gadget((instruction >> 12) & 15U,
                                                     (instruction >> 22) & 3U));
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>((instruction >> 16) &
                                                       31U)
                           << 16));
                } else if ((instruction & 0xff20'0c00U) == 0x1e20'0c00U) {
                  emit(words, uml_arm64_gadget_illegal);
                  emit(words, address);
                  ++compiled;
                  return {Program{guest_address, compiled, std::move(words),
                                  program_accesses_fp_state},
                          true, address, instruction};
		} else if ((instruction & 0xbfe0'fc00U) == 0x0e00'0c00U) {
			const auto lane = detail::decode_simd_lane_index(
				(instruction >> 16) & 31U);
			const bool q = (instruction & 0x4000'0000U) != 0;
			const auto gadget = lane ? simd_duplicate_general_gadget(q,
				lane->element_shift,
				((instruction >> 5) & 31U) == 31U) : nullptr;
			if (gadget == nullptr) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			emit(words, gadget);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8));
		} else {
			emit_fatal(words, address);
		}
		goto instruction_decoded;

decode_load_store:
		if ((instruction & 0xff00'0000U) == 0xd800'0000U) {
			/* PRFM (literal) is an implementation-defined performance hint. */
			emit(words, uml_arm64_gadget_nop);
		} else if ((instruction & 0x3b00'0000U) == 0x1800'0000U) {
			const bool vector = (instruction & 0x0400'0000U) != 0;
			const unsigned operation = instruction >> 30;
			if (vector && operation == 3U) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const unsigned destination = instruction & 31U;
			emit(words, literal_load_gadget(
				vector, operation, !vector && destination == 31U));
			const auto destination_offset = vector ?
				offsetof(CpuState, vectors) +
					destination * sizeof(VectorRegister) :
				destination < 31U ? destination * sizeof(std::uint64_t) : 0U;
			const auto displacement =
				sign_extend((instruction >> 5) & 0x7ffffU, 19) * 4;
			emit(words, destination_offset |
				(static_cast<std::uint64_t>(displacement) << 16));
			emit(words, address);
		} else if ((instruction & 0xbfbf'fc00U) == 0x0d01'8400U) {
			const bool load = (instruction & 0x0040'0000U) != 0U;
			const unsigned lane = instruction >> 30;
			const bool sp_base = ((instruction >> 5) & 31U) == 31U;
			emit(words, simd_ordered_lane_gadgets[load][lane][sp_base]);
			emit(words, simd_ordered_memory_operand(instruction));
			emit(words, address);
		} else if ((instruction & 0x3f20'0c00U) == 0x1d00'0800U) {
			const unsigned size = instruction >> 30;
			const unsigned opc = (instruction >> 22) & 3U;
			if ((opc & 2U) != 0U && size != 0U) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const bool load = (opc & 1U) != 0U;
			const bool sp_base = ((instruction >> 5) & 31U) == 31U;
			if ((opc & 2U) != 0U) {
				const unsigned native = host_has_lrcpc3();
				emit(words, (load ? simd_ordered_q_load_gadgets :
					simd_ordered_q_store_gadgets)[native][sp_base]);
			} else {
				emit(words, (load ? simd_ordered_scalar_load_gadgets :
					simd_ordered_scalar_store_gadgets)[size][sp_base]);
			}
			const auto offset = sign_extend(
				(instruction >> 12) & 0x1ffU, 9);
			emit(words, simd_ordered_memory_operand(instruction, offset));
			emit(words, address);
		} else if ((instruction & 0xbf20'0000U) == 0x0c00'0000U) {
			const unsigned opcode = (instruction >> 12) & 15U;
			const bool interleaved = opcode == 8U || opcode == 4U ||
				opcode == 0U;
			const bool valid_opcode = interleaved || opcode == 7U ||
				opcode == 10U || opcode == 6U || opcode == 2U;
			const bool post_index = (instruction & 0x0080'0000U) != 0;
			const bool valid_no_offset = post_index ||
				((instruction >> 16) & 31U) == 0U;
			const bool valid_arrangement = !interleaved ||
				(instruction & 0x4000'0c00U) != 0x0000'0c00U;
			if (valid_opcode && valid_no_offset && valid_arrangement) {
				const bool load =
					(instruction & 0x0040'0000U) != 0;
				const bool q = (instruction & 0x4000'0000U) != 0;
				const unsigned size = (instruction >> 10) & 3U;
				const unsigned base = (instruction >> 5) & 31U;
				const unsigned offset_register =
					(instruction >> 16) & 31U;
				const unsigned address_mode = !post_index ? 0U :
					offset_register == 31U ? 1U : 2U;
				const auto *gadgets =
					simd_structure_gadgets(opcode, q, size);
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				const auto offset_register_offset = offset_register < 31U ?
					offset_register * sizeof(std::uint64_t) : 0U;
				emit(words, (load ? gadgets->load : gadgets->store)
					[base == 31U][address_mode]);
				emit(words, (instruction & 31U) |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					(static_cast<std::uintptr_t>(offset_register_offset)
						<< 32) |
					(static_cast<std::uintptr_t>(
						std::size_t{1} << size) << 48));
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0xbf80'0000U) == 0x0d00'0000U ||
			   (instruction & 0xbf80'0000U) == 0x0d80'0000U) {
			const unsigned opcode = (instruction >> 13) & 7U;
			const unsigned scale = opcode >> 1;
			const bool load = (instruction & 0x0040'0000U) != 0;
			const bool scale_bit = (instruction & 0x0000'1000U) != 0;
			const unsigned size = (instruction >> 10) & 3U;
			const bool post_index = (instruction & 0x0080'0000U) != 0;
			const bool valid_no_offset = post_index ||
				((instruction >> 16) & 31U) == 0U;
			const bool replicate = scale == 3U && load && !scale_bit;
			const bool valid_lane = scale == 0U ||
				(scale == 1U && (size & 1U) == 0U) ||
				(scale == 2U && (size & 2U) == 0U &&
				 ((size & 1U) == 0U || !scale_bit));
			if (valid_no_offset && replicate) {
				const bool q = (instruction & 0x4000'0000U) != 0;
				const unsigned count =
					((instruction & 0x0020'0000U) != 0 ? 1U : 0U) +
					(opcode == 6U ? 1U : 3U);
				const unsigned base = (instruction >> 5) & 31U;
				const unsigned offset_register =
					(instruction >> 16) & 31U;
				const unsigned address_mode = !post_index ? 0U :
					offset_register == 31U ? 1U : 2U;
				const auto &gadgets = simd_replicate_gadgets(size, q);
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				const auto offset_register_offset = offset_register < 31U ?
					offset_register * sizeof(std::uint64_t) : 0U;
				emit(words, gadgets[count - 1U][base == 31U]
					[address_mode]);
				emit(words, (instruction & 31U) |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					(static_cast<std::uintptr_t>(offset_register_offset)
						<< 32));
				emit(words, address);
			} else if (valid_no_offset && scale != 3U && valid_lane) {
				const bool q = (instruction & 0x4000'0000U) != 0;
				const unsigned count =
					((instruction & 0x0020'0000U) != 0 ? 1U : 0U) +
					((opcode & 1U) != 0 ? 3U : 1U);
				const unsigned width_index = scale < 2U ? scale :
					size == 0U ? 2U : 3U;
				const unsigned lane = scale == 0U ?
					(static_cast<unsigned>(q) << 3) |
					(static_cast<unsigned>(scale_bit) << 2) | size :
					scale == 1U ?
					(static_cast<unsigned>(q) << 2) |
					(static_cast<unsigned>(scale_bit) << 1) | (size >> 1) :
					(static_cast<unsigned>(q) << (1U - size)) |
					static_cast<unsigned>(scale_bit);
				const unsigned lane_offset = lane << width_index;
				const unsigned base = (instruction >> 5) & 31U;
				const unsigned offset_register =
					(instruction >> 16) & 31U;
				const unsigned address_mode = !post_index ? 0U :
					offset_register == 31U ? 1U : 2U;
				const auto &gadgets = simd_lane_gadgets(width_index);
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				const auto offset_register_offset = offset_register < 31U ?
					offset_register * sizeof(std::uint64_t) : 0U;
				emit(words, (load ? gadgets[count - 1U].load :
					gadgets[count - 1U].store)[base == 31U]
					[address_mode]);
				emit(words, (instruction & 31U) |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					(static_cast<std::uintptr_t>(offset_register_offset)
						<< 32) |
					(static_cast<std::uintptr_t>(lane_offset) << 48));
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0xff20'0400U) == 0xf820'0400U) {
			if (!options.enable_pointer_authentication) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const unsigned destination = instruction & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			const bool writeback = (instruction & 0x800U) != 0;
			if (writeback && base == destination && base != 31U) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const auto encoded_offset = ((instruction >> 12) & 0x1ffU) |
				(((instruction >> 22) & 1U) << 9);
			const auto displacement = sign_extend(encoded_offset, 10) * 8;
			emit(words, pointer_auth_load_gadget(
				(instruction & 0x0080'0000U) != 0, writeback,
				base == 31U, destination == 31U));
			emit(words, pointer_auth_load_operand(
				destination, base, displacement));
			emit(words, address);
		} else if ((instruction & 0xffc0'0000U) == 0xb940'0000U ||
			   (instruction & 0xffc0'0000U) == 0xf940'0000U ||
			   (instruction & 0xffc0'0000U) == 0xb900'0000U ||
			   (instruction & 0xffc0'0000U) == 0xf900'0000U) {
			const auto opcode = instruction & 0xffc0'0000U;
			const bool transfer_zero = (instruction & 31U) == 31U;
			const auto reg = instruction & 31U;
			const auto base = (instruction >> 5) & 31U;
			const auto scale = opcode == 0xb940'0000U ||
				opcode == 0xb900'0000U ? 2U : 3U;
			const auto operation = opcode == 0xb940'0000U ||
				opcode == 0xf940'0000U ? 1U : 0U;
			emit(words, scalar_unsigned_memory_gadget(
				scale, operation, base == 31U, transfer_zero));
			const auto offset = ((instruction >> 10) & 0xfffU) << scale;
			const auto reg_offset = reg < 31U ?
				reg * sizeof(std::uint64_t) : 0U;
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			emit(words, reg_offset |
				(static_cast<std::uintptr_t>(base_offset) << 16) |
				(static_cast<std::uintptr_t>(offset) << 32));
			emit(words, address);
		} else if ((instruction & 0x3f20'0c00U) == 0x1900'0800U &&
			   (instruction & 0x3fbf'fc00U) != 0x1980'0800U) {
			const auto fixed = instruction & 0xffe0'fc00U;
			const bool lrcpc3 = fixed == 0x9900'0800U ||
				fixed == 0xd900'0800U || fixed == 0x9900'1800U ||
				fixed == 0xd900'1800U || fixed == 0x9940'0800U ||
				fixed == 0xd940'0800U || fixed == 0x9940'1800U ||
				fixed == 0xd940'1800U;
			const bool lscp = fixed == 0xd900'5800U ||
				fixed == 0xd940'5800U || fixed == 0xd940'7800U;
			if (!lrcpc3 && !lscp) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const bool load = fixed != 0x9900'0800U &&
				fixed != 0xd900'0800U && fixed != 0x9900'1800U &&
				fixed != 0xd900'1800U && fixed != 0xd900'5800U;
			const bool writeback = lrcpc3 &&
				(instruction & 0x0000'1000U) == 0;
			const unsigned first = instruction & 31U;
			const unsigned second = (instruction >> 16) & 31U;
			if (load && first == second && first != 31U) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const unsigned base = (instruction >> 5) & 31U;
			const unsigned register_mode = (first == 31U ? 2U : 0U) |
				(second == 31U ? 1U : 0U);
			if (lscp) {
				const auto &gadgets = fixed == 0xd940'5800U ?
					consistent_pair_ldap_gadgets :
					fixed == 0xd940'7800U ?
					consistent_pair_ldapp_gadgets :
					consistent_pair_store_gadgets;
				emit(words, gadgets[register_mode]);
				emit(words, ordered_pair_operand(instruction));
				emit(words, address);
			} else {
				const unsigned size = (instruction >> 30) - 2U;
				/* LDIAPP post-index overlap selects WBSUPPRESS, which is the
				 * no-writeback gadget operating at the original address. */
				const bool effective_writeback = writeback &&
					(!load || base == 31U ||
					 (base != first && base != second));
				emit(words, load ?
					ordered_pair_load_gadgets[size][effective_writeback]
						[register_mode] :
					ordered_pair_store_gadgets[size][writeback]
						[register_mode]);
				emit(words, ordered_pair_operand(instruction));
				emit(words, address);
			}
		} else if ((instruction & 0x3fff'fc00U) == 0x38bf'c000U ||
			   (instruction & 0x3f20'0c00U) == 0x1900'0000U) {
			const bool no_offset =
				(instruction & 0x3fff'fc00U) == 0x38bf'c000U;
			const unsigned size = instruction >> 30;
			const unsigned operation = no_offset ? 1U :
				(instruction >> 22) & 3U;
			const bool valid = no_offset ||
				!((operation == 2U && size == 3U) ||
				  (operation == 3U && size >= 2U));
			if (!valid) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const auto offset = no_offset ? std::int64_t{0} :
				sign_extend((instruction >> 12) & 0x1ffU, 9);
			const unsigned transfer = instruction & 31U;
			if (operation == 0U) {
				emit(words, rcpc_store_gadgets[size][transfer == 31U]);
			} else {
				unsigned variant = size;
				if (operation == 2U)
					variant = size == 0U ? 5U :
						size == 1U ? 7U : 8U;
				else if (operation == 3U)
					variant = size == 0U ? 4U : 6U;
				emit(words,
					rcpc_load_gadgets[variant][transfer == 31U]);
			}
			emit(words, rcpc_memory_operand(instruction, offset));
			emit(words, address);
		} else if ((instruction & 0x3fbf'fc00U) == 0x1980'0800U) {
			if ((instruction >> 30) >= 2U) {
				const bool load =
					(instruction & 0x0040'0000U) != 0;
				const bool wide = (instruction >> 30) == 3U;
				const unsigned transfer = instruction & 31U;
				const unsigned base = (instruction >> 5) & 31U;
				const bool suppress = load && base != 31U && base == transfer;
				Gadget gadget{};
				if (load) {
					if (wide)
						gadget = suppress ?
							uml_arm64_gadget_ordered_writeback_load_x_suppress :
							transfer == 31U ?
							uml_arm64_gadget_ordered_writeback_load_x_z :
							uml_arm64_gadget_ordered_writeback_load_x_r;
					else
						gadget = suppress ?
							uml_arm64_gadget_ordered_writeback_load_w_suppress :
							transfer == 31U ?
							uml_arm64_gadget_ordered_writeback_load_w_z :
							uml_arm64_gadget_ordered_writeback_load_w_r;
				} else if (wide) {
					gadget = transfer == 31U ?
						uml_arm64_gadget_ordered_writeback_store_x_z :
						uml_arm64_gadget_ordered_writeback_store_x_r;
				} else {
					gadget = transfer == 31U ?
						uml_arm64_gadget_ordered_writeback_store_w_z :
						uml_arm64_gadget_ordered_writeback_store_w_r;
				}
				const auto transfer_offset = transfer < 31U ?
					transfer * sizeof(std::uint64_t) : 0U;
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				emit(words, gadget);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16));
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0x3fa0'0000U) == 0x0880'0000U) {
			const bool valid = ((instruction >> 16) & 31U) == 31U &&
				((instruction >> 10) & 31U) == 31U;
			if (valid) {
				const bool load =
					(instruction & 0x0040'0000U) != 0;
				const unsigned ordering =
					(instruction & 0x0000'8000U) != 0;
				const unsigned size = instruction >> 30;
				const unsigned transfer = instruction & 31U;
				const unsigned base = (instruction >> 5) & 31U;
				emit(words, load ?
					ordered_load_gadgets[ordering][size]
						[transfer == 31U] :
					ordered_store_gadgets[ordering][size]
						[transfer == 31U]);
				const auto transfer_offset = transfer < 31U ?
					transfer * sizeof(std::uint64_t) : 0U;
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16));
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0xbf20'fc00U) == 0x1920'0800U ||
			   ((instruction & 0xbf20'8c00U) == 0x3820'8000U &&
			    ((instruction >> 12) & 7U) >= 1U &&
			    ((instruction >> 12) & 7U) <= 3U)) {
			const bool compare_swap =
				(instruction & 0xbf20'fc00U) == 0x1920'0800U;
			const unsigned encoded_operation =
				(instruction >> 12) & 7U;
			const bool valid = compare_swap ||
				(encoded_operation >= 1U && encoded_operation <= 3U);
			if (!valid) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const unsigned first = (instruction >> 16) & 31U;
			const unsigned second = instruction & 31U;
			const unsigned source = compare_swap ? second : first;
			const unsigned result = compare_swap ? first : second;
			const bool acquire =
				(instruction & 0x0080'0000U) != 0 && result != 31U;
			const bool release =
				(instruction & 0x0040'0000U) != 0;
			const unsigned ordering =
				(acquire ? 2U : 0U) | (release ? 1U : 0U);
			const unsigned registers = (source == 31U ? 2U : 0U) |
				(result == 31U ? 1U : 0U);
			const unsigned operation = compare_swap ? 0U :
				encoded_operation;
			const unsigned soft = (instruction >> 30) & 1U;
			const unsigned base = (instruction >> 5) & 31U;
			if (base == 31U)
				emit(words,
					uml_arm64_gadget_atomic_sp_alignment_write);
			emit(words, rcw_scalar_gadgets[operation][soft][ordering]
				[registers]);
			emit(words, rcw_scalar_operand(instruction, compare_swap));
			emit(words, address);
		} else if ((instruction & 0xbf20'fc00U) == 0x1920'0c00U ||
			   ((instruction & 0xbf20'8c00U) == 0x1920'8000U &&
			    (((instruction >> 12) & 7U) != 0U ||
			     (instruction & 0x4000'0000U) != 0U))) {
			const bool compare_swap =
				(instruction & 0xbf20'fc00U) == 0x1920'0c00U;
			const unsigned operation = (instruction >> 12) & 7U;
			const unsigned first = instruction & 31U;
			const unsigned second = (instruction >> 16) & 31U;
			const bool valid_operation = compare_swap ||
				(operation >= 1U && operation <= 3U);
			const bool valid_registers = compare_swap ?
				(first & 1U) == 0 && (second & 1U) == 0 :
				first != 31U && second != 31U && first != second;
			if (!valid_operation || !valid_registers) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const bool acquire =
				(instruction & 0x0080'0000U) != 0;
			const bool release =
				(instruction & 0x0040'0000U) != 0;
			const unsigned ordering =
				(acquire ? 2U : 0U) | (release ? 1U : 0U);
			const unsigned soft = (instruction >> 30) & 1U;
			const unsigned base = (instruction >> 5) & 31U;
			if (base == 31U)
				emit(words,
					uml_arm64_gadget_atomic_sp_alignment_write);
			if (compare_swap) {
				const unsigned registers =
					(first == 30U ? 2U : 0U) |
					(second == 30U ? 1U : 0U);
				emit(words,
					rcw_pair_cas_gadgets[soft][ordering][registers]);
			} else {
				emit(words, rcw_pair_modify_gadgets[operation - 1U]
					[soft][ordering]);
			}
			emit(words, rcw_pair_operand(instruction));
			emit(words, address);
		} else if ((instruction & 0xbf20'0c00U) == 0x1920'0000U) {
			const unsigned first = instruction & 31U;
			const unsigned second = (instruction >> 16) & 31U;
			const unsigned o3 = (instruction >> 15) & 1U;
			const unsigned opcode = (instruction >> 12) & 7U;
			const unsigned operation = o3 ? 8U : opcode;
			const bool valid_operation =
				(instruction & 0x4000'0000U) == 0U &&
				(!o3 || opcode == 0U) &&
				(operation == 1U || operation == 3U || operation == 8U);
			const bool valid_registers = first != 31U && second != 31U &&
				first != second;
			if (valid_operation && valid_registers) {
				const bool acquire =
					(instruction & 0x0080'0000U) != 0;
				const bool release =
					(instruction & 0x0040'0000U) != 0;
				const unsigned ordering =
					(acquire ? 2U : 0U) | (release ? 1U : 0U);
				const unsigned family = operation == 1U ? 0U :
					operation == 3U ? 1U : 2U;
				emit(words, atomic_pair128_gadgets[family][ordering]);
				const unsigned base = (instruction >> 5) & 31U;
				const auto first_offset = first * sizeof(std::uint64_t);
				const auto second_offset = second * sizeof(std::uint64_t);
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				emit(words, first_offset |
					(static_cast<std::uintptr_t>(second_offset) << 16) |
					(static_cast<std::uintptr_t>(base_offset) << 32));
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0x3f20'0c00U) == 0x1900'0400U ||
			   (instruction & 0x3f20'0c00U) == 0x1d00'0400U ||
			   (instruction & 0x3fe0'0c00U) == 0x19c0'0400U ||
			   (instruction & 0x3fe0'0c00U) == 0x1dc0'0400U) {
			/* SET and SETG occupy values also accepted by the broad CPYF/CPY
			 * masks. Resolve both specific families before selecting CPY. */
			const bool set_tag =
				(instruction & 0x3fe0'0c00U) == 0x1dc0'0400U;
			const bool set = !set_tag &&
				(instruction & 0x3fe0'0c00U) == 0x19c0'0400U;
			const bool copy_forward = !set && !set_tag &&
				(instruction & 0x3f20'0c00U) == 0x1900'0400U;
			const unsigned family = set ? 2U : copy_forward ? 0U : 1U;
			const unsigned destination = instruction & 31U;
			const unsigned count = (instruction >> 5) & 31U;
			const unsigned source = (instruction >> 16) & 31U;
			const unsigned stage = (set || set_tag) ?
				(instruction >> 14) & 3U :
				(instruction >> 22) & 3U;
			const bool valid = (instruction >> 30) == 0 && stage != 3 &&
				destination != 31U && count != 31U &&
				destination != count && source != destination &&
				source != count && (set || set_tag || source != 31U);
			if (!valid) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			emit(words, set_tag ?
				mops_set_tag_gadget(stage, source == 31U) :
				mops_gadget(family, stage, source == 31U));
			if (stage != 1) {
				emit(words, mops_operand(instruction));
				if (set_tag || stage == 2)
					emit(words, address);
			}
		} else if ((instruction & 0xffff'fc00U) == 0xd9e0'0000U ||
			   (instruction & 0xffff'fc00U) == 0xd9a0'0000U ||
			   (instruction & 0xffff'fc00U) == 0xd920'0000U) {
			/* LDGM/STGM/STZGM are undefined at EL0. Reject them before
			 * observing CpuState or memory, matching the strict interpreter.
			 */
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0xffc0'0000U) == 0x6880'0000U ||
			   (instruction & 0xffc0'0000U) == 0x6900'0000U ||
			   (instruction & 0xffc0'0000U) == 0x6980'0000U) {
			const unsigned encoded_mode = (instruction >> 23) & 3U;
			const unsigned mode = encoded_mode - 1U;
			const unsigned first = instruction & 31U;
			const unsigned second = (instruction >> 10) & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			const auto immediate = static_cast<std::int16_t>(
				detail::sign_extend((instruction >> 15) & 0x7fU, 7) * 16);
			emit(words, mte_store_pair_tag_gadget(mode, base == 31U,
				first == 31U, second == 31U));
			emit(words, mte_memory_operand(
				base, first, second, immediate, false));
			emit(words, address);
		} else if ((instruction & 0xff20'0000U) == 0xd920'0000U) {
			const unsigned operation = (instruction >> 22) & 3U;
			const unsigned encoded_mode = (instruction >> 10) & 3U;
			const unsigned transfer = instruction & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			const auto immediate = static_cast<std::int16_t>(
				detail::sign_extend((instruction >> 12) & 0x1ffU, 9) * 16);
			if (encoded_mode == 0) {
				if (operation != 1U) {
					emit(words, uml_arm64_gadget_illegal);
					emit(words, address);
					++compiled;
					return {Program{guest_address, compiled,
						std::move(words), program_accesses_fp_state},
						true, address, instruction};
				}
				emit(words, mte_load_tag_gadget(
					base == 31U, transfer == 31U));
				emit(words, mte_memory_operand(
					base, transfer, 31U, immediate, false));
			} else {
				emit(words, mte_tag_store_gadget(operation,
					encoded_mode - 1U, base == 31U));
				emit(words, mte_memory_operand(
					base, transfer, 31U, immediate, true));
			}
			emit(words, address);
		} else if (const auto atomic_floating =
				detail::decode_atomic_floating(instruction);
			   atomic_floating.recognized) {
			if (!atomic_floating.valid) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state}, true, address, instruction};
			}
			const unsigned size = instruction >> 30;
			const unsigned operation =
				static_cast<unsigned>(atomic_floating.operation);
			const unsigned variant = atomic_floating.return_old ?
				(atomic_floating.acquire ? 2U : 0U) |
					(atomic_floating.release ? 1U : 0U) :
				atomic_floating.release ? 5U : 4U;
			const unsigned base = (instruction >> 5) & 31U;
			const auto &gadgets = host_has_lsfe() ?
				atomic_floating_native_gadgets :
				atomic_floating_software_gadgets;
			emit(words, gadgets[size][operation][base == 31U][variant]);
			emit(words, detail::atomic_floating_operand(instruction));
			emit(words, address);
			program_accesses_fp_state = true;
		} else if ((instruction & 0xffff'fc00U) == 0xf83f'd000U ||
			   (instruction & 0xffff'fc00U) == 0xf83f'9000U ||
			   (instruction & 0xffe0'fc00U) == 0xf820'b000U ||
			   (instruction & 0xffe0'fc00U) == 0xf820'a000U) {
			const unsigned first = instruction & 31U;
			if ((first & 1U) != 0 || first >= 24) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
			const auto fixed = instruction & 0xffe0'fc00U;
			const unsigned operation =
				(instruction & 0xffff'fc00U) == 0xf83f'd000U ? 0U :
				(instruction & 0xffff'fc00U) == 0xf83f'9000U ? 1U :
				fixed == 0xf820'b000U ? 2U : 3U;
			const unsigned base = (instruction >> 5) & 31U;
			const unsigned status = (instruction >> 16) & 31U;
			emit(words, ls64_gadget(
				operation, base == 31U, status == 31U));
			emit(words, ls64_operand(first, base, status,
				base == 31U, status == 31U));
			emit(words, address);
		} else if ((instruction & 0x3f20'0c00U) == 0x3820'0000U ||
			   (instruction & 0x3f20'0c00U) == 0x1920'0400U) {
			const bool unprivileged =
				(instruction & 0x0100'0000U) != 0;
			const unsigned encoded_size = instruction >> 30;
			const unsigned o3 = (instruction >> 15) & 1U;
			const unsigned opcode = (instruction >> 12) & 7U;
			const bool supported = unprivileged ?
				(encoded_size <= 1U &&
				 ((!o3 && (opcode == 0U || opcode == 1U ||
					     opcode == 3U)) ||
				  (o3 && opcode == 0U))) :
				(!o3 || opcode == 0U);
			if (supported) {
				const unsigned operation = o3 ? 8U : opcode;
				const unsigned size = unprivileged ?
					encoded_size + 2U : encoded_size;
				const unsigned source = (instruction >> 16) & 31U;
				const unsigned destination = instruction & 31U;
				const bool acquire =
					(instruction & 0x0080'0000U) != 0 &&
					destination != 31U;
				const bool release =
					(instruction & 0x0040'0000U) != 0;
				const unsigned ordering =
					(acquire ? 2U : 0U) | (release ? 1U : 0U);
				const unsigned registers =
					(source == 31U ? 2U : 0U) |
					(destination == 31U ? 1U : 0U);
				const unsigned base = (instruction >> 5) & 31U;
				if (base == 31U)
					emit(words,
						uml_arm64_gadget_atomic_sp_alignment_write);
				emit(words, atomic_rmw_gadgets[operation][ordering]
					[size][registers]);
				const auto source_offset = source < 31U ?
					source * sizeof(std::uint64_t) : 0U;
				const auto destination_offset = destination < 31U ?
					destination * sizeof(std::uint64_t) : 0U;
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				emit(words, destination_offset |
					(static_cast<std::uintptr_t>(source_offset) << 16) |
					(static_cast<std::uintptr_t>(base_offset) << 32));
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0xbfa0'0000U) == 0x0820'0000U ||
			   (instruction & 0xbfa0'0000U) == 0x0980'0000U) {
			const unsigned compare = (instruction >> 16) & 31U;
			const unsigned replacement = instruction & 31U;
			const bool unprivileged = (instruction & 0x0100'0000U) != 0;
			const unsigned size_code = instruction >> 30;
			const bool valid = ((instruction >> 10) & 31U) == 31U &&
				(compare & 1U) == 0 && (replacement & 1U) == 0 &&
				size_code <= 1U && (!unprivileged || size_code == 1U);
			if (valid) {
				const bool acquire =
					(instruction & 0x0040'0000U) != 0;
				const bool release =
					(instruction & 0x0000'8000U) != 0;
				const unsigned ordering =
					(acquire ? 2U : 0U) | (release ? 1U : 0U);
				const unsigned registers =
					(compare == 30U ? 2U : 0U) |
					(replacement == 30U ? 1U : 0U);
				const unsigned base = (instruction >> 5) & 31U;
				if (base == 31U)
					emit(words,
						uml_arm64_gadget_atomic_sp_alignment_write);
				emit(words, compare_swap_pair_gadgets[ordering]
					[size_code][registers]);
				const auto compare_offset = compare * sizeof(std::uint64_t);
				const auto replacement_offset =
					replacement * sizeof(std::uint64_t);
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				emit(words, compare_offset |
					(static_cast<std::uintptr_t>(replacement_offset) << 16) |
					(static_cast<std::uintptr_t>(base_offset) << 32));
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0x3fa0'0000U) == 0x08a0'0000U ||
			   (instruction & 0xbfa0'0000U) == 0x8980'0000U) {
			const bool unprivileged = (instruction & 0x0100'0000U) != 0;
			const bool valid = ((instruction >> 10) & 31U) == 31U &&
				(!unprivileged || (instruction >> 30) == 3U);
			if (valid) {
				const unsigned compare = (instruction >> 16) & 31U;
				const unsigned replacement = instruction & 31U;
				const bool acquire =
					(instruction & 0x0040'0000U) != 0 &&
					compare != 31U;
				const bool release =
					(instruction & 0x0000'8000U) != 0;
				const unsigned ordering =
					(acquire ? 2U : 0U) | (release ? 1U : 0U);
				const unsigned registers =
					(compare == 31U ? 2U : 0U) |
					(replacement == 31U ? 1U : 0U);
				const unsigned base = (instruction >> 5) & 31U;
				if (base == 31U)
					emit(words,
						uml_arm64_gadget_atomic_sp_alignment_write);
				emit(words, compare_swap_gadgets[ordering]
					[instruction >> 30][registers]);
				const auto compare_offset = compare < 31U ?
					compare * sizeof(std::uint64_t) : 0U;
				const auto replacement_offset = replacement < 31U ?
					replacement * sizeof(std::uint64_t) : 0U;
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				emit(words, compare_offset |
					(static_cast<std::uintptr_t>(replacement_offset) << 16) |
					(static_cast<std::uintptr_t>(base_offset) << 32));
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0xbfa0'0000U) == 0x8820'0000U) {
			const bool load = (instruction & 0x0040'0000U) != 0;
			const unsigned first = instruction & 31U;
			const unsigned second = (instruction >> 10) & 31U;
			const unsigned status = (instruction >> 16) & 31U;
			const bool valid = !load ||
				(status == 31U && first != second);
			if (valid) {
				const unsigned ordering =
					(instruction & 0x0000'8000U) != 0;
				const unsigned size = (instruction >> 30) - 2U;
				const unsigned base = (instruction >> 5) & 31U;
				if (base == 31U)
					emit(words, load ?
						uml_arm64_gadget_atomic_sp_alignment_read :
						uml_arm64_gadget_atomic_sp_alignment_write);
				if (load) {
					const unsigned registers =
						(first == 31U ? 2U : 0U) |
						(second == 31U ? 1U : 0U);
					emit(words, exclusive_pair_load_gadgets
						[ordering][size][registers]);
					const auto first_offset = first < 31U ?
						first * sizeof(std::uint64_t) : 0U;
					const auto second_offset = second < 31U ?
						second * sizeof(std::uint64_t) : 0U;
					const auto base_offset = base < 31U ?
						base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
					emit(words, first_offset |
						(static_cast<std::uintptr_t>(second_offset) << 16) |
						(static_cast<std::uintptr_t>(base_offset) << 32));
				} else {
					const unsigned registers =
						(first == 31U ? 4U : 0U) |
						(second == 31U ? 2U : 0U) |
						(status == 31U ? 1U : 0U);
					emit(words, exclusive_pair_store_gadgets
						[ordering][size][registers]);
					const auto first_offset = first < 31U ?
						first * sizeof(std::uint64_t) : 0U;
					const auto second_offset = second < 31U ?
						second * sizeof(std::uint64_t) : 0U;
					const auto status_offset = status < 31U ?
						status * sizeof(std::uint64_t) : 0U;
					const auto base_offset = base < 31U ?
						base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
					emit(words, first_offset |
						(static_cast<std::uintptr_t>(second_offset) << 16) |
						(static_cast<std::uintptr_t>(status_offset) << 32) |
						(static_cast<std::uintptr_t>(base_offset) << 48));
				}
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0x3fa0'0000U) == 0x0800'0000U ||
			   (instruction & 0xbfa0'0000U) == 0x8900'0000U) {
			const bool load = (instruction & 0x0040'0000U) != 0;
			const unsigned transfer = instruction & 31U;
			const unsigned status = (instruction >> 16) & 31U;
			const bool valid = ((instruction >> 10) & 31U) == 31U &&
				(!load || status == 31U);
			if (valid) {
				const unsigned ordering =
					(instruction & 0x0000'8000U) != 0;
				const unsigned size = instruction >> 30;
				const unsigned base = (instruction >> 5) & 31U;
				if (base == 31U)
					emit(words, load ?
						uml_arm64_gadget_atomic_sp_alignment_read :
						uml_arm64_gadget_atomic_sp_alignment_write);
				if (load) {
					emit(words, exclusive_load_gadgets[ordering]
						[size][transfer == 31U]);
					const auto transfer_offset = transfer < 31U ?
						transfer * sizeof(std::uint64_t) : 0U;
					const auto base_offset = base < 31U ?
						base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
					emit(words, transfer_offset |
						(static_cast<std::uintptr_t>(base_offset) << 16));
				} else {
					const unsigned registers =
						(transfer == 31U ? 2U : 0U) |
						(status == 31U ? 1U : 0U);
					emit(words, exclusive_store_gadgets[ordering]
						[size][registers]);
					const auto transfer_offset = transfer < 31U ?
						transfer * sizeof(std::uint64_t) : 0U;
					const auto status_offset = status < 31U ?
						status * sizeof(std::uint64_t) : 0U;
					const auto base_offset = base < 31U ?
						base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
					emit(words, transfer_offset |
						(static_cast<std::uintptr_t>(status_offset) << 16) |
						(static_cast<std::uintptr_t>(base_offset) << 32));
				}
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0x3b00'0000U) == 0x3900'0000U &&
			   (instruction & 0x0400'0000U) == 0) {
			const auto size_code = instruction >> 30;
			const auto operation = (instruction >> 22) & 3U;
			if (size_code == 3 && operation == 2) {
				/* PRFM is architecturally only a hint. */
				emit(words, uml_arm64_gadget_nop);
			} else if ((size_code >= 2 && operation == 3) ||
				   (size_code == 3 && operation > 1)) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			} else {
				const unsigned transfer = instruction & 31U;
				const unsigned base = (instruction >> 5) & 31U;
				emit(words, scalar_unsigned_memory_gadget(size_code,
					operation, base == 31U, transfer == 31U));
				const auto transfer_offset = transfer < 31U ?
					transfer * sizeof(std::uint64_t) : 0U;
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				const auto offset =
					((instruction >> 10) & 0xfffU) << size_code;
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					(static_cast<std::uintptr_t>(offset) << 32));
				emit(words, address);
			}
		} else if ((instruction & 0x3b00'0000U) == 0x3900'0000U) {
			const auto size_code = instruction >> 30;
			const auto operation = (instruction >> 22) & 3U;
			if (operation >= 2 && size_code != 0) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			} else {
				const unsigned transfer = instruction & 31U;
				const unsigned base = (instruction >> 5) & 31U;
				emit(words, vector_unsigned_memory_gadget(
					size_code, operation, base == 31U));
				const auto transfer_offset = offsetof(CpuState, vectors) +
					transfer * sizeof(VectorRegister);
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				const unsigned scale = operation >= 2U ? 4U : size_code;
				const auto offset =
					((instruction >> 10) & 0xfffU) << scale;
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					(static_cast<std::uintptr_t>(offset) << 32));
				emit(words, address);
			}
		} else if ((instruction & 0x3b00'0000U) == 0x3800'0000U &&
			   (instruction & 0x0400'0000U) == 0) {
			const auto size_code = instruction >> 30;
			const auto operation = (instruction >> 22) & 3U;
			const bool prefetch = size_code == 3U && operation == 2U;
			const bool valid_transfer =
				(size_code != 3 || operation <= 1) &&
				(size_code != 2 || operation != 3);
			const bool register_offset =
				(instruction & 0x0020'0000U) != 0;
			const auto option = (instruction >> 13) & 7U;
			const auto mode = (instruction >> 10) & 3U;
			const bool valid_offset = !register_offset ||
				(mode == 2U && (option & 2U) != 0);
			const bool valid_prefetch = prefetch && (register_offset ?
				(mode == 2U && (option & 2U) != 0) : mode == 0U);
			if (valid_prefetch) {
				emit(words, uml_arm64_gadget_nop);
			} else if (prefetch) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			} else if (valid_transfer && valid_offset) {
				const auto *gadgets =
					scalar_other_gadgets(size_code, operation);
				const unsigned transfer = instruction & 31U;
				const unsigned base = (instruction >> 5) & 31U;
				const auto transfer_offset = transfer < 31U ?
					transfer * sizeof(std::uint64_t) : 0U;
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				if (register_offset) {
					const unsigned offset_register =
						(instruction >> 16) & 31U;
					const auto offset_register_offset = offset_register < 31U ?
						offset_register * sizeof(std::uint64_t) :
						offsetof(CpuState, gadget_zero);
					const unsigned extension = option == 2U ? 0U :
						option == 6U ? 2U : 1U;
					const unsigned address_mode = extension +
						((instruction & 0x1000U) != 0 ? 3U : 0U);
					emit(words, gadgets->register_offset[address_mode]
						[base == 31U][transfer == 31U]);
					emit(words, transfer_offset |
						(static_cast<std::uintptr_t>(base_offset) << 16) |
						(static_cast<std::uintptr_t>(
							offset_register_offset) << 32));
				} else {
					const bool overlap = operation != 0U && base != 31U &&
						base == transfer;
					const unsigned address_mode = mode == 1U ?
						(overlap ? 3U : 2U) : mode == 3U ?
						(overlap ? 0U : 1U) : 0U;
					emit(words, gadgets->immediate[address_mode]
						[base == 31U][transfer == 31U]);
					const auto displacement = sign_extend(
						(instruction >> 12) & 0x1ffU, 9);
					emit(words, transfer_offset |
						(static_cast<std::uintptr_t>(base_offset) << 16) |
						(static_cast<std::uint64_t>(displacement) << 32));
				}
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0x3b00'0000U) == 0x3800'0000U) {
			const auto size_code = instruction >> 30;
			const auto operation = (instruction >> 22) & 3U;
			const bool valid_transfer = operation < 2 || size_code == 0;
			const bool register_offset =
				(instruction & 0x0020'0000U) != 0;
			const auto option = (instruction >> 13) & 7U;
			const auto mode = (instruction >> 10) & 3U;
			const bool valid_offset = register_offset ?
				(mode == 2U && (option & 2U) != 0) : mode != 2U;
			if (valid_transfer && valid_offset) {
				const auto *gadgets =
					vector_other_gadgets(size_code, operation);
				const unsigned transfer = instruction & 31U;
				const unsigned base = (instruction >> 5) & 31U;
				const auto transfer_offset = offsetof(CpuState, vectors) +
					transfer * sizeof(VectorRegister);
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				if (register_offset) {
					const unsigned offset_register =
						(instruction >> 16) & 31U;
					const auto offset_register_offset =
						offset_register < 31U ?
						offset_register * sizeof(std::uint64_t) :
						offsetof(CpuState, gadget_zero);
					const unsigned extension = option == 2U ? 0U :
						option == 6U ? 2U : 1U;
					const unsigned address_mode = extension +
						((instruction & 0x1000U) != 0 ? 3U : 0U);
					emit(words, gadgets->register_offset[address_mode]
						[base == 31U]);
					emit(words, transfer_offset |
						(static_cast<std::uintptr_t>(base_offset) << 16) |
						(static_cast<std::uintptr_t>(
							offset_register_offset) << 32));
				} else {
					const unsigned address_mode = mode == 1U ? 2U :
						mode == 3U ? 1U : 0U;
					emit(words, gadgets->immediate[address_mode]
						[base == 31U]);
					const auto displacement = sign_extend(
						(instruction >> 12) & 0x1ffU, 9);
					emit(words, transfer_offset |
						(static_cast<std::uintptr_t>(base_offset) << 16) |
						(static_cast<std::uint64_t>(displacement) << 32));
				}
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0x3a00'0000U) == 0x2800'0000U &&
			   (instruction & 0x0400'0000U) == 0) {
			const auto operation = instruction >> 30;
			const bool load = (instruction & 0x0040'0000U) != 0;
			const auto mode = (instruction >> 23) & 3U;
			const auto first = instruction & 31U;
			const auto second = (instruction >> 10) & 31U;
			const bool valid =
				(operation != 1U || (load && mode != 0U)) &&
				(!load || first != second);
			if (valid) {
				const unsigned base = (instruction >> 5) & 31U;
				const bool overlap = load && base != 31U &&
					(base == first || base == second);
				const unsigned address_mode = mode == 1U ?
					(overlap ? 3U : 2U) : mode == 3U ?
					(overlap ? 0U : 1U) : 0U;
				const unsigned register_mode =
					(first == 31U ? 2U : 0U) +
					(second == 31U ? 1U : 0U);
				const auto *gadgets =
					integer_pair_gadgets(operation, load);
				const auto first_offset = first < 31U ?
					first * sizeof(std::uint64_t) : 0U;
				const auto second_offset = second < 31U ?
					second * sizeof(std::uint64_t) : 0U;
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				const auto displacement = sign_extend(
					(instruction >> 15) & 0x7fU, 7) <<
					(operation >= 2U ? 3U : 2U);
				emit(words, (*gadgets)[address_mode][base == 31U]
					[register_mode]);
				emit(words, first_offset |
					(static_cast<std::uintptr_t>(second_offset) << 16) |
					(static_cast<std::uintptr_t>(base_offset) << 32) |
					((static_cast<std::uint64_t>(displacement) & 0xffffU)
						<< 48));
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else if ((instruction & 0x3a00'0000U) == 0x2800'0000U &&
			   (instruction & 0x0400'0000U) != 0) {
			const auto operation = instruction >> 30;
			const bool load = (instruction & 0x0040'0000U) != 0;
			const auto first = instruction & 31U;
			const auto second = (instruction >> 10) & 31U;
			if (!load || first != second) {
				const unsigned base = (instruction >> 5) & 31U;
				const unsigned mode = (instruction >> 23) & 3U;
				const unsigned address_mode = mode == 1U ? 2U :
					mode == 3U ? 1U : 0U;
				const auto &gadgets =
					vector_pair_gadgets(operation, load);
				const auto first_offset = offsetof(CpuState, vectors) +
					first * sizeof(VectorRegister);
				const auto second_offset = offsetof(CpuState, vectors) +
					second * sizeof(VectorRegister);
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				const auto displacement = sign_extend(
					(instruction >> 15) & 0x7fU, 7) <<
					(operation < 2U ? operation + 2U : 4U);
				emit(words, gadgets[address_mode][base == 31U]);
				emit(words, first_offset |
					(static_cast<std::uintptr_t>(second_offset) << 16) |
					(static_cast<std::uintptr_t>(base_offset) << 32) |
					((static_cast<std::uint64_t>(displacement) & 0xffffU)
						<< 48));
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return {Program{guest_address, compiled, std::move(words),
					program_accesses_fp_state},
					true, address, instruction};
			}
		} else {
			emit_fatal(words, address);
		}
		goto instruction_decoded;

decode_exception:
		if ((instruction & 0xffe0'001fU) == 0xd420'0000U) {
			emit(words, uml_arm64_gadget_breakpoint);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0xffe0'001fU) == 0xd400'0002U ||
			   (instruction & 0xffe0'001fU) == 0xd400'0003U ||
			   (instruction & 0xffe0'001fU) == 0xd440'0000U ||
			   (instruction & 0xffe0'001fU) == 0xd4a0'0001U ||
			   (instruction & 0xffe0'001fU) == 0xd4a0'0002U ||
			   (instruction & 0xffe0'001fU) == 0xd4a0'0003U ||
			   instruction == 0xd6bf'03e0U ||
			   instruction == 0xd69f'03e0U ||
			   instruction == 0xd69f'0bffU ||
			   instruction == 0xd69f'0fffU) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else if ((instruction & 0xffe0'001fU) == 0xd400'0001U) {
			emit(words, uml_arm64_gadget_svc);
			emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		} else {
			emit_fatal(words, address);
		}
instruction_decoded:
		++compiled;
	}
	const auto fallthrough = guest_address + compiled * sizeof(std::uint32_t);
	if (resolve_link) {
		emit(words, uml_arm64_gadget_branch_linked);
		emit(words, reinterpret_cast<std::uintptr_t>(
			resolve_link(fallthrough)));
	} else {
		emit(words, uml_arm64_gadget_interrupt);
		emit(words, fallthrough);
	}
	return {Program{guest_address, compiled, std::move(words),
		program_accesses_fp_state}, true,
		guest_address + compiled * sizeof(std::uint32_t), 0};
}

::uml::cpu::EngineEvent execute(const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb) noexcept
{
	std::size_t executed = 0;
	return execute(program, cpu, tlb, executed);
}

::uml::cpu::EngineEvent execute(const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
	      std::size_t &executed) noexcept
{
	executed = 0;
	if (program.words().empty())
		return ::uml::cpu::EngineEvent::fatal;
	if (program.accesses_fp_state())
		cpu.access_fp_state();
	const auto result =
		uml_arm64_gadget_enter(program.words().data(), &cpu, &tlb,
			program.instruction_count());
	restore_native_fp_environment(cpu);
	executed = result.executed;
	return static_cast<::uml::cpu::EngineEvent>(result.event);
}

::uml::cpu::EngineEvent execute_preserving_native_fp_environment(
	const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
	std::size_t &executed) noexcept
{
	executed = 0;
	if (program.words().empty())
		return ::uml::cpu::EngineEvent::fatal;
	if (program.accesses_fp_state())
		cpu.access_fp_state();
	const auto result =
		uml_arm64_gadget_enter(program.words().data(), &cpu, &tlb,
			program.instruction_count());
	executed = result.executed;
	return static_cast<::uml::cpu::EngineEvent>(result.event);
}

} // namespace uml::cpu::arm64::gadgets
