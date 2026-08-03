// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/bfloat_dot.hpp"
#include "../../detail/bfloat_matrix.hpp"
#include "../../detail/bfloat_multiply.hpp"
#include "../../detail/half_fused_long.hpp"
#include "../../detail/half_dot.hpp"
#include "../../detail/half_matrix.hpp"
#include "../../detail/floating_arithmetic.hpp"
#include "../../detail/bfloat_conversion.hpp"
#include "../../detail/floating_compare.hpp"
#include "../../detail/floating_complex.hpp"
#include "../../detail/floating_conversion.hpp"
#include "../../detail/floating_fused.hpp"
#include "../../detail/floating_horizontal.hpp"
#include "../../detail/floating_reciprocal.hpp"
#include "../../detail/floating_reciprocal_software.hpp"
#include "../../detail/floating_round.hpp"
#include "../../detail/floating_scale.hpp"
#include "../../detail/instruction_helpers.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace uml::cpu::arm64 {

using detail::continue_after;
using detail::decode_little_endian;
using detail::encode_little_endian;

RunResult Dispatcher::bfloat_conversion(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_bfloat_conversion(instruction);
	if (!decoded.recognized)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = instruction & 31U;
	const auto source = cpu.vectors[(instruction >> 5) & 31U].bytes;
	auto result = decoded.scalar && (cpu.fpcr & 4U) != 0U ?
		cpu.vectors[destination].bytes :
		decoded.upper ? cpu.vectors[destination].bytes :
			std::array<std::byte, 16>{};
	const unsigned lanes = decoded.scalar ? 1U : 4U;
	const unsigned first_lane = decoded.upper ? 4U : 0U;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto input = static_cast<std::uint32_t>(decode_little_endian(
			std::span{source}.subspan(lane * 4U, 4U)));
		const auto converted = detail::convert_to_bfloat(input, cpu.fpcr);
		encode_little_endian(converted.bits, std::span{result}.subspan(
			(first_lane + lane) * 2U, 2U));
		cpu.fpsr |= converted.exceptions;
	}
	cpu.vectors[destination].bytes = result;
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::bfloat_multiply_long(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_bfloat_multiply_long(instruction);
	if (!decoded.recognized)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = instruction & 31U;
	const unsigned first_register = (instruction >> 5) & 31U;
	const unsigned second_register = (instruction >> 16) &
		(decoded.by_element ? 15U : 31U);
	const auto addends = cpu.vectors[destination].bytes;
	const auto first = cpu.vectors[first_register].bytes;
	const auto second = cpu.vectors[second_register].bytes;
	auto result = addends;
	for (unsigned lane = 0; lane < 4U; ++lane) {
		const unsigned first_lane = lane * 2U + decoded.top;
		const unsigned second_lane = decoded.by_element ? decoded.index :
			lane * 2U + decoded.top;
		const auto addend = static_cast<std::uint32_t>(decode_little_endian(
			std::span{addends}.subspan(lane * 4U, 4U)));
		const auto left = static_cast<std::uint16_t>(decode_little_endian(
			std::span{first}.subspan(first_lane * 2U, 2U)));
		const auto right = static_cast<std::uint16_t>(decode_little_endian(
			std::span{second}.subspan(second_lane * 2U, 2U)));
		encode_little_endian(detail::bfloat_multiply_add(
			addend, left, right, cpu.fpcr),
			std::span{result}.subspan(lane * 4U, 4U));
	}
	cpu.vectors[destination].bytes = result;
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::bfloat_dot(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_bfloat_dot(instruction);
	if (!decoded.recognized)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = instruction & 31U;
	const auto first = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto second = cpu.vectors[(instruction >> 16) & 31U].bytes;
	const auto addends = cpu.vectors[destination].bytes;
	std::array<std::byte, 16> result{};
	const unsigned lanes = decoded.wide ? 4U : 2U;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const unsigned first_pair = lane * 2U;
		const unsigned second_pair = decoded.by_element ?
			decoded.index * 2U : first_pair;
		const auto read_bfloat = [](const auto &source, unsigned element) {
			return static_cast<std::uint16_t>(decode_little_endian(
				std::span{source}.subspan(element * 2U, 2U)));
		};
		const auto addend = static_cast<std::uint32_t>(decode_little_endian(
			std::span{addends}.subspan(lane * 4U, 4U)));
		encode_little_endian(detail::bfloat_dot_add(addend,
			read_bfloat(first, first_pair),
			read_bfloat(first, first_pair + 1U),
			read_bfloat(second, second_pair),
			read_bfloat(second, second_pair + 1U), cpu.fpcr),
			std::span{result}.subspan(lane * 4U, 4U));
	}
	cpu.vectors[destination].bytes = result;
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::bfloat_matrix_multiply(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	if (!detail::decode_bfloat_matrix_multiply(instruction))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = instruction & 31U;
	const auto addends = cpu.vectors[destination].bytes;
	const auto left = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 31U].bytes;
	cpu.vectors[destination].bytes = detail::bfloat_matrix_multiply_add(
		addends, left, right, cpu.fpcr);
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::half_fused_long(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_half_fused_long(instruction);
	if (!decoded.recognized)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = instruction & 31U;
	const auto addends = cpu.vectors[destination].bytes;
	const auto first = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto second = cpu.vectors[(instruction >> 16) &
		(decoded.by_element ? 15U : 31U)].bytes;
	std::array<std::byte, 16> result{};
	const unsigned lanes = decoded.wide ? 4U : 2U;
	const unsigned first_lane = decoded.upper ? lanes : 0U;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const unsigned second_lane = decoded.by_element ? decoded.index :
			first_lane + lane;
		const auto addend = static_cast<std::uint32_t>(decode_little_endian(
			std::span{addends}.subspan(lane * 4U, 4U)));
		const auto left = static_cast<std::uint16_t>(decode_little_endian(
			std::span{first}.subspan((first_lane + lane) * 2U, 2U)));
		const auto right = static_cast<std::uint16_t>(decode_little_endian(
			std::span{second}.subspan(second_lane * 2U, 2U)));
		const auto fused = detail::half_fused_multiply_add(addend,
			left, right, decoded.subtract, cpu.fpcr);
		encode_little_endian(fused.bits,
			std::span{result}.subspan(lane * 4U, 4U));
		cpu.fpsr |= fused.exceptions;
	}
	cpu.vectors[destination].bytes = result;
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::half_matrix(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_half_matrix(instruction);
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = instruction & 31U;
	const auto addends = cpu.vectors[destination].bytes;
	const auto first = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto second = cpu.vectors[(instruction >> 16) & 31U].bytes;
	const auto result = detail::half_matrix_multiply_add(addends, first,
		second, decoded.widening, cpu.fpcr);
	cpu.vectors[destination].bytes = result.bytes;
	cpu.fpsr |= result.exceptions;
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::half_dot(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_half_dot(instruction);
	if (!decoded.recognized)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = instruction & 31U;
	const auto addends = cpu.vectors[destination].bytes;
	const auto first = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto second = cpu.vectors[(instruction >> 16) & 31U].bytes;
	const auto result = detail::half_dot_add(addends, first, second,
		decoded, cpu.fpcr);
	cpu.vectors[destination].bytes = result.bytes;
	cpu.fpsr |= result.exceptions;
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_reciprocal(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_floating_reciprocal(instruction);
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	detail::execute_floating_reciprocal_software(cpu, decoded,
		instruction & 31U, (instruction >> 5) & 31U,
		(instruction >> 16) & 31U);
	cpu.pc += 4U;
	return continue_after(address, instruction);
}


} // namespace uml::cpu::arm64
