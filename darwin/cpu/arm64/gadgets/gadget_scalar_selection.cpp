// SPDX-License-Identifier: GPL-2.0-or-later
#include "gadget_selection_dependencies.hpp"

namespace uml::cpu::arm64::gadgets::selection {

struct GcsGadget final {
	Gadget entry{};
	std::uintptr_t operand{};
	bool has_operand{};
	bool always_traps{};
};

[[nodiscard, maybe_unused]] std::optional<GcsGadget>
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
extern constexpr std::array<std::array<Gadget, 2>, 8> pointer_auth_gadgets{{
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

[[nodiscard]] std::optional<PointerAuthOperation>
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

[[nodiscard]] std::uintptr_t pointer_auth_operand(
	unsigned destination, unsigned modifier) noexcept
{
	const auto destination_offset = destination * sizeof(std::uint64_t);
	const auto modifier_offset = modifier < 31 ?
		modifier * sizeof(std::uint64_t) : offsetof(CpuState, sp);
	return destination_offset |
		(static_cast<std::uintptr_t>(modifier_offset) << 16);
}

[[nodiscard]] std::uintptr_t pointer_auth_generic_operand(
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

[[nodiscard]] std::uintptr_t pointer_auth_load_operand(
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

[[nodiscard]] bool pointer_auth_lr_unavailable(
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

[[nodiscard]] bool cmpbr_immediate_zero_result(
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

[[nodiscard]] bool cmpbr_equal_register_result(unsigned cc) noexcept
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

[[nodiscard]] std::uint16_t ls64_register_offset(
	unsigned index) noexcept
{
	static_assert(offsetof(CpuState, registers) == 0);
	return static_cast<std::uint16_t>(
		offsetof(CpuState, registers) + index * sizeof(std::uint64_t));
}

[[nodiscard]] std::uintptr_t ls64_operand(unsigned first,
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

[[nodiscard]] PstateImmediateGadget pstate_boolean_gadget(
	Gadget zero, Gadget one, unsigned immediate) noexcept
{
	return {(immediate & 1U) != 0 ? one : zero, true, false};
}

extern constexpr std::array<Gadget, 16> pstate_daif_set_gadgets{
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

extern constexpr std::array<Gadget, 16> pstate_daif_clear_gadgets{
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

[[nodiscard]] PstateImmediateGadget pstate_immediate_gadget(
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

[[nodiscard]] SystemRegisterGadget system_register_gadget(
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

[[nodiscard]] std::uint16_t mte_cpu_slot_offset(
	unsigned index, bool stack_pointer) noexcept
{
	static_assert(offsetof(CpuState, registers) == 0);
	static_assert(offsetof(CpuState, sp) == 248);
	return static_cast<std::uint16_t>(index < 31 ? index * 8U :
		stack_pointer ? offsetof(CpuState, sp) : 0U);
}

[[nodiscard]] std::uintptr_t mte_memory_operand(unsigned base,
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

[[nodiscard]] auto make_byte_popcount_table() noexcept
{
	std::array<std::uint8_t, 256> table{};
	for (unsigned value = 0; value < table.size(); ++value)
		table[value] = static_cast<std::uint8_t>(std::popcount(value));
	return table;
}

extern const auto byte_popcount_table = make_byte_popcount_table();

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

} // namespace uml::cpu::arm64::gadgets::selection
