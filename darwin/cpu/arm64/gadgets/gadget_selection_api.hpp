// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gadget_selection_dependencies.hpp"

namespace uml::cpu::arm64::gadgets::selection {

template <typename Words>
void emit(Words &words, Gadget gadget) {
  words.push_back(reinterpret_cast<std::uintptr_t>(gadget));
}

template <typename Words>
void emit(Words &words, std::uintptr_t operand) {
  words.push_back(operand);
}

using ScalarOtherRegisterChoices = std::array<Gadget, 2>;

using ScalarOtherBaseChoices = std::array<ScalarOtherRegisterChoices, 2>;

using ScalarOtherImmediateModes = std::array<ScalarOtherBaseChoices, 4>;

using ScalarOtherRegisterModes = std::array<ScalarOtherBaseChoices, 6>;

struct ScalarOtherGadgets final {
	ScalarOtherImmediateModes immediate;
	ScalarOtherRegisterModes register_offset;
};

using VectorOtherBaseModes = std::array<Gadget, 2>;

using VectorOtherImmediateModes = std::array<VectorOtherBaseModes, 3>;

using VectorOtherRegisterModes = std::array<VectorOtherBaseModes, 6>;

struct VectorOtherGadgets final {
	VectorOtherImmediateModes immediate;
	VectorOtherRegisterModes register_offset;
};

using IntegerPairRegisterModes = std::array<Gadget, 4>;

using IntegerPairBaseModes = std::array<IntegerPairRegisterModes, 2>;

using IntegerPairAddressModes = std::array<IntegerPairBaseModes, 4>;

using VectorPairBaseModes = std::array<Gadget, 2>;

using VectorPairAddressModes = std::array<VectorPairBaseModes, 3>;

using SimdStructureAddressModes = std::array<Gadget, 3>;

using SimdStructureBases = std::array<SimdStructureAddressModes, 2>;

struct SimdStructureGadgets final {
	SimdStructureBases store;
	SimdStructureBases load;
};

using SimdReplicateAddressModes = std::array<Gadget, 3>;

using SimdReplicateBases = std::array<SimdReplicateAddressModes, 2>;

using SimdReplicateCounts = std::array<SimdReplicateBases, 4>;

using SimdLaneAddressModes = std::array<Gadget, 3>;

using SimdLaneBases = std::array<SimdLaneAddressModes, 2>;

struct SimdLaneGadgets final {
	SimdLaneBases store;
	SimdLaneBases load;
};

using SimdLaneCounts = std::array<SimdLaneGadgets, 4>;

using AtomicFloatingVariants = std::array<Gadget, 6>;

using AtomicFloatingBases = std::array<AtomicFloatingVariants, 2>;

using AtomicFloatingOperations = std::array<AtomicFloatingBases, 5>;

using AtomicFloatingGadgets = std::array<AtomicFloatingOperations, 4>;

using AtomicRegisterGadgets = std::array<Gadget, 4>;

using AtomicSizeGadgets = std::array<AtomicRegisterGadgets, 4>;

using AtomicOrderGadgets = std::array<AtomicSizeGadgets, 4>;

using AtomicOperationGadgets = std::array<AtomicOrderGadgets, 9>;

using AtomicPair128OrderGadgets = std::array<Gadget, 4>;

using AtomicPair128Gadgets = std::array<AtomicPair128OrderGadgets, 3>;

using CompareSwapGadgets = std::array<AtomicSizeGadgets, 4>;

using CompareSwapPairSizeGadgets = std::array<AtomicRegisterGadgets, 2>;

using CompareSwapPairGadgets = std::array<CompareSwapPairSizeGadgets, 4>;

using RcwScalarOrderingGadgets = std::array<AtomicRegisterGadgets, 4>;

using RcwScalarSoftGadgets = std::array<RcwScalarOrderingGadgets, 2>;

using RcwScalarGadgets = std::array<RcwScalarSoftGadgets, 4>;

using RcwPairCasOrderingGadgets = std::array<AtomicRegisterGadgets, 4>;

using RcwPairCasGadgets = std::array<RcwPairCasOrderingGadgets, 2>;

using RcwPairModifyOrderingGadgets = std::array<Gadget, 4>;

using RcwPairModifySoftGadgets =
	std::array<RcwPairModifyOrderingGadgets, 2>;

using RcwPairModifyGadgets = std::array<RcwPairModifySoftGadgets, 3>;

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

using OrderedLoadGadgets = ExclusiveLoadGadgets;

using OrderedStoreGadgets = ExclusiveLoadGadgets;

using RcpcRegisterGadgets = std::array<Gadget, 2>;

using RcpcLoadGadgets = std::array<RcpcRegisterGadgets, 9>;

using RcpcStoreGadgets = std::array<RcpcRegisterGadgets, 4>;

using SimdOrderedAddressGadgets = std::array<Gadget, 2>;

using SimdOrderedScalarGadgets =
	std::array<SimdOrderedAddressGadgets, 4>;

using SimdOrderedImplementationGadgets =
	std::array<SimdOrderedAddressGadgets, 2>;

using SimdOrderedLaneGadgets =
	std::array<std::array<SimdOrderedAddressGadgets, 2>, 2>;

using OrderedPairRegisterGadgets = std::array<Gadget, 4>;

using OrderedPairAddressGadgets =
	std::array<OrderedPairRegisterGadgets, 2>;

using OrderedPairGadgets = std::array<OrderedPairAddressGadgets, 2>;

using ConsistentPairGadgets = std::array<Gadget, 4>;

struct GcsGadget final {
	Gadget entry{};
	std::uintptr_t operand{};
	bool has_operand{};
	bool always_traps{};
};

struct PointerAuthOperation final {
	unsigned operation;
	unsigned destination;
	unsigned modifier;
	bool zero_modifier;
};

struct SystemRegisterGadget final {
	Gadget gadget;
	bool operand;
};

struct PstateImmediateGadget final {
	Gadget gadget;
	bool valid;
	bool fault_address;
};

using CpaModeGadgets = std::array<Gadget, 8>;

using CpaShiftGadgets = std::array<CpaModeGadgets, 8>;

using SimdThreeSameGadgets = std::array<Gadget, 11>;

enum class SimdDotSignedness {
	signed_signed,
	unsigned_unsigned,
	signed_unsigned,
	unsigned_signed,
};

using SimdWideningGadgets = std::array<Gadget, 6>;

using SimdUnaryGadgets = std::array<Gadget, 11>;

struct SimdImmediateShift final {
	unsigned lane_bits;
	unsigned shift;
	unsigned shape;
};

enum class SimdSaturatingShiftLeft {
	signed_to_signed,
	signed_to_unsigned,
	unsigned_to_unsigned,
};

struct SimdIndexedElement final {
	unsigned register_index;
	unsigned lane_index;
};

extern const AtomicFloatingGadgets atomic_floating_native_gadgets;
extern const AtomicFloatingGadgets atomic_floating_software_gadgets;
extern const AtomicOperationGadgets atomic_rmw_gadgets;
extern const AtomicPair128Gadgets atomic_pair128_gadgets;
extern const CompareSwapGadgets compare_swap_gadgets;
extern const RcwScalarGadgets rcw_scalar_gadgets;
extern const RcwPairCasGadgets rcw_pair_cas_gadgets;
extern const RcwPairModifyGadgets rcw_pair_modify_gadgets;
extern const CompareSwapPairGadgets compare_swap_pair_gadgets;
extern const ExclusiveLoadGadgets exclusive_load_gadgets;
extern const ExclusiveStoreGadgets exclusive_store_gadgets;
extern const ExclusivePairLoadGadgets exclusive_pair_load_gadgets;
extern const ExclusivePairStoreGadgets exclusive_pair_store_gadgets;
extern const OrderedLoadGadgets ordered_load_gadgets;
extern const OrderedStoreGadgets ordered_store_gadgets;
extern const RcpcLoadGadgets rcpc_load_gadgets;
extern const RcpcStoreGadgets rcpc_store_gadgets;
extern const SimdOrderedScalarGadgets simd_ordered_scalar_load_gadgets;
extern const SimdOrderedScalarGadgets simd_ordered_scalar_store_gadgets;
extern const SimdOrderedImplementationGadgets simd_ordered_q_load_gadgets;
extern const SimdOrderedImplementationGadgets simd_ordered_q_store_gadgets;
extern const SimdOrderedLaneGadgets simd_ordered_lane_gadgets;
extern const OrderedPairGadgets ordered_pair_load_gadgets;
extern const OrderedPairGadgets ordered_pair_store_gadgets;
extern const ConsistentPairGadgets consistent_pair_ldap_gadgets;
extern const ConsistentPairGadgets consistent_pair_ldapp_gadgets;
extern const ConsistentPairGadgets consistent_pair_store_gadgets;
extern const std::array<std::array<Gadget, 2>, 8> pointer_auth_gadgets;
extern const std::array<Gadget, 16> pstate_daif_set_gadgets;
extern const std::array<Gadget, 16> pstate_daif_clear_gadgets;
extern const std::array<std::uint8_t, 256> byte_popcount_table;

[[nodiscard]] Gadget literal_load_gadget(bool, unsigned int, bool) noexcept;
[[nodiscard]] Gadget scalar_unsigned_memory_gadget(unsigned int, unsigned int, bool, bool) noexcept;
[[nodiscard]] Gadget scalar_unsigned_memory_inplace_gadget(unsigned int, unsigned int, bool, unsigned int) noexcept;
[[nodiscard]] Gadget vector_unsigned_memory_gadget(unsigned int, unsigned int, bool) noexcept;
[[nodiscard]] const ScalarOtherGadgets * scalar_other_gadgets(unsigned int, unsigned int) noexcept;
[[nodiscard]] const VectorOtherGadgets * vector_other_gadgets(unsigned int, unsigned int) noexcept;
[[nodiscard]] const IntegerPairAddressModes * integer_pair_gadgets(unsigned int, bool) noexcept;
[[nodiscard]] const VectorPairAddressModes & vector_pair_gadgets(unsigned int, bool) noexcept;
[[nodiscard]] const SimdStructureGadgets * simd_structure_gadgets(unsigned int, bool, unsigned int) noexcept;
[[nodiscard]] const SimdReplicateCounts & simd_replicate_gadgets(unsigned int, bool) noexcept;
[[nodiscard]] const SimdLaneCounts & simd_lane_gadgets(unsigned int) noexcept;
[[nodiscard]] bool host_has_sm3() noexcept;
[[nodiscard]] bool host_has_sm4() noexcept;
[[nodiscard]] bool host_has_sha1() noexcept;
[[nodiscard]] bool host_has_sha256() noexcept;
[[nodiscard]] bool host_has_sha512() noexcept;
[[nodiscard]] bool host_has_sha3() noexcept;
[[nodiscard]] bool host_has_aes() noexcept;
[[nodiscard]] bool host_has_pmull() noexcept;
[[nodiscard]] bool host_has_rdm() noexcept;
[[nodiscard]] bool host_has_dotprod() noexcept;
[[nodiscard]] bool host_has_i8mm() noexcept;
[[nodiscard]] bool host_has_lrcpc3() noexcept;
[[nodiscard]] bool host_has_lut() noexcept;
[[nodiscard]] bool host_has_frintts() noexcept;
[[nodiscard]] bool host_has_afp() noexcept;
[[nodiscard]] bool host_has_faminmax() noexcept;
[[nodiscard]] bool host_has_fp8() noexcept;
[[nodiscard]] bool host_has_fp8dot2() noexcept;
[[nodiscard]] bool host_has_fp8dot4() noexcept;
[[nodiscard]] bool host_has_fp8fma() noexcept;
[[nodiscard]] bool host_has_f8f16mm() noexcept;
[[nodiscard]] bool host_has_f8f32mm() noexcept;
[[nodiscard]] bool host_has_bf16() noexcept;
[[nodiscard]] bool host_has_ebf16() noexcept;
[[nodiscard]] bool host_has_fhm() noexcept;
[[nodiscard]] bool host_has_f16mm() noexcept;
[[nodiscard]] bool host_has_f16f32mm() noexcept;
[[nodiscard]] bool host_has_f16f32dot() noexcept;
[[nodiscard]] bool host_has_lsfe() noexcept;
[[nodiscard]] std::optional<GcsGadget> gcs_gadget(std::uint32_t) noexcept;
[[nodiscard]] std::optional<PointerAuthOperation> decode_pointer_auth_hint(std::uint32_t) noexcept;
[[nodiscard]] std::uintptr_t pointer_auth_operand(unsigned int, unsigned int) noexcept;
[[nodiscard]] std::uintptr_t pointer_auth_generic_operand(unsigned int, unsigned int, unsigned int) noexcept;
[[nodiscard]] Gadget pointer_auth_branch_gadget(bool, bool, bool, bool) noexcept;
[[nodiscard]] Gadget pointer_auth_guarded_call_gadget(bool, bool, bool) noexcept;
[[nodiscard]] Gadget pointer_auth_guarded_return_gadget(bool) noexcept;
[[nodiscard]] Gadget pointer_auth_load_gadget(bool, bool, bool, bool) noexcept;
[[nodiscard]] std::uintptr_t pointer_auth_load_operand(unsigned int, unsigned int, std::int64_t) noexcept;
[[nodiscard]] bool pointer_auth_lr_unavailable(std::uint32_t) noexcept;
/* Fused guest CMP of two registers + B.cond. */
[[nodiscard]] Gadget compare_register_branch_gadget(
	bool wide, unsigned int condition, bool linked) noexcept;

/* Fused guest CMP-immediate + B.cond. Returns nullptr for a condition with no
 * fused entry, which is the caller's signal to emit the two gadgets. */
[[nodiscard]] Gadget compare_immediate_branch_gadget(
	bool wide, unsigned int condition, bool linked) noexcept;
[[nodiscard]] Gadget conditional_branch_gadget(unsigned int) noexcept;
[[nodiscard]] Gadget conditional_branch_linked_gadget(unsigned int) noexcept;
[[nodiscard]] Gadget conditional_branch_inline_gadget(unsigned int) noexcept;
[[nodiscard]] Gadget compare_branch_inline_gadget(bool, bool) noexcept;
[[nodiscard]] Gadget test_branch_inline_gadget(bool) noexcept;
[[nodiscard]] Gadget compare_immediate_branch_inline_gadget(
	bool, unsigned int) noexcept;
[[nodiscard]] Gadget compare_register_branch_inline_gadget(
	bool, unsigned int) noexcept;
[[nodiscard]] Gadget compare_branch_gadget(bool, bool, bool) noexcept;
[[nodiscard]] Gadget test_branch_gadget(bool, bool) noexcept;
[[nodiscard]] Gadget cmpbr_immediate_gadget(bool, unsigned int) noexcept;
[[nodiscard]] Gadget cmpbr_register_gadget(unsigned int, unsigned int, bool, bool) noexcept;
[[nodiscard]] bool cmpbr_immediate_zero_result(unsigned int, unsigned int) noexcept;
[[nodiscard]] bool cmpbr_equal_register_result(unsigned int) noexcept;
[[nodiscard]] Gadget ls64_gadget(unsigned int, bool, bool) noexcept;
[[nodiscard]] std::uint16_t ls64_register_offset(unsigned int) noexcept;
[[nodiscard]] std::uintptr_t ls64_operand(unsigned int, unsigned int, unsigned int, bool, bool) noexcept;
[[nodiscard]] Gadget mops_gadget(unsigned int, unsigned int, bool) noexcept;
[[nodiscard]] Gadget mops_set_tag_gadget(unsigned int, bool) noexcept;
[[nodiscard]] PstateImmediateGadget pstate_boolean_gadget(Gadget, Gadget, unsigned int) noexcept;
[[nodiscard]] PstateImmediateGadget pstate_immediate_gadget(std::uint32_t) noexcept;
[[nodiscard]] SystemRegisterGadget system_register_gadget(std::uint32_t) noexcept;
[[nodiscard]] Gadget mte_load_tag_gadget(bool, bool) noexcept;
[[nodiscard]] Gadget mte_tag_store_gadget(unsigned int, unsigned int, bool) noexcept;
[[nodiscard]] Gadget mte_store_pair_tag_gadget(unsigned int, bool, bool, bool) noexcept;
[[nodiscard]] std::uint16_t mte_cpu_slot_offset(unsigned int, bool) noexcept;
[[nodiscard]] std::uintptr_t mte_memory_operand(unsigned int, unsigned int, unsigned int, std::int16_t, bool) noexcept;
[[nodiscard]] Gadget conditional_compare_gadget(unsigned int, bool, bool, bool) noexcept;
[[nodiscard]] Gadget conditional_select_gadget(unsigned int, unsigned int, bool) noexcept;
[[nodiscard]] Gadget data_memory_barrier_gadget(bool, unsigned int) noexcept;
[[nodiscard]] Gadget memory_barrier_gadget(std::uint32_t) noexcept;
[[nodiscard]] Gadget architectural_hint_gadget(std::uint32_t) noexcept;
[[nodiscard]] Gadget integer_min_max_gadget(unsigned int, bool, bool) noexcept;
[[nodiscard]] Gadget add_sub_immediate_gadget(bool, bool, bool, bool, bool) noexcept;
#if UML_GADGET_INPLACE_IMMEDIATE
[[nodiscard]] Gadget add_sub_immediate_inplace_gadget(bool, bool, bool, unsigned int) noexcept;
[[nodiscard]] Gadget logical_immediate_inplace_gadget(bool, unsigned int, unsigned int) noexcept;
[[nodiscard]] Gadget move_wide_keep_inplace_gadget(bool, unsigned int, unsigned int) noexcept;
[[nodiscard]] Gadget add_sub_register_inplace_gadget(bool, bool, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_fused_inplace_gadget(bool, unsigned int, unsigned int) noexcept;
[[nodiscard]] Gadget fused_floating_inplace_gadget(const detail::FusedFloatingArithmetic &, unsigned int) noexcept;
#endif
[[nodiscard]] Gadget add_sub_register_gadget(bool, bool, bool, bool, unsigned int, unsigned int, bool, bool, bool, bool) noexcept;
[[nodiscard]] Gadget add_sub_carry_gadget(bool, bool, bool, bool, bool, bool, bool) noexcept;
[[nodiscard]] Gadget variable_shift_gadget(bool, unsigned int, bool, bool, bool) noexcept;
[[nodiscard]] Gadget divide_gadget(bool, bool, bool, bool, bool) noexcept;
[[nodiscard]] Gadget logical_immediate_gadget(bool, unsigned int, bool, bool) noexcept;
[[nodiscard]] Gadget logical_shifted_gadget(bool, unsigned int, bool, unsigned int, bool, bool, bool) noexcept;
[[nodiscard]] Gadget multiply_add_gadget(unsigned int, bool, bool, bool, bool, bool) noexcept;
[[nodiscard]] Gadget bitfield_gadget(bool, unsigned int, bool, bool) noexcept;
[[nodiscard]] Gadget extract_register_gadget(bool, bool, bool, bool, bool) noexcept;
[[nodiscard]] Gadget crc32_gadget(unsigned int, bool, bool, bool, bool) noexcept;
[[nodiscard]] Gadget bit_reverse_count_gadget(bool, unsigned int) noexcept;
[[nodiscard]] Gadget mte_pointer_gadget(unsigned int, bool, bool, bool) noexcept;
[[nodiscard]] Gadget mte_tag_add_sub_gadget(bool, bool, bool) noexcept;
[[nodiscard]] Gadget mte_irg_gadget(bool, bool, bool) noexcept;
[[nodiscard]] std::array<std::uint8_t, 256> make_byte_popcount_table() noexcept;
[[nodiscard]] Gadget cpa_pointer_gadget(bool, unsigned int, bool, bool, bool) noexcept;
[[nodiscard]] Gadget cpa_multiply_gadget(bool, bool, bool, bool, bool) noexcept;
[[nodiscard]] Gadget simd_extract_gadget(bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_three_same_gadget(unsigned int, bool, bool, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_destination_resident_mla_gadget(bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_rdm_three_same_gadget(bool, bool, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_dot_vector_gadget(SimdDotSignedness, bool, bool) noexcept;
[[nodiscard]] Gadget simd_dot_indexed_gadget(SimdDotSignedness, bool, unsigned int, bool) noexcept;
[[nodiscard]] Gadget simd_matrix_gadget(SimdDotSignedness, bool) noexcept;
[[nodiscard]] Gadget simd_widening_gadget(unsigned int, bool, bool, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_high_narrow_gadget(bool, bool, bool, unsigned int) noexcept;
[[nodiscard]] std::uintptr_t simd_unary_operand(unsigned int, unsigned int) noexcept;
[[nodiscard]] std::uintptr_t
simd_ternary_operand(unsigned int, unsigned int, unsigned int,
                     unsigned int = 0U) noexcept;
[[nodiscard]] std::uintptr_t
simd_quaternary_operand(unsigned int, unsigned int, unsigned int,
                        unsigned int) noexcept;
[[nodiscard]] Gadget simd_integer_unary_gadget(unsigned int, bool, bool, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_compare_zero_native_gadget(unsigned int, bool, bool, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_floating_compare_gadget(const detail::SimdFloatingCompareDecode &) noexcept;
[[nodiscard]] Gadget simd_floating_precision_vector_gadget(const detail::SimdFloatingPrecisionVectorDecode &) noexcept;
[[nodiscard]] Gadget floating_to_integer_gadget(const detail::FloatingToIntegerDecode &, bool) noexcept;
[[nodiscard]] Gadget integer_to_floating_gadget(const detail::IntegerToFloatingDecode &, bool) noexcept;
[[nodiscard]] Gadget simd_reverse_native_gadget(unsigned int, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_byte_unary_gadget(unsigned int, bool) noexcept;
[[nodiscard]] Gadget simd_integer_reduce_gadget(unsigned int, bool, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_unsigned_estimate_gadget(bool, bool, bool) noexcept;
[[nodiscard]] Gadget floating_arithmetic_gadget(unsigned int, bool, unsigned int) noexcept;
[[nodiscard]] Gadget basic_floating_arithmetic_gadget(const detail::BasicFloatingArithmetic &) noexcept;
[[nodiscard]] unsigned int floating_round_operation_index(detail::FloatingRoundOperation) noexcept;
[[nodiscard]] Gadget floating_round_gadget(const detail::FloatingRound &) noexcept;
[[nodiscard]] Gadget floating_horizontal_gadget(const detail::FloatingHorizontal &) noexcept;
[[nodiscard]] Gadget floating_scale_gadget(const detail::FloatingScale &) noexcept;
[[nodiscard]] Gadget fp8_gadget(const detail::Fp8Instruction &) noexcept;
[[nodiscard]] Gadget bfloat_conversion_gadget(const detail::BFloatConversion &) noexcept;
[[nodiscard]] Gadget bfloat_multiply_long_gadget(const detail::BFloatMultiplyLong &) noexcept;
[[nodiscard]] Gadget bfloat_dot_gadget(const detail::BFloatDot &) noexcept;
[[nodiscard]] Gadget bfloat_matrix_gadget() noexcept;
[[nodiscard]] Gadget half_fused_long_gadget(const detail::HalfFusedLong &) noexcept;
[[nodiscard]] Gadget half_matrix_gadget(const detail::HalfMatrix &) noexcept;
[[nodiscard]] Gadget half_dot_gadget(const detail::HalfDot &) noexcept;
[[nodiscard]] Gadget floating_reciprocal_gadget(const detail::FloatingReciprocal &) noexcept;
[[nodiscard]] Gadget floating_complex_gadget(const detail::FloatingComplex &) noexcept;
[[nodiscard]] std::uintptr_t floating_complex_operand(std::uint32_t, const detail::FloatingComplex &) noexcept;
[[nodiscard]] std::uint64_t expand_vfp_immediate(std::uint8_t, unsigned int) noexcept;
[[nodiscard]] Gadget simd_floating_sign_gadget(bool, bool, bool, bool) noexcept;
[[nodiscard]] Gadget floating_multiply_add_gadget(bool, bool, bool) noexcept;
[[nodiscard]] Gadget fused_floating_arithmetic_gadget(const detail::FusedFloatingArithmetic &) noexcept;
[[nodiscard]] Gadget simd_bitwise_gadget(unsigned int, bool) noexcept;
[[nodiscard]] Gadget simd_move_gadget(bool) noexcept;
[[nodiscard]] Gadget simd_modified_immediate_gadget(unsigned int, bool) noexcept;
[[nodiscard]] Gadget simd_duplicate_element_gadget(bool, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_duplicate_general_gadget(bool, unsigned int, bool) noexcept;
[[nodiscard]] Gadget simd_insert_general_gadget(unsigned int, bool) noexcept;
[[nodiscard]] Gadget simd_insert_element_gadget(unsigned int) noexcept;
[[nodiscard]] Gadget simd_lane_to_gpr_gadget(bool, bool, unsigned int, bool) noexcept;
[[nodiscard]] std::optional<SimdImmediateShift> decode_simd_immediate_shift(std::uint32_t, bool) noexcept;
[[nodiscard]] Gadget simd_shift_right_immediate_gadget(unsigned int, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_shift_left_immediate_gadget(bool, unsigned int) noexcept;
[[nodiscard]] std::optional<SimdImmediateShift> decode_simd_saturating_shift_left(std::uint32_t) noexcept;
[[nodiscard]] Gadget simd_saturating_shift_left_gadget(SimdSaturatingShiftLeft, unsigned int, unsigned int) noexcept;
[[nodiscard]] Gadget simd_shift_narrow_gadget(unsigned int, bool, bool) noexcept;
[[nodiscard]] Gadget simd_saturating_narrow_gadget(unsigned int, bool, unsigned int, bool, bool) noexcept;
[[nodiscard]] Gadget simd_shift_left_long_gadget(unsigned int, bool, bool) noexcept;
[[nodiscard]] Gadget simd_indexed_same_gadget(unsigned int, unsigned int, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_indexed_long_gadget(unsigned int, bool, unsigned int, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_indexed_saturating_long_gadget(unsigned int, unsigned int, bool, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_indexed_saturating_high_gadget(unsigned int, unsigned int, bool, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_indexed_saturating_accumulate_high_gadget(unsigned int, unsigned int, bool, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_indexed_integer_gadget(unsigned int, bool, bool, bool, unsigned int, unsigned int) noexcept;
[[nodiscard]] Gadget simd_indexed_floating_gadget(unsigned int, bool, bool, bool, unsigned int, unsigned int) noexcept;
[[nodiscard]] Gadget simd_fmulx_gadget(bool, bool, bool, bool) noexcept;
[[nodiscard]] std::optional<SimdIndexedElement> decode_simd_indexed_element(std::uint32_t) noexcept;
[[nodiscard]] std::optional<SimdIndexedElement> decode_simd_indexed_floating_element(std::uint32_t) noexcept;
[[nodiscard]] SimdIndexedElement decode_simd_dot_indexed_element(std::uint32_t) noexcept;
[[nodiscard]] bool is_simd_indexed_integer_group(unsigned int, bool, bool) noexcept;
[[nodiscard]] bool is_simd_indexed_scalar_reserved(unsigned int, bool, unsigned int) noexcept;
[[nodiscard]] std::uint64_t repeat_simd_element(std::uint64_t, unsigned int) noexcept;
[[nodiscard]] std::uint64_t simd_shift_insert_mask(unsigned int, unsigned int, bool) noexcept;
[[nodiscard]] Gadget simd_permute_gadget(unsigned int, bool, unsigned int) noexcept;
[[nodiscard]] Gadget simd_table_lookup_gadget(bool, unsigned int, bool) noexcept;
[[nodiscard]] Gadget simd_lut_native_gadget(unsigned int, unsigned int) noexcept;
[[nodiscard]] Gadget simd_aes_gadget(unsigned int, bool) noexcept;
[[nodiscard]] Gadget simd_sha_three_gadget(unsigned int, bool) noexcept;
[[nodiscard]] Gadget simd_sha_two_gadget(unsigned int, bool) noexcept;
[[nodiscard]] Gadget simd_sha512_three_gadget(std::uint32_t, bool) noexcept;
[[nodiscard]] Gadget simd_sm3tt_gadget(unsigned int, unsigned int, bool) noexcept;
[[nodiscard]] Gadget simd_xar_gadget(unsigned int, bool) noexcept;
[[nodiscard]] Gadget simd_pmull_gadget(bool, unsigned int, bool) noexcept;
[[nodiscard]] Gadget floating_select_gadget(unsigned int, unsigned int) noexcept;
[[nodiscard]] Gadget floating_compare_gadget(unsigned int, bool, bool) noexcept;
[[nodiscard]] Gadget floating_convert_precision_gadget(unsigned int, unsigned int) noexcept;
[[nodiscard]] Gadget floating_conditional_compare_gadget(unsigned int, unsigned int, bool) noexcept;

} // namespace uml::cpu::arm64::gadgets::selection
