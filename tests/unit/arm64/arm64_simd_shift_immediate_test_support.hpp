// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace arm64_simd_shift_immediate_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

inline constexpr unsigned qc = 1U << 27;

enum class Form {
	vector_64,
	vector_128,
	scalar,
};

enum class Operation {
	sshr,
	ushr,
	ssra,
	usra,
	srshr,
	urshr,
	srsra,
	ursra,
	sri,
	shl,
	sli,
	sqshl,
	uqshl,
	sqshlu,
	shrn,
	rshrn,
	sqshrn,
	sqrshrn,
	sqshrun,
	sqrshrun,
	uqshrn,
	uqrshrn,
};

struct Encoding final {
	bool unsigned_group;
	unsigned opcode;
};

[[nodiscard]] constexpr Encoding encoding(Operation operation)
{
	switch (operation) {
	case Operation::sshr: return {false, 0U};
	case Operation::ushr: return {true, 0U};
	case Operation::ssra: return {false, 2U};
	case Operation::usra: return {true, 2U};
	case Operation::srshr: return {false, 4U};
	case Operation::urshr: return {true, 4U};
	case Operation::srsra: return {false, 6U};
	case Operation::ursra: return {true, 6U};
	case Operation::sri: return {true, 8U};
	case Operation::shl: return {false, 10U};
	case Operation::sli: return {true, 10U};
	case Operation::sqshlu: return {true, 12U};
	case Operation::sqshl: return {false, 14U};
	case Operation::uqshl: return {true, 14U};
	case Operation::shrn: return {false, 16U};
	case Operation::rshrn: return {false, 17U};
	case Operation::sqshrun: return {true, 16U};
	case Operation::sqrshrun: return {true, 17U};
	case Operation::sqshrn: return {false, 18U};
	case Operation::uqshrn: return {true, 18U};
	case Operation::sqrshrn: return {false, 19U};
	case Operation::uqrshrn: return {true, 19U};
	}
	return {};
}

[[nodiscard]] constexpr std::optional<Operation> operation_for(
	bool unsigned_group, unsigned opcode)
{
	switch (opcode) {
	case 0U:
		return unsigned_group ? Operation::ushr : Operation::sshr;
	case 2U:
		return unsigned_group ? Operation::usra : Operation::ssra;
	case 4U:
		return unsigned_group ? Operation::urshr : Operation::srshr;
	case 6U:
		return unsigned_group ? Operation::ursra : Operation::srsra;
	case 8U:
		return unsigned_group ? std::optional{Operation::sri} : std::nullopt;
	case 10U:
		return unsigned_group ? Operation::sli : Operation::shl;
	case 12U:
		return unsigned_group ? std::optional{Operation::sqshlu} :
			std::nullopt;
	case 14U:
		return unsigned_group ? Operation::uqshl : Operation::sqshl;
	case 16U:
		return unsigned_group ? Operation::sqshrun : Operation::shrn;
	case 17U:
		return unsigned_group ? Operation::sqrshrun : Operation::rshrn;
	case 18U:
		return unsigned_group ? Operation::uqshrn : Operation::sqshrn;
	case 19U:
		return unsigned_group ? Operation::uqrshrn : Operation::sqrshrn;
	default:
		return std::nullopt;
	}
}

[[nodiscard]] constexpr bool is_narrow(Operation operation)
{
	return operation == Operation::shrn || operation == Operation::rshrn ||
		operation == Operation::sqshrn ||
		operation == Operation::sqrshrn ||
		operation == Operation::sqshrun ||
		operation == Operation::sqrshrun ||
		operation == Operation::uqshrn ||
		operation == Operation::uqrshrn;
}

[[nodiscard]] constexpr bool is_saturating_narrow(Operation operation)
{
	return is_narrow(operation) && operation != Operation::shrn &&
		operation != Operation::rshrn;
}

[[nodiscard]] constexpr bool is_saturating_left(Operation operation)
{
	return operation == Operation::sqshl || operation == Operation::uqshl ||
		operation == Operation::sqshlu;
}

[[nodiscard]] constexpr bool is_left(Operation operation)
{
	return operation == Operation::shl || operation == Operation::sli ||
		is_saturating_left(operation);
}

[[nodiscard]] constexpr bool valid(
	Operation operation, Form form, unsigned encoded)
{
	if (encoded < 8U)
		return false;
	if (is_narrow(operation))
		return encoded < 64U &&
			(form != Form::scalar || is_saturating_narrow(operation));

	const unsigned lane_bits = std::bit_floor(encoded);
	if (lane_bits > 64U)
		return false;
	if (form == Form::vector_64 && lane_bits == 64U)
		return false;
	if (form == Form::scalar && !is_saturating_left(operation) &&
	    lane_bits != 64U)
		return false;
	return true;
}

[[nodiscard]] constexpr std::uint32_t raw_instruction(Form form,
	bool unsigned_group, unsigned opcode, unsigned encoded,
	unsigned destination, unsigned source)
{
	return (form == Form::scalar ? 0x5f00'0400U : 0x0f00'0400U) |
		(form == Form::vector_128 ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((encoded & 127U) << 16) | ((opcode & 31U) << 11) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	Operation operation, unsigned encoded, unsigned destination,
	unsigned source)
{
	const auto fields = encoding(operation);
	return raw_instruction(form, fields.unsigned_group, fields.opcode,
		encoded, destination, source);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed * 67U + reg * 29U + byte * 17U + 3U);
	for (unsigned reg = 0; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(reg + seed) *
			 0x0101'0101'0101'0101ULL);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = seed & 1U;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	return cpu;
}

[[nodiscard]] constexpr std::uint64_t mask(unsigned bits)
{
	return bits == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] inline std::uint64_t read_lane(
	const std::array<std::byte, 16> &bytes, std::size_t offset,
	std::size_t size)
{
	std::uint64_t value = 0;
	for (std::size_t byte = 0; byte < size; ++byte)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(bytes[offset + byte])) << (byte * 8U);
	return value;
}

inline void write_lane(std::array<std::byte, 16> &bytes,
	std::size_t offset, std::size_t size, std::uint64_t value)
{
	for (std::size_t byte = 0; byte < size; ++byte)
		bytes[offset + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

[[nodiscard]] constexpr __int128 signed_value(
	std::uint64_t raw, unsigned bits)
{
	raw &= mask(bits);
	const auto sign = std::uint64_t{1} << (bits - 1U);
	return (raw & sign) == 0 ? static_cast<__int128>(raw) :
		static_cast<__int128>(raw) -
			(static_cast<__int128>(1) << bits);
}

[[nodiscard]] constexpr __int128 arithmetic_shift_right(
	__int128 value, unsigned shift)
{
	const __int128 divisor = static_cast<__int128>(1) << shift;
	return value >= 0 ? value / divisor :
		-((-value + divisor - 1) / divisor);
}

[[nodiscard]] constexpr __int128 rounded_signed_shift_right(
	std::uint64_t raw, unsigned bits, unsigned shift)
{
	return arithmetic_shift_right(signed_value(raw, bits) +
		(static_cast<__int128>(1) << (shift - 1U)), shift);
}

[[nodiscard]] constexpr unsigned __int128 rounded_unsigned_shift_right(
	std::uint64_t raw, unsigned shift)
{
	return (static_cast<unsigned __int128>(raw) +
		(static_cast<unsigned __int128>(1) << (shift - 1U))) >> shift;
}

[[nodiscard]] inline std::uint64_t saturate_signed(
	__int128 value, unsigned bits, bool &saturated)
{
	const __int128 minimum =
		-(static_cast<__int128>(1) << (bits - 1U));
	const __int128 maximum =
		(static_cast<__int128>(1) << (bits - 1U)) - 1;
	if (value < minimum) {
		value = minimum;
		saturated = true;
	} else if (value > maximum) {
		value = maximum;
		saturated = true;
	}
	return static_cast<std::uint64_t>(
		static_cast<unsigned __int128>(value)) & mask(bits);
}

[[nodiscard]] inline std::uint64_t saturate_unsigned(
	__int128 value, unsigned bits, bool &saturated)
{
	const auto maximum =
		(static_cast<unsigned __int128>(1) << bits) - 1U;
	if (value < 0) {
		saturated = true;
		return 0;
	}
	const auto unsigned_value = static_cast<unsigned __int128>(value);
	if (unsigned_value > maximum) {
		saturated = true;
		return static_cast<std::uint64_t>(maximum);
	}
	return static_cast<std::uint64_t>(unsigned_value);
}

[[nodiscard]] inline std::uint64_t saturate_unsigned(
	unsigned __int128 value, unsigned bits, bool &saturated)
{
	const auto maximum =
		(static_cast<unsigned __int128>(1) << bits) - 1U;
	if (value > maximum) {
		saturated = true;
		return static_cast<std::uint64_t>(maximum);
	}
	return static_cast<std::uint64_t>(value);
}

[[nodiscard]] inline arm64::CpuState expected_regular_state(
	const arm64::CpuState &initial, Form form, Operation operation,
	unsigned encoded, unsigned destination, unsigned source)
{
	const unsigned lane_bits = std::bit_floor(encoded);
	const std::size_t lane_size = lane_bits / 8U;
	const std::size_t vector_size = form == Form::scalar ? lane_size :
		form == Form::vector_128 ? 16U : 8U;
	const unsigned shift = is_left(operation) ?
		encoded - lane_bits : 2U * lane_bits - encoded;
	const auto old_source = initial.vectors[source].bytes;
	const auto old_destination = initial.vectors[destination].bytes;
	auto expected = initial;
	expected.vectors[destination] = {};
	bool saturated = false;

	for (std::size_t offset = 0; offset < vector_size;
	     offset += lane_size) {
		const auto raw = read_lane(old_source, offset, lane_size);
		const auto old = read_lane(old_destination, offset, lane_size);
		std::uint64_t value{};
		switch (operation) {
		case Operation::sshr:
			value = static_cast<std::uint64_t>(
				arithmetic_shift_right(
					signed_value(raw, lane_bits), shift));
			break;
		case Operation::ushr:
			value = shift == lane_bits ? 0U : raw >> shift;
			break;
		case Operation::ssra:
			value = old + static_cast<std::uint64_t>(
				arithmetic_shift_right(
					signed_value(raw, lane_bits), shift));
			break;
		case Operation::usra:
			value = old + (shift == lane_bits ? 0U : raw >> shift);
			break;
		case Operation::srshr:
			value = static_cast<std::uint64_t>(
				rounded_signed_shift_right(raw, lane_bits, shift));
			break;
		case Operation::urshr:
			value = static_cast<std::uint64_t>(
				rounded_unsigned_shift_right(raw, shift));
			break;
		case Operation::srsra:
			value = old + static_cast<std::uint64_t>(
				rounded_signed_shift_right(raw, lane_bits, shift));
			break;
		case Operation::ursra:
			value = old + static_cast<std::uint64_t>(
				rounded_unsigned_shift_right(raw, shift));
			break;
		case Operation::sri: {
			const auto insert_mask =
				shift == lane_bits ? 0U : mask(lane_bits) >> shift;
			value = (old & ~insert_mask) |
				(shift == lane_bits ? 0U : raw >> shift);
			break;
		}
		case Operation::shl:
			value = static_cast<std::uint64_t>(
				static_cast<unsigned __int128>(raw) << shift);
			break;
		case Operation::sli: {
			const auto preserve_mask =
				shift == 0U ? 0U :
				(std::uint64_t{1} << shift) - 1U;
			value = static_cast<std::uint64_t>(
				static_cast<unsigned __int128>(raw) << shift) |
				(old & preserve_mask);
			break;
		}
		case Operation::sqshl:
			value = saturate_signed(
				signed_value(raw, lane_bits) *
					(static_cast<__int128>(1) << shift),
				lane_bits, saturated);
			break;
		case Operation::uqshl:
			value = saturate_unsigned(
				static_cast<unsigned __int128>(raw) << shift,
				lane_bits, saturated);
			break;
		case Operation::sqshlu:
			value = saturate_unsigned(
				signed_value(raw, lane_bits) *
					(static_cast<__int128>(1) << shift),
				lane_bits, saturated);
			break;
		default:
			break;
		}
		write_lane(expected.vectors[destination].bytes, offset,
			lane_size, value & mask(lane_bits));
	}
	if (saturated)
		expected.fpsr |= qc;
	return expected;
}

[[nodiscard]] inline arm64::CpuState expected_narrow_state(
	const arm64::CpuState &initial, Form form, Operation operation,
	unsigned encoded, unsigned destination, unsigned source)
{
	const unsigned destination_bits = std::bit_floor(encoded);
	const unsigned source_bits = destination_bits * 2U;
	const std::size_t destination_size = destination_bits / 8U;
	const std::size_t source_size = source_bits / 8U;
	const unsigned shift = source_bits - encoded;
	const auto old_source = initial.vectors[source].bytes;
	const auto old_destination = initial.vectors[destination].bytes;
	auto expected = initial;
	expected.vectors[destination] = {};
	const std::size_t output_base =
		form == Form::vector_128 ? 8U : 0U;
	if (output_base != 0U)
		for (std::size_t byte = 0; byte < output_base; ++byte)
			expected.vectors[destination].bytes[byte] =
				old_destination[byte];
	const std::size_t source_bytes =
		form == Form::scalar ? source_size : 16U;
	bool saturated = false;

	for (std::size_t input = 0, output = output_base;
	     input < source_bytes;
	     input += source_size, output += destination_size) {
		const auto raw = read_lane(old_source, input, source_size);
		std::uint64_t value{};
		switch (operation) {
		case Operation::shrn:
			value = raw >> shift;
			break;
		case Operation::rshrn:
			value = static_cast<std::uint64_t>(
				rounded_unsigned_shift_right(raw, shift));
			break;
		case Operation::sqshrn:
			value = saturate_signed(
				arithmetic_shift_right(
					signed_value(raw, source_bits), shift),
				destination_bits, saturated);
			break;
		case Operation::sqrshrn:
			value = saturate_signed(
				rounded_signed_shift_right(raw, source_bits, shift),
				destination_bits, saturated);
			break;
		case Operation::sqshrun:
			value = saturate_unsigned(
				arithmetic_shift_right(
					signed_value(raw, source_bits), shift),
				destination_bits, saturated);
			break;
		case Operation::sqrshrun:
			value = saturate_unsigned(
				rounded_signed_shift_right(raw, source_bits, shift),
				destination_bits, saturated);
			break;
		case Operation::uqshrn:
			value = saturate_unsigned(
				static_cast<unsigned __int128>(raw) >> shift,
				destination_bits, saturated);
			break;
		case Operation::uqrshrn:
			value = saturate_unsigned(
				rounded_unsigned_shift_right(raw, shift),
				destination_bits, saturated);
			break;
		default:
			break;
		}
		write_lane(expected.vectors[destination].bytes, output,
			destination_size, value);
	}
	if (saturated)
		expected.fpsr |= qc;
	return expected;
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, Form form, Operation operation,
	unsigned encoded, unsigned destination, unsigned source)
{
	return is_narrow(operation) ?
		expected_narrow_state(initial, form, operation, encoded,
			destination, source) :
		expected_regular_state(initial, form, operation, encoded,
			destination, source);
}

inline void set_lane_pattern(arm64::CpuState &cpu, unsigned reg,
	unsigned lane_bits, std::size_t bytes, unsigned pattern)
{
	const std::size_t lane_size = lane_bits / 8U;
	for (std::size_t offset = 0, lane = 0; offset < bytes;
	     offset += lane_size, ++lane) {
		std::uint64_t value{};
		switch (pattern) {
		case 0:
			continue;
		case 1:
			value = mask(lane_bits);
			break;
		case 2:
			value = (lane & 1U) == 0 ?
				std::uint64_t{1} << (lane_bits - 1U) :
				(std::uint64_t{1} << (lane_bits - 1U)) - 1U;
			break;
		default:
			value = (lane & 1U) == 0 ? 1U : mask(lane_bits) - 1U;
			break;
		}
		write_lane(cpu.vectors[reg].bytes, offset, lane_size, value);
	}
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::advanced_simd);
}

} // namespace arm64_simd_shift_immediate_test
