// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/instruction_helpers.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace uml::backend::arm64 {

using detail::continue_after;
using detail::decode_little_endian;
using detail::encode_little_endian;

namespace {

template<typename Floating>
[[nodiscard]] Floating decode_lane(const std::array<std::byte, 16> &bytes,
				   std::size_t lane)
{
	using Integer = std::conditional_t<sizeof(Floating) == 8,
		std::uint64_t, std::uint32_t>;
	return std::bit_cast<Floating>(static_cast<Integer>(decode_little_endian(
		std::span{bytes}.subspan(lane * sizeof(Floating), sizeof(Floating)))));
}

template<typename Floating>
void encode_lane(std::array<std::byte, 16> &bytes, std::size_t lane,
		 Floating value)
{
	using Integer = std::conditional_t<sizeof(Floating) == 8,
		std::uint64_t, std::uint32_t>;
	encode_little_endian(std::bit_cast<Integer>(value),
		std::span{bytes}.subspan(lane * sizeof(Floating), sizeof(Floating)));
}

template<typename Floating>
[[nodiscard]] bool execute_floating_vector(CpuState &cpu,
					   std::uint32_t instruction,
					   std::size_t lanes)
{
	const unsigned opcode = (instruction >> 11) & 0x1fU;
	const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
	const bool alternate = (instruction & 0x0080'0000U) != 0;
	if (opcode < 0x18)
		return false;
	const auto left = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	const auto right = cpu.vectors[(instruction >> 16) & 0x1fU].bytes;
	const auto old_destination = cpu.vectors[instruction & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t lane = 0; lane < lanes; ++lane) {
		Floating first;
		Floating second;
		if (unsigned_group &&
		    ((opcode == 0x1a && !alternate) || opcode == 0x1e)) {
			const auto &source = lane < lanes / 2 ? left : right;
			const std::size_t pair = (lane % (lanes / 2)) * 2;
			first = decode_lane<Floating>(source, pair);
			second = decode_lane<Floating>(source, pair + 1);
		} else {
			first = decode_lane<Floating>(left, lane);
			second = decode_lane<Floating>(right, lane);
		}
		Floating value;
		if (opcode == 0x18 && !unsigned_group) {
			value = alternate ? std::fmin(first, second) :
				std::fmax(first, second);
		} else if (opcode == 0x19 && !unsigned_group) {
			const auto accumulator = decode_lane<Floating>(
				old_destination, lane);
			value = alternate ? accumulator - first * second :
				accumulator + first * second;
		} else if (opcode == 0x1a && !unsigned_group) {
			value = alternate ? first - second : first + second;
		} else if (opcode == 0x1a && unsigned_group) {
			value = alternate ? std::fabs(first - second) : first + second;
		} else if (opcode == 0x1b && !alternate) {
			value = first * second;
		} else if (opcode == 0x1c) {
			const bool comparison = !unsigned_group ? first == second :
				alternate ? first > second : first >= second;
			using Integer = std::conditional_t<sizeof(Floating) == 8,
				std::uint64_t, std::uint32_t>;
			encode_little_endian(comparison ? ~Integer{0} : Integer{0},
				std::span{result}.subspan(lane * sizeof(Floating),
					sizeof(Floating)));
			continue;
		} else if (opcode == 0x1d && unsigned_group) {
			const bool comparison = alternate ?
				std::fabs(first) > std::fabs(second) :
				std::fabs(first) >= std::fabs(second);
			using Integer = std::conditional_t<sizeof(Floating) == 8,
				std::uint64_t, std::uint32_t>;
			encode_little_endian(comparison ? ~Integer{0} : Integer{0},
				std::span{result}.subspan(lane * sizeof(Floating),
					sizeof(Floating)));
			continue;
		} else if (opcode == 0x1e) {
			value = alternate ? std::fmin(first, second) :
				std::fmax(first, second);
		} else if (opcode == 0x1f && unsigned_group && !alternate) {
			value = first / second;
		} else if (opcode == 0x1f && !unsigned_group) {
			value = alternate ?
				(static_cast<Floating>(3) - first * second) /
					static_cast<Floating>(2) :
				static_cast<Floating>(2) - first * second;
		} else {
			return false;
		}
		encode_lane(result, lane, value);
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	return true;
}

template<typename Floating>
[[nodiscard]] bool execute_floating_unary(CpuState &cpu,
					  std::uint32_t instruction,
					  std::size_t lanes)
{
	using Integer = std::conditional_t<sizeof(Floating) == 8,
		std::uint64_t, std::uint32_t>;
	constexpr Integer sign_bit = Integer{1} << (sizeof(Floating) * 8 - 1);
	const auto operation = instruction & 0xbf3f'fc00U;
	const auto source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t lane = 0; lane < lanes; ++lane) {
		const auto raw = static_cast<Integer>(decode_little_endian(
			std::span{source}.subspan(lane * sizeof(Floating),
				sizeof(Floating))));
		Integer value;
		if (operation == 0x0e20'f800U) {
			value = raw & ~sign_bit;
		} else if (operation == 0x2e20'f800U) {
			value = raw ^ sign_bit;
		} else if (operation == 0x2e21'f800U) {
			value = std::bit_cast<Integer>(
				std::sqrt(std::bit_cast<Floating>(raw)));
		} else {
			return false;
		}
		encode_little_endian(value,
			std::span{result}.subspan(lane * sizeof(Floating),
				sizeof(Floating)));
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	return true;
}

} // namespace

RunResult Dispatcher::simd_floating_unary(CpuState &cpu, GuestMemory &,
					   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_double = (instruction & 0x0040'0000U) != 0;
	const std::size_t vector_size =
		(instruction & 0x4000'0000U) != 0 ? 16 : 8;
	if ((is_double && vector_size != 16) ||
	    !(is_double ? execute_floating_unary<double>(cpu, instruction, 2) :
		      execute_floating_unary<float>(cpu, instruction,
			      vector_size / 4)))
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_floating_arithmetic(CpuState &cpu, GuestMemory &,
						std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_double = (instruction & 0x0040'0000U) != 0;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const std::size_t vector_size =
		(instruction & 0x4000'0000U) != 0 ? 16 : 8;
	if ((is_double && !scalar && vector_size != 16) ||
	    !(is_double ? execute_floating_vector<double>(cpu, instruction,
			scalar ? 1 : 2) : execute_floating_vector<float>(cpu,
			instruction, scalar ? 1 : vector_size / 4)))
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::backend::arm64
