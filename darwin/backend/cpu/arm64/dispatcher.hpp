// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu_state.hpp"
#include "memory.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::backend::arm64 {

enum class StopReason {
	continued,
	budget_exhausted,
	system_call,
	breakpoint,
	memory_fault,
	illegal_instruction,
	unsupported_instruction,
};

struct RunResult final {
	StopReason reason{StopReason::budget_exhausted};
	std::uint64_t address{};
	std::uint32_t instruction{};
	std::size_t instructions{};
	bool write{};
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

	[[nodiscard]] RunResult exception(CpuState &, GuestMemory &,
					 std::uint32_t) const;
	[[nodiscard]] RunResult branch_immediate(CpuState &, GuestMemory &,
					      std::uint32_t) const;
	[[nodiscard]] RunResult branch_conditional(CpuState &, GuestMemory &,
						std::uint32_t) const;
	[[nodiscard]] RunResult compare_branch(CpuState &, GuestMemory &,
					    std::uint32_t) const;
	[[nodiscard]] RunResult test_branch(CpuState &, GuestMemory &,
					 std::uint32_t) const;
	[[nodiscard]] RunResult branch_register(CpuState &, GuestMemory &,
					     std::uint32_t) const;
	[[nodiscard]] RunResult pc_relative(CpuState &, GuestMemory &,
					 std::uint32_t) const;
	[[nodiscard]] RunResult move_wide(CpuState &, GuestMemory &,
				       std::uint32_t) const;
	[[nodiscard]] RunResult add_sub_immediate(CpuState &, GuestMemory &,
					       std::uint32_t) const;
	[[nodiscard]] RunResult add_sub_register(CpuState &, GuestMemory &,
						std::uint32_t) const;
	[[nodiscard]] RunResult add_sub_carry(CpuState &, GuestMemory &,
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
	[[nodiscard]] RunResult load_store_other(CpuState &, GuestMemory &,
					      std::uint32_t) const;
	[[nodiscard]] RunResult load_store_pair(CpuState &, GuestMemory &,
					     std::uint32_t) const;
	[[nodiscard]] RunResult load_store_simd_structure(CpuState &,
						      GuestMemory &,
						      std::uint32_t) const;
	[[nodiscard]] RunResult load_store_simd_single_structure(
		CpuState &, GuestMemory &, std::uint32_t) const;
	[[nodiscard]] RunResult load_store_exclusive(CpuState &, GuestMemory &,
							 std::uint32_t) const;
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
	[[nodiscard]] RunResult floating_compare(CpuState &, GuestMemory &,
						    std::uint32_t) const;
	[[nodiscard]] RunResult floating_move(CpuState &, GuestMemory &,
						 std::uint32_t) const;
	[[nodiscard]] RunResult floating_integer_convert(CpuState &, GuestMemory &,
							    std::uint32_t) const;
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
	[[nodiscard]] RunResult simd_floating_arithmetic(CpuState &, GuestMemory &,
							    std::uint32_t) const;
	[[nodiscard]] RunResult simd_table_lookup(CpuState &, GuestMemory &,
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
	[[nodiscard]] RunResult cache_maintenance(CpuState &, GuestMemory &,
					     std::uint32_t) const;
	[[nodiscard]] RunResult memory_barrier(CpuState &, GuestMemory &,
					   std::uint32_t) const;
	[[nodiscard]] RunResult nop(CpuState &, GuestMemory &,
				std::uint32_t) const;
};

} // namespace uml::backend::arm64
