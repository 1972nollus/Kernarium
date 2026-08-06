// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gadget_selection_dependencies.hpp"

namespace uml::cpu::arm64::gadgets::selection {

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

} // namespace uml::cpu::arm64::gadgets::selection
