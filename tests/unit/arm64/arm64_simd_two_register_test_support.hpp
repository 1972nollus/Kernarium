// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace arm64_simd_two_register_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Registers final {
	unsigned destination;
	unsigned source;
};

inline constexpr std::array target_opcodes{
	0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U,
	18U, 19U, 20U};

[[nodiscard]] constexpr std::uint32_t instruction(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode, Registers registers)
{
	return (scalar ? 0x5e20'0800U : 0x0e20'0800U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((opcode & 31U) << 12) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

/*
 * This is an independent transcription of the scalar and vector allocation
 * tables. Keep it separate from the production decoders: an invalid tuple is
 * required to stop before any architectural effect.
 */
[[nodiscard]] constexpr bool allocated(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode)
{
	if (scalar) {
		switch (opcode) {
		case 3:
		case 7:
			return true;
		case 8:
		case 9:
			return size == 3U;
		case 10:
			return !unsigned_group && size == 3U;
		case 11:
			return size == 3U;
		case 18:
			return unsigned_group && size < 3U;
		case 20:
			return size < 3U;
		default:
			return false;
		}
	}

	switch (opcode) {
	case 0:
		return unsigned_group ? size < 2U : size < 3U;
	case 1:
		return !unsigned_group && size == 0U;
	case 2:
	case 4:
	case 6:
		return size < 3U;
	case 3:
	case 7:
	case 8:
	case 9:
	case 11:
		return q || size < 3U;
	case 5:
		return unsigned_group ? size <= 1U : size == 0U;
	case 10:
		return !unsigned_group && (q || size < 3U);
	case 18:
	case 20:
		return size < 3U;
	case 19:
		return unsigned_group && size < 3U;
	default:
		return false;
	}
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
	if ((raw & (std::uint64_t{1} << (bits - 1U))) == 0)
		return static_cast<__int128>(raw);
	return static_cast<__int128>(raw) -
		(static_cast<__int128>(1) << bits);
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

[[nodiscard]] inline arm64::CpuState expected_integer_state(
	const arm64::CpuState &initial, bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode, Registers registers)
{
	const auto source = initial.vectors[registers.source].bytes;
	const auto old_destination =
		initial.vectors[registers.destination].bytes;
	std::array<std::byte, 16> result{};
	const std::size_t lane_size = std::size_t{1} << size;
	const unsigned lane_bits = static_cast<unsigned>(lane_size * 8U);
	const std::size_t vector_size = scalar ? lane_size : q ? 16U : 8U;
	bool saturated = false;

	if (opcode == 2U || opcode == 6U) {
		const std::size_t wide_size = lane_size * 2U;
		const unsigned wide_bits = lane_bits * 2U;
		for (std::size_t input = 0, output = 0; input < vector_size;
		     input += wide_size, output += wide_size) {
			const auto first = read_lane(source, input, lane_size);
			const auto second =
				read_lane(source, input + lane_size, lane_size);
			const __int128 sum = unsigned_group ?
				static_cast<__int128>(first) +
					static_cast<__int128>(second) :
				signed_value(first, lane_bits) +
					signed_value(second, lane_bits);
			std::uint64_t value = static_cast<std::uint64_t>(
				static_cast<unsigned __int128>(sum)) & mask(wide_bits);
			if (opcode == 6U)
				value = (value +
					read_lane(old_destination, output, wide_size)) &
					mask(wide_bits);
			write_lane(result, output, wide_size, value);
		}
	} else if (opcode == 3U) {
		for (std::size_t offset = 0; offset < vector_size;
		     offset += lane_size) {
			const auto input = read_lane(source, offset, lane_size);
			const auto destination =
				read_lane(old_destination, offset, lane_size);
			const auto value = unsigned_group ?
				saturate_unsigned(
					static_cast<__int128>(destination) +
						signed_value(input, lane_bits),
					lane_bits, saturated) :
				saturate_signed(
					signed_value(destination, lane_bits) +
						static_cast<__int128>(input),
					lane_bits, saturated);
			write_lane(result, offset, lane_size, value);
		}
	} else if (opcode == 4U) {
		for (std::size_t offset = 0; offset < vector_size;
		     offset += lane_size) {
			const auto input = read_lane(source, offset, lane_size);
			unsigned count = 0;
			if (unsigned_group) {
				for (int bit = static_cast<int>(lane_bits) - 1;
				     bit >= 0 && ((input >> bit) & 1U) == 0; --bit)
					++count;
			} else {
				const auto sign = (input >> (lane_bits - 1U)) & 1U;
				for (int bit = static_cast<int>(lane_bits) - 2;
				     bit >= 0 && ((input >> bit) & 1U) == sign; --bit)
					++count;
			}
			write_lane(result, offset, lane_size, count);
		}
	} else if (opcode == 7U || opcode == 11U) {
		for (std::size_t offset = 0; offset < vector_size;
		     offset += lane_size) {
			const auto input = read_lane(source, offset, lane_size);
			const auto signed_input = signed_value(input, lane_bits);
			const auto transformed = unsigned_group ? -signed_input :
				signed_input < 0 ? -signed_input : signed_input;
			const auto value = opcode == 7U ?
				saturate_signed(transformed, lane_bits, saturated) :
				static_cast<std::uint64_t>(
					static_cast<unsigned __int128>(transformed)) &
					mask(lane_bits);
			write_lane(result, offset, lane_size, value);
		}
	} else if (opcode == 18U || opcode == 20U) {
		const std::size_t source_size = lane_size * 2U;
		const unsigned source_bits = lane_bits * 2U;
		const std::size_t output_offset = !scalar && q ? 8U : 0U;
		if (output_offset != 0)
			std::copy_n(old_destination.begin(), 8, result.begin());
		const std::size_t input_size = scalar ? source_size : 16U;
		for (std::size_t input = 0, output = output_offset;
		     input < input_size;
		     input += source_size, output += lane_size) {
			const auto raw = read_lane(source, input, source_size);
			std::uint64_t value;
			if (opcode == 18U && unsigned_group)
				value = saturate_unsigned(
					signed_value(raw, source_bits), lane_bits, saturated);
			else if (opcode == 18U)
				value = raw & mask(lane_bits);
			else if (unsigned_group)
				value = saturate_unsigned(
					static_cast<__int128>(raw), lane_bits, saturated);
			else
				value = saturate_signed(
					signed_value(raw, source_bits), lane_bits, saturated);
			write_lane(result, output, lane_size, value);
		}
	} else if (opcode == 19U) {
		const std::size_t source_base = q ? 8U : 0U;
		for (std::size_t input = source_base, output = 0; output < 16U;
		     input += lane_size, output += lane_size * 2U) {
			const auto value =
				read_lane(source, input, lane_size) << lane_bits;
			write_lane(result, output, lane_size * 2U, value);
		}
	}

	auto expected = initial;
	expected.vectors[registers.destination].bytes = result;
	if (saturated)
		expected.fpsr |= 1U << 27;
	return expected;
}

[[nodiscard]] constexpr std::uint8_t reverse_bits(std::uint8_t value)
{
	value = static_cast<std::uint8_t>((value >> 4) | (value << 4));
	value = static_cast<std::uint8_t>(
		((value & 0xccU) >> 2) | ((value & 0x33U) << 2));
	return static_cast<std::uint8_t>(
		((value & 0xaaU) >> 1) | ((value & 0x55U) << 1));
}

[[nodiscard]] inline arm64::CpuState expected_bit_unary_state(
	const arm64::CpuState &initial, bool q, bool unsigned_group,
	unsigned size, unsigned opcode, Registers registers)
{
	const auto source = initial.vectors[registers.source].bytes;
	std::array<std::byte, 16> result{};
	const std::size_t vector_size = q ? 16U : 8U;
	if (opcode == 5U) {
		for (std::size_t byte = 0; byte < vector_size; ++byte) {
			const auto value =
				std::to_integer<std::uint8_t>(source[byte]);
			result[byte] = unsigned_group && size == 0U ?
				static_cast<std::byte>(~value) :
				unsigned_group ?
					static_cast<std::byte>(reverse_bits(value)) :
					static_cast<std::byte>(std::popcount(value));
		}
	} else {
		const std::size_t block_size =
			opcode == 0U && !unsigned_group ? 8U :
			opcode == 0U ? 4U : 2U;
		const std::size_t lane_size = std::size_t{1} << size;
		for (std::size_t block = 0; block < vector_size;
		     block += block_size)
			for (std::size_t lane = 0; lane < block_size;
			     lane += lane_size)
				std::copy_n(source.begin() + block + lane, lane_size,
					result.begin() + block + block_size - lane -
						lane_size);
	}
	auto expected = initial;
	expected.vectors[registers.destination].bytes = result;
	return expected;
}

[[nodiscard]] inline arm64::CpuState expected_compare_zero_state(
	const arm64::CpuState &initial, bool scalar, bool q, bool alternate,
	unsigned size, unsigned opcode, Registers registers)
{
	const auto source = initial.vectors[registers.source].bytes;
	std::array<std::byte, 16> result{};
	const std::size_t lane_size = std::size_t{1} << size;
	const unsigned lane_bits = static_cast<unsigned>(lane_size * 8U);
	const std::size_t vector_size = scalar ? lane_size : q ? 16U : 8U;
	for (std::size_t offset = 0; offset < vector_size;
	     offset += lane_size) {
		const auto value =
			signed_value(read_lane(source, offset, lane_size), lane_bits);
		const bool matches = opcode == 8U ?
			(alternate ? value >= 0 : value > 0) :
			opcode == 9U ? (alternate ? value <= 0 : value == 0) :
				value < 0;
		for (std::size_t byte = 0; byte < lane_size; ++byte)
			result[offset + byte] =
				matches ? std::byte{0xff} : std::byte{};
	}
	auto expected = initial;
	expected.vectors[registers.destination].bytes = result;
	return expected;
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode, Registers registers)
{
	if (!scalar && (opcode == 0U || opcode == 1U || opcode == 5U))
		return expected_bit_unary_state(
			initial, q, unsigned_group, size, opcode, registers);
	if (opcode >= 8U && opcode <= 10U)
		return expected_compare_zero_state(initial, scalar, q,
			unsigned_group, size, opcode, registers);
	return expected_integer_state(initial, scalar, q, unsigned_group,
		size, opcode, registers);
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::advanced_simd);
}

template<typename Callback>
void for_each_register_case(Callback &&callback)
{
	for (unsigned destination = 0; destination < 32U; ++destination)
		callback(Registers{destination, 11U}, "rd", destination);
	for (unsigned source = 0; source < 32U; ++source)
		callback(Registers{7U, source}, "rn", source);
	for (unsigned reg = 0; reg < 32U; ++reg)
		callback(Registers{reg, reg}, "destructive", reg);
}

template<typename Opcodes, typename Callback>
void for_each_allocated_form(const Opcodes &opcodes, Callback &&callback)
{
	for (const auto opcode : opcodes) {
		for (const bool q : {false, true})
			for (const bool unsigned_group : {false, true})
				for (unsigned size = 0; size < 4U; ++size)
					if (allocated(false, q, unsigned_group,
					    size, opcode))
						callback(false, q, unsigned_group,
							size, opcode);
		for (const bool unsigned_group : {false, true})
			for (unsigned size = 0; size < 4U; ++size)
				if (allocated(true, false, unsigned_group,
				    size, opcode))
					callback(true, false, unsigned_group,
						size, opcode);
	}
}

inline void check_form(engine_tests::EngineHarness &harness,
	std::string_view family, bool scalar, bool q, bool unsigned_group,
	unsigned size, unsigned opcode)
{
	for_each_register_case([&](Registers registers, const char *register_axis,
				       unsigned register_value) {
		const auto encoded = instruction(scalar, q, unsigned_group, size,
			opcode, registers);
		const unsigned seed = encoded ^ (register_value * 0x45d9'f3bU);
		const auto initial = state(seed);
		const auto expected = expected_state(initial, scalar, q,
			unsigned_group, size, opcode, registers);
		char name[192];
		std::snprintf(name, sizeof(name),
			"%.*s scalar=%u q=%u u=%u size=%u opcode=%u %s=%u",
			static_cast<int>(family.size()), family.data(),
			scalar ? 1U : 0U, q ? 1U : 0U,
			unsigned_group ? 1U : 0U, size, opcode,
			register_axis, register_value);
		harness.check_one(name, encoded, initial, expected,
			native_requirements());
	});
}

} // namespace arm64_simd_two_register_test
