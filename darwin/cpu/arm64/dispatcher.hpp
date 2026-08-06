// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu_state.hpp"
#include "memory.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64 {

enum class StopReason {
	continued,
	budget_exhausted,
	system_call,
	breakpoint,
	memory_fault,
	gcs_fault,
	illegal_instruction,
	unavailable_feature,
	unsupported_instruction,
	floating_point_trap,
};

struct RunResult final {
	StopReason reason{StopReason::budget_exhausted};
	std::uint64_t address{};
	std::uint32_t instruction{};
	std::size_t instructions{};
	bool write{};
	/* Fetch faults are neither data reads nor writes. Keep the historical
	 * write member for the many instruction-family aggregate initializers,
	 * and use this field to preserve the otherwise unrepresentable execute
	 * direction at the dispatcher boundary. */
	::uml::memory::MemoryAccess access{
		::uml::memory::MemoryAccess::read};
	/* ESR-shaped metadata for StopReason::floating_point_trap. */
	std::uint32_t syndrome{};
};

class Dispatcher final {
public:
	[[nodiscard]] RunResult run(CpuState &cpu, GuestMemory &memory,
				    std::size_t budget) const;
	[[nodiscard]] RunResult step(CpuState &cpu, GuestMemory &memory) const;
	[[nodiscard]] RunResult execute(CpuState &cpu, GuestMemory &memory,
					std::uint32_t instruction) const;

private:
	using Handler = RunResult (Dispatcher::*)(CpuState &, GuestMemory &,
						 std::uint32_t) const;
	struct DecodeEntry final {
		std::uint32_t mask;
		std::uint32_t value;
		Handler handler;
	};

	[[nodiscard]] RunResult execute_decoded(CpuState &, GuestMemory &,
						std::uint32_t) const;
	[[nodiscard]] RunResult exception(CpuState &, GuestMemory &,
					 std::uint32_t) const;
	[[nodiscard]] RunResult branch_immediate(CpuState &, GuestMemory &,
					      std::uint32_t) const;
	[[nodiscard]] RunResult branch_conditional(CpuState &, GuestMemory &,
						std::uint32_t) const;
	[[nodiscard]] RunResult compare_branch(CpuState &, GuestMemory &,
						std::uint32_t) const;
	[[nodiscard]] RunResult compare_conditional_branch(CpuState &,
		GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult test_branch(CpuState &, GuestMemory &,
					 std::uint32_t) const;
	[[nodiscard]] RunResult branch_register(CpuState &, GuestMemory &,
					     std::uint32_t) const;
	[[nodiscard]] RunResult branch_authenticated(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult pc_relative(CpuState &, GuestMemory &,
					 std::uint32_t) const;
	[[nodiscard]] RunResult move_wide(CpuState &, GuestMemory &,
				       std::uint32_t) const;
	[[nodiscard]] RunResult add_sub_immediate(CpuState &, GuestMemory &,
					       std::uint32_t) const;
	[[nodiscard]] RunResult mte_add_sub_tag(CpuState &, GuestMemory &,
					    std::uint32_t) const;
	[[nodiscard]] RunResult add_sub_register(CpuState &, GuestMemory &,
						std::uint32_t) const;
	[[nodiscard]] RunResult add_sub_carry(CpuState &, GuestMemory &,
					     std::uint32_t) const;
	[[nodiscard]] RunResult mte_pointer(CpuState &, GuestMemory &,
				       std::uint32_t) const;
	[[nodiscard]] RunResult cpa_pointer(CpuState &, GuestMemory &,
					    std::uint32_t) const;
	[[nodiscard]] RunResult cpa_multiply(CpuState &, GuestMemory &,
					     std::uint32_t) const;
	[[nodiscard]] RunResult logical_shifted(CpuState &, GuestMemory &,
					     std::uint32_t) const;
	[[nodiscard]] RunResult logical_immediate(CpuState &, GuestMemory &,
					       std::uint32_t) const;
	[[nodiscard]] RunResult bitfield(CpuState &, GuestMemory &,
					       std::uint32_t) const;
	[[nodiscard]] RunResult extract_register(CpuState &, GuestMemory &,
						std::uint32_t) const;
	[[nodiscard]] RunResult load_store_unsigned(CpuState &, GuestMemory &,
						    std::uint32_t) const;
	[[nodiscard]] RunResult load_literal(CpuState &, GuestMemory &,
					    std::uint32_t) const;
	[[nodiscard]] RunResult load_store_other(CpuState &, GuestMemory &,
					      std::uint32_t) const;
	[[nodiscard]] RunResult load_pointer_authenticated(CpuState &,
		GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult load_store_pair(CpuState &, GuestMemory &,
					     std::uint32_t) const;
	[[nodiscard]] RunResult load_store_simd_structure(CpuState &,
						      GuestMemory &,
						      std::uint32_t) const;
	[[nodiscard]] RunResult load_store_simd_single_structure(
		CpuState &, GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult load_store_simd_ordered(CpuState &,
		GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult load_store_exclusive(CpuState &, GuestMemory &,
						      std::uint32_t) const;
	[[nodiscard]] RunResult load_store_64b(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult load_store_allocation_tag(CpuState &,
		GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult memory_copy_set(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult load_store_compare_swap(CpuState &, GuestMemory &,
						std::uint32_t) const;
	[[nodiscard]] RunResult load_store_compare_swap_pair(CpuState &,
		GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult load_store_atomic(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult load_store_atomic_floating(CpuState &,
		GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult load_store_atomic_pair_128(CpuState &,
		GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult load_store_rcw(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult load_store_rcw_pair(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult load_store_rcpc(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult load_store_ordered_pair(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult load_store_ordered_writeback(CpuState &,
		GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult conditional_select(CpuState &, GuestMemory &,
						std::uint32_t) const;
	[[nodiscard]] RunResult conditional_compare(CpuState &, GuestMemory &,
						 std::uint32_t) const;
	[[nodiscard]] RunResult multiply_add(CpuState &, GuestMemory &,
					  std::uint32_t) const;
	[[nodiscard]] RunResult variable_shift(CpuState &, GuestMemory &,
					   std::uint32_t) const;
	[[nodiscard]] RunResult divide(CpuState &, GuestMemory &,
				       std::uint32_t) const;
	[[nodiscard]] RunResult bit_reverse_count(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult pointer_auth_strip(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult pointer_auth_code(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult pointer_auth_generic(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult guarded_control_stack(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult unavailable_extension(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult integer_min_max(CpuState &, GuestMemory &,
					       std::uint32_t) const;
	[[nodiscard]] RunResult flag_manipulation(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult pstate_immediate(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult crc32(CpuState &, GuestMemory &,
					 std::uint32_t) const;
	[[nodiscard]] RunResult floating_compare(CpuState &, GuestMemory &,
						    std::uint32_t) const;
	[[nodiscard]] RunResult floating_move(CpuState &, GuestMemory &,
						 std::uint32_t) const;
	[[nodiscard]] RunResult floating_integer_convert(CpuState &, GuestMemory &,
							    std::uint32_t) const;
	[[nodiscard]] RunResult floating_to_integer_checked(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult floating_javascript_convert(CpuState &,
		GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult integer_to_floating_checked(CpuState &,
		GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult floating_arithmetic(CpuState &, GuestMemory &,
						       std::uint32_t) const;
	[[nodiscard]] RunResult floating_conditional_compare(CpuState &,
							GuestMemory &,
							std::uint32_t) const;
	[[nodiscard]] RunResult floating_conditional_select(CpuState &,
						       GuestMemory &,
						       std::uint32_t) const;
	[[nodiscard]] RunResult floating_multiply_add(CpuState &, GuestMemory &,
						 std::uint32_t) const;
	[[nodiscard]] RunResult floating_basic_arithmetic(CpuState &, GuestMemory &,
						    std::uint32_t) const;
	[[nodiscard]] RunResult floating_fused_arithmetic(CpuState &, GuestMemory &,
						    std::uint32_t) const;
	[[nodiscard]] RunResult floating_round(CpuState &, GuestMemory &,
						 std::uint32_t) const;
	[[nodiscard]] RunResult floating_horizontal(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult floating_scale(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult bfloat_conversion(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult bfloat_dot(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult bfloat_matrix_multiply(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult bfloat_multiply_long(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult half_fused_long(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult half_dot(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult half_matrix(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult fp8(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult floating_reciprocal(CpuState &, GuestMemory &,
						      std::uint32_t) const;
	[[nodiscard]] RunResult floating_complex(CpuState &, GuestMemory &,
						   std::uint32_t) const;
	[[nodiscard]] RunResult simd_modified_immediate(CpuState &,
						     GuestMemory &,
						     std::uint32_t) const;
	[[nodiscard]] RunResult simd_compare_zero(CpuState &, GuestMemory &,
						      std::uint32_t) const;
	[[nodiscard]] RunResult simd_compare_register(CpuState &, GuestMemory &,
							  std::uint32_t) const;
	[[nodiscard]] RunResult simd_bitwise_register(CpuState &, GuestMemory &,
							  std::uint32_t) const;
	[[nodiscard]] RunResult simd_min_max(CpuState &, GuestMemory &,
						  std::uint32_t) const;
	[[nodiscard]] RunResult simd_bit_unary(CpuState &, GuestMemory &,
						   std::uint32_t) const;
	[[nodiscard]] RunResult simd_reduce_integer(CpuState &, GuestMemory &,
							std::uint32_t) const;
	[[nodiscard]] RunResult simd_extract(CpuState &, GuestMemory &,
						 std::uint32_t) const;
	[[nodiscard]] RunResult simd_permute(CpuState &, GuestMemory &,
						 std::uint32_t) const;
	[[nodiscard]] RunResult simd_arithmetic_register(CpuState &,
							     GuestMemory &,
							     std::uint32_t) const;
	[[nodiscard]] RunResult simd_three_same(CpuState &, GuestMemory &,
						   std::uint32_t) const;
	[[nodiscard]] RunResult simd_saturating_rounding_multiply_accumulate(
		CpuState &, GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult simd_dot_product(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult simd_matrix_multiply(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult simd_floating_multiply_extended(CpuState &,
		GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult simd_two_register_misc(CpuState &, GuestMemory &,
							 std::uint32_t) const;
	[[nodiscard]] RunResult simd_across_lanes(CpuState &, GuestMemory &,
						     std::uint32_t) const;
	[[nodiscard]] RunResult simd_scalar_pairwise(CpuState &, GuestMemory &,
						       std::uint32_t) const;
	[[nodiscard]] RunResult simd_by_indexed_element(CpuState &, GuestMemory &,
							  std::uint32_t) const;
	[[nodiscard]] RunResult simd_crypto(CpuState &, GuestMemory &,
					       std::uint32_t) const;
	[[nodiscard]] RunResult simd_arithmetic_narrow(CpuState &, GuestMemory &,
							   std::uint32_t) const;
	[[nodiscard]] RunResult simd_arithmetic_wide(CpuState &, GuestMemory &,
							 std::uint32_t) const;
	[[nodiscard]] RunResult simd_narrow_move(CpuState &, GuestMemory &,
						    std::uint32_t) const;
	[[nodiscard]] RunResult simd_integer_float_convert(CpuState &,
							      GuestMemory &,
							      std::uint32_t) const;
	[[nodiscard]] RunResult simd_floating_unary(CpuState &, GuestMemory &,
						       std::uint32_t) const;
	[[nodiscard]] RunResult simd_floating_compare(CpuState &, GuestMemory &,
							 std::uint32_t) const;
	[[nodiscard]] RunResult simd_floating_precision_vector(CpuState &,
		GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult simd_floating_arithmetic(CpuState &, GuestMemory &,
							    std::uint32_t) const;
	[[nodiscard]] RunResult simd_table_lookup(CpuState &, GuestMemory &,
						      std::uint32_t) const;
	[[nodiscard]] RunResult simd_lut_lookup(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult simd_shift_right_narrow(CpuState &,
							GuestMemory &,
							std::uint32_t) const;
	[[nodiscard]] RunResult simd_shift_immediate(CpuState &, GuestMemory &,
							 std::uint32_t) const;
	[[nodiscard]] RunResult simd_shift_register(CpuState &, GuestMemory &,
							std::uint32_t) const;
	[[nodiscard]] RunResult simd_shift_left_long(CpuState &, GuestMemory &,
							 std::uint32_t) const;
	[[nodiscard]] RunResult simd_general_move(CpuState &, GuestMemory &,
						      std::uint32_t) const;
	[[nodiscard]] RunResult simd_insert_general(CpuState &, GuestMemory &,
						       std::uint32_t) const;
	[[nodiscard]] RunResult simd_insert_element(CpuState &, GuestMemory &,
						       std::uint32_t) const;
	[[nodiscard]] RunResult simd_duplicate_general(CpuState &, GuestMemory &,
							   std::uint32_t) const;
	[[nodiscard]] RunResult simd_duplicate_element(CpuState &, GuestMemory &,
							   std::uint32_t) const;
	[[nodiscard]] RunResult system_register(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult system_register_pair(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult system_instruction_pair(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult system_instruction(CpuState &, GuestMemory &,
		std::uint32_t) const;
	[[nodiscard]] RunResult cache_maintenance(CpuState &, GuestMemory &,
					     std::uint32_t) const;
	[[nodiscard]] RunResult memory_barrier(CpuState &, GuestMemory &,
					   std::uint32_t) const;
	[[nodiscard]] RunResult clear_exclusive(CpuState &, GuestMemory &,
					   std::uint32_t) const;
	[[nodiscard]] RunResult nop(CpuState &, GuestMemory &,
				std::uint32_t) const;
};

} // namespace uml::cpu::arm64
