// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/floating_integer_conversion.hpp"
#include "../../detail/instruction_helpers.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace uml::backend::arm64 {

using detail::continue_after;
using detail::decode_little_endian;
using detail::encode_little_endian;

RunResult Dispatcher::floating_javascript_convert(CpuState &cpu,
	GuestMemory &, std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	if ((instruction & 0xffff'fc00U) != 0x1e7e'0000U)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const auto source =
		cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto input = decode_little_endian(std::span{source}.first(8U));
	const auto converted =
		detail::convert_floating_to_javascript_integer(input, cpu.fpcr);
	cpu.write_register(instruction & 31U, converted.value, false);
	cpu.nzcv = converted.sets_z ? 0x4000'0000U : 0U;
	cpu.fpsr |= converted.exceptions;
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::integer_to_floating_checked(CpuState &cpu,
	GuestMemory &, std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_integer_to_floating(instruction);
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = instruction & 31U;
	const unsigned source_register = (instruction >> 5) & 31U;
	const auto source = decoded.source_gpr ? std::array<std::byte, 16>{} :
		cpu.vectors[source_register].bytes;
	const std::uint64_t gpr_source = decoded.source_gpr ?
		cpu.read_register(source_register, false) : 0U;
	auto result = decoded.scalar && (cpu.fpcr & 4U) != 0U ?
		cpu.vectors[destination].bytes : std::array<std::byte, 16>{};
	std::uint32_t exceptions{};
	const unsigned integer_bytes = decoded.integer_width / 8U;
	const unsigned destination_bytes = decoded.destination_width / 8U;
	for (unsigned lane = 0; lane < decoded.lanes; ++lane) {
		const auto input = decoded.source_gpr ? gpr_source :
			decode_little_endian(std::span{source}.subspan(
				lane * integer_bytes, integer_bytes));
		const auto converted = detail::convert_integer_to_floating_bits(
			input, decoded.integer_width, decoded.destination_width,
			decoded.fractional_bits, decoded.unsigned_source, cpu.fpcr);
		encode_little_endian(converted.bits,
			std::span{result}.subspan(
				lane * destination_bytes, destination_bytes));
		exceptions |= converted.exceptions;
	}
	cpu.vectors[destination].bytes = result;
	cpu.fpsr |= exceptions;
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::floating_to_integer_checked(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_floating_to_integer(instruction);
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned destination = instruction & 31U;
	const auto source = cpu.vectors[(instruction >> 5) & 31U].bytes;
	std::uint32_t exceptions{};
	if (decoded.destination_gpr) {
		const auto input = decode_little_endian(
			std::span{source}.first(decoded.source_width / 8U));
		const auto converted = detail::convert_floating_to_integer_bits(input,
			decoded.source_width, decoded.integer_width,
			decoded.fractional_bits,
			decoded.unsigned_result, decoded.rounding,
			cpu.fpcr);
		cpu.write_register(destination,
			decoded.integer_width == 64U ? converted.value :
				static_cast<std::uint32_t>(converted.value), false);
		exceptions = converted.exceptions;
	} else {
		auto result = decoded.scalar && (cpu.fpcr & 4U) != 0U ?
			cpu.vectors[destination].bytes : std::array<std::byte, 16>{};
		const unsigned lane_bytes = decoded.source_width / 8U;
		for (unsigned lane = 0; lane < decoded.lanes; ++lane) {
			const auto input = decode_little_endian(std::span{source}.subspan(
				lane * lane_bytes, lane_bytes));
			const auto converted = detail::convert_floating_to_integer_bits(
				input, decoded.source_width, decoded.integer_width,
				decoded.fractional_bits,
				decoded.unsigned_result, decoded.rounding, cpu.fpcr);
			encode_little_endian(converted.value,
				std::span{result}.subspan(lane * decoded.integer_width / 8U,
					decoded.integer_width / 8U));
			exceptions |= converted.exceptions;
		}
		cpu.vectors[destination].bytes = result;
	}
	cpu.fpsr |= exceptions;
	cpu.pc += 4U;
	return continue_after(address, instruction);
}

RunResult Dispatcher::simd_integer_float_convert(CpuState &cpu,
						  GuestMemory &,
						  std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto operation = instruction & 0xafbb'fc00U;
	const bool integer_to_float = operation == 0x0e21'd800U ||
		operation == 0x2e21'd800U;
	const bool unsigned_integer = operation == 0x2e21'd800U ||
		operation == 0x2ea1'b800U;
	if (!integer_to_float && operation != 0x0ea1'b800U &&
	    operation != 0x2ea1'b800U)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const bool is_double = (instruction & 0x0040'0000U) != 0;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const bool q = (instruction & 0x4000'0000U) != 0;
	if (is_double && !q && !scalar)
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	const std::size_t lane_size = is_double ? 8 : 4;
	const std::size_t vector_size = scalar ? lane_size : q ? 16 : 8;
	const auto source = cpu.vectors[(instruction >> 5) & 0x1fU].bytes;
	std::array<std::byte, 16> result{};
	for (std::size_t offset = 0; offset < vector_size; offset += lane_size) {
		const auto raw = decode_little_endian(
			std::span{source}.subspan(offset, lane_size));
		std::uint64_t converted;
		if (integer_to_float) {
			long double value;
			if (unsigned_integer)
				value = is_double ? static_cast<long double>(raw) :
					static_cast<long double>(static_cast<std::uint32_t>(raw));
			else
				value = is_double ? static_cast<long double>(
					static_cast<std::int64_t>(raw)) :
					static_cast<long double>(static_cast<std::int32_t>(raw));
			converted = is_double ? std::bit_cast<std::uint64_t>(
				static_cast<double>(value)) : std::bit_cast<std::uint32_t>(
				static_cast<float>(value));
		} else {
			long double value = is_double ? static_cast<long double>(
				std::bit_cast<double>(raw)) : static_cast<long double>(
				std::bit_cast<float>(static_cast<std::uint32_t>(raw)));
			value = std::trunc(value);
			if (std::isnan(value)) {
				converted = 0;
			} else if (unsigned_integer) {
				const auto maximum = is_double ? static_cast<long double>(
					std::numeric_limits<std::uint64_t>::max()) :
					static_cast<long double>(
						std::numeric_limits<std::uint32_t>::max());
				converted = value <= 0 ? 0 : value >= maximum ?
					(is_double ? ~std::uint64_t{0} : 0xffff'ffffULL) :
					static_cast<std::uint64_t>(value);
			} else {
				const auto minimum = is_double ? static_cast<long double>(
					std::numeric_limits<std::int64_t>::min()) :
					static_cast<long double>(
						std::numeric_limits<std::int32_t>::min());
				const auto maximum = is_double ? static_cast<long double>(
					std::numeric_limits<std::int64_t>::max()) :
					static_cast<long double>(
						std::numeric_limits<std::int32_t>::max());
				converted = value <= minimum ?
					(is_double ? std::uint64_t{1} << 63 : 0x8000'0000ULL) :
					value >= maximum ?
					(is_double ? 0x7fff'ffff'ffff'ffffULL : 0x7fff'ffffULL) :
					static_cast<std::uint64_t>(
						static_cast<std::int64_t>(value));
			}
		}
		encode_little_endian(converted,
			std::span{result}.subspan(offset, lane_size));
	}
	cpu.vectors[instruction & 0x1fU].bytes = result;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::backend::arm64
