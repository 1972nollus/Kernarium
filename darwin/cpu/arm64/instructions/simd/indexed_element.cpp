// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"
#include "complete_helpers.hpp"

#include "../../detail/floating_compare.hpp"
#include "../../detail/floating_indexed.hpp"
#include "../../detail/instruction_helpers.hpp"
#include "../../detail/integer_multiply.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

#pragma STDC FENV_ACCESS ON

namespace uml::cpu::arm64 {

using detail::continue_after;
using detail::decode_little_endian;
using detail::encode_little_endian;
using namespace simd_complete_detail;

RunResult Dispatcher::simd_across_lanes(CpuState &cpu, GuestMemory &,
					std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 12) & 0x1fU;
	if (opcode != 12 && opcode != 15)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const bool alternate = (instruction & 0x0080'0000U) != 0;
	const bool is_double = (instruction & 0x0040'0000U) != 0;
	if (is_double || (instruction & 0x4000'0000U) == 0)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU];
	float value = read_floating_lane<float>(source, 0);
	for (std::size_t lane = 1; lane < 4; ++lane) {
		const auto next = read_floating_lane<float>(source, lane);
		if (opcode == 12)
			value = alternate ? std::fmin(value, next) : std::fmax(value, next);
		else if (std::isnan(value) || std::isnan(next))
			value = std::numeric_limits<float>::quiet_NaN();
		else
			value = alternate ? std::min(value, next) : std::max(value, next);
	}
	auto &destination = cpu.vectors[instruction & 0x1fU];
	destination.bytes.fill(std::byte{});
	write_floating_lane(destination, 0, value);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_scalar_pairwise(CpuState &cpu, GuestMemory &,
					    std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned opcode = (instruction >> 12) & 0x1fU;
	if (opcode != 12 && opcode != 13 && opcode != 15)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const bool alternate = (instruction & 0x0080'0000U) != 0;
	const bool is_double = (instruction & 0x0040'0000U) != 0;
	const auto &source = cpu.vectors[(instruction >> 5) & 0x1fU];
	auto &destination = cpu.vectors[instruction & 0x1fU];
	destination.bytes.fill(std::byte{});
	if (is_double) {
		const auto first = read_floating_lane<double>(source, 0);
		const auto second = read_floating_lane<double>(source, 1);
		double value;
		if (opcode == 13)
			value = first + second;
		else if (opcode == 12)
			value = alternate ? std::fmin(first, second) : std::fmax(first, second);
		else
			value = alternate ? std::min(first, second) : std::max(first, second);
		write_floating_lane(destination, 0, value);
	} else {
		const auto first = read_floating_lane<float>(source, 0);
		const auto second = read_floating_lane<float>(source, 1);
		float value;
		if (opcode == 13)
			value = first + second;
		else if (opcode == 12)
			value = alternate ? std::fmin(first, second) : std::fmax(first, second);
		else
			value = alternate ? std::min(first, second) : std::max(first, second);
		write_floating_lane(destination, 0, value);
	}
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_by_indexed_element(CpuState &cpu, GuestMemory &,
					       std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const bool q = (instruction & 0x4000'0000U) != 0;
	const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
	const unsigned size = (instruction >> 22) & 0x3U;
	const unsigned operation = (instruction >> 12) & 0xfU;
	const bool integer_group = scalar ?
		(unsigned_group ? operation == 13U || operation == 15U :
		 operation == 3U || operation == 7U || operation == 11U ||
		 operation == 12U || operation == 13U) :
		(unsigned_group ? operation == 0U || operation == 2U ||
		 operation == 4U || operation == 6U || operation == 10U ||
		 operation == 13U || operation == 15U :
		 operation == 2U || operation == 3U || operation == 6U ||
		 operation == 7U || operation == 8U || operation == 10U ||
		 operation == 11U || operation == 12U || operation == 13U);
	const bool floating = operation == 9U || (!unsigned_group &&
		(operation == 1U || operation == 5U));
	const bool ordinary_dot = !scalar && operation == 14U;
	const bool mixed_dot = !scalar && !unsigned_group && operation == 15U &&
		(size == 0U || size == 2U);
	const bool dot = ordinary_dot || mixed_dot;
	const bool scalar_allocated_operation = unsigned_group ?
		operation == 9U || operation == 13U || operation == 15U :
		operation == 1U || operation == 3U || operation == 5U ||
		operation == 7U || operation == 9U || operation == 11U ||
		operation == 12U || operation == 13U;
	if (integer_group && size != 1U && size != 2U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (ordinary_dot && size != 2U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (scalar && (!scalar_allocated_operation ||
	    ((operation == 1U || operation == 5U || operation == 9U) &&
	     size == 1U)))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (!integer_group && !floating && !dot)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	if (floating && size == 1U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (floating && size == 3U && !scalar && !q)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (floating && size == 3U && (instruction & 0x0020'0000U) != 0)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (integer_group && unsigned_group &&
	    (operation == 13U || operation == 15U)) {
		if (!detail::execute_saturating_rounding_multiply_accumulate(
			    cpu, instruction))
			return {StopReason::illegal_instruction, address, instruction,
				0, false};
		cpu.pc += 4;
		return continue_after(address, instruction);
	}
	unsigned element_register = (instruction >> 16) & 0xfU;
	unsigned element_index;
	if (floating && size == 0U) {
		element_index = ((instruction >> 9) & 4U) |
			((instruction >> 20) & 1U) |
			((instruction >> 20) & 2U);
	} else if (size == 1U) {
		element_index = ((instruction >> 9) & 4U) |
			((instruction >> 20) & 1U) |
			((instruction >> 20) & 2U);
	} else {
		element_register |= ((instruction >> 20) & 1U) << 4;
		element_index = ((instruction >> 10) & 2U) |
			((instruction >> 21) & 1U);
		if (size == 3)
			element_index >>= 1;
	}
	const std::size_t narrow_size = floating && size == 0U ? 2U :
		std::size_t{1} << size;
	if (element_index * narrow_size + narrow_size > 16)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	if (dot) {
		const bool left_signed = ordinary_dot ? !unsigned_group : size == 0U;
		const bool right_signed = ordinary_dot ? !unsigned_group : size == 2U;
		const unsigned vector_bytes = q ? 16U : 8U;
		const auto left = cpu.vectors[(instruction >> 5) & 31U].bytes;
		const auto right = cpu.vectors[element_register].bytes;
		const auto accumulator = cpu.vectors[instruction & 31U].bytes;
		std::array<std::byte, 16> result{};
		for (unsigned group = 0; group < vector_bytes / 4U; ++group) {
			std::int64_t sum = 0;
			for (unsigned byte = 0; byte < 4U; ++byte) {
				const auto left_raw = std::to_integer<std::uint8_t>(
					left[group * 4U + byte]);
				const auto right_raw = std::to_integer<std::uint8_t>(
					right[element_index * 4U + byte]);
				const std::int64_t left_value = left_signed ?
					static_cast<std::int8_t>(left_raw) : left_raw;
				const std::int64_t right_value = right_signed ?
					static_cast<std::int8_t>(right_raw) : right_raw;
				sum += left_value * right_value;
			}
			const auto old = decode_little_endian(
				std::span{accumulator}.subspan(group * 4U, 4U));
			encode_little_endian(static_cast<std::uint32_t>(old + sum),
				std::span{result}.subspan(group * 4U, 4U));
		}
		cpu.vectors[instruction & 31U].bytes = result;
		cpu.pc += 4;
		return continue_after(address, instruction);
	}
	const auto element_raw = decode_little_endian(
		std::span{cpu.vectors[element_register].bytes}.subspan(
			element_index * narrow_size, narrow_size));
	const auto source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto old_destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	if (floating) {
		if (scalar && (cpu.fpcr & 4U) != 0)
			result = operation == 1U || operation == 5U ?
				old_destination : source;
		const std::size_t vector_size = scalar ? narrow_size : q ? 16 : 8;
		const std::size_t lanes = vector_size / narrow_size;
		auto execute = [&]<typename Floating>() {
			using Integer = std::conditional_t<sizeof(Floating) == 8,
				std::uint64_t, std::conditional_t<sizeof(Floating) == 4,
				std::uint32_t, std::uint16_t>>;
			const auto element = std::bit_cast<Floating>(
				static_cast<Integer>(element_raw));
			for (std::size_t lane = 0; lane < lanes; ++lane) {
				const auto left = std::bit_cast<Floating>(
					static_cast<Integer>(decode_little_endian(
						std::span{source}.subspan(
							lane * sizeof(Floating), sizeof(Floating)))));
				const auto accumulator = std::bit_cast<Floating>(
					static_cast<Integer>(decode_little_endian(
						std::span{old_destination}.subspan(
							lane * sizeof(Floating), sizeof(Floating)))));
				Floating value;
				if ((cpu.fpcr & 2U) != 0U ||
				    (sizeof(Floating) != 2U && (cpu.fpcr & 1U) != 0U)) {
					const auto indexed_operation = operation == 1U ?
						detail::IndexedFloatingOperation::add :
						operation == 5U ?
						detail::IndexedFloatingOperation::subtract :
						unsigned_group ?
						detail::IndexedFloatingOperation::
							multiply_extended :
						detail::IndexedFloatingOperation::multiply;
					const auto software =
						detail::indexed_floating_software(
							accumulator, left, element,
							indexed_operation, cpu.fpcr);
					value = std::bit_cast<Floating>(software.bits);
					cpu.fpsr |= software.exceptions;
				} else {
					value = operation == 1U || operation == 5U ?
						fused_multiply_add(left, element, accumulator,
							operation == 5U) : unsigned_group ?
						multiply_extended(left, element) : left * element;
				}
				encode_little_endian(std::bit_cast<Integer>(value),
					std::span{result}.subspan(
						lane * sizeof(Floating), sizeof(Floating)));
			}
		};
		if (size == 0U)
			execute.template operator()<_Float16>();
		else if (size == 2U)
			execute.template operator()<float>();
		else
			execute.template operator()<double>();
	} else {
		const bool widening = operation == 2 || operation == 3 ||
			operation == 6 || operation == 7 || operation == 10 || operation == 11;
		const bool doubling = operation == 3 || operation == 7 ||
			operation == 11 || operation == 12 || operation == 13 ||
			operation == 15;
		const bool rounding_accumulate = unsigned_group &&
			(operation == 13U || operation == 15U);
		if (widening) {
			const std::size_t wide_size = narrow_size * 2;
			const unsigned narrow_bits = static_cast<unsigned>(narrow_size * 8);
			const unsigned wide_bits = narrow_bits * 2;
			const std::size_t source_base = !scalar && q ? 8 : 0;
			const std::size_t lanes = scalar ? 1 : 16 / wide_size;
			for (std::size_t lane = 0; lane < lanes; ++lane) {
				const auto raw = decode_little_endian(std::span{source}.subspan(
					source_base + lane * narrow_size, narrow_size));
				const SignedWide first = unsigned_group && !doubling ? raw :
					as_signed(raw, narrow_bits);
				const SignedWide second = unsigned_group && !doubling ? element_raw :
					as_signed(element_raw, narrow_bits);
				SignedWide product = first * second * (doubling ? 2 : 1);
				std::uint64_t value;
				if (doubling) {
					value = saturate_signed(cpu, product, wide_bits);
					if (operation == 3U || operation == 7U) {
						const auto accumulator = decode_little_endian(
							std::span{old_destination}.subspan(
								lane * wide_size, wide_size));
						const SignedWide combined = as_signed(accumulator,
							wide_bits) + (operation == 3U ?
							as_signed(value, wide_bits) :
							-as_signed(value, wide_bits));
						value = saturate_signed(cpu, combined, wide_bits);
					}
				} else {
					const auto accumulator = decode_little_endian(
						std::span{old_destination}.subspan(lane * wide_size,
							wide_size));
					value = operation == 2 ? accumulator +
						static_cast<std::uint64_t>(product) : operation == 6 ?
						accumulator - static_cast<std::uint64_t>(product) :
						static_cast<std::uint64_t>(product);
				}
				encode_little_endian(value & mask_for(wide_bits),
					std::span{result}.subspan(lane * wide_size, wide_size));
			}
		} else {
			const unsigned bits = static_cast<unsigned>(narrow_size * 8);
			const std::size_t vector_size = scalar ? narrow_size : q ? 16 : 8;
			for (std::size_t offset = 0; offset < vector_size; offset += narrow_size) {
				const auto first = decode_little_endian(
					std::span{source}.subspan(offset, narrow_size));
				const auto old = decode_little_endian(
					std::span{old_destination}.subspan(offset, narrow_size));
				const SignedWide doubled_product = as_signed(first, bits) *
					as_signed(element_raw, bits) * 2;
				std::uint64_t value;
				if (operation == 0)
					value = old + first * element_raw;
				else if (operation == 4)
					value = old - first * element_raw;
				else if (operation == 8)
					value = first * element_raw;
				else if (rounding_accumulate) {
					const SignedWide accumulator = as_signed(old, bits);
					SignedWide combined = accumulator *
						(SignedWide{1} << bits);
					combined += operation == 13U ?
						doubled_product : -doubled_product;
					combined += SignedWide{1} << (bits - 1U);
					value = saturate_signed(cpu, combined >> bits, bits);
				} else {
					const auto product = doubled_product +
						(operation == 13 ? SignedWide{1} << (bits - 1) : 0);
					value = saturate_signed(cpu, product >> bits, bits);
				}
				encode_little_endian(value & mask_for(bits),
					std::span{result}.subspan(offset, narrow_size));
			}
		}
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::cpu::arm64
