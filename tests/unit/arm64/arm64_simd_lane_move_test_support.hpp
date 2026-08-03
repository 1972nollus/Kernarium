// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace arm64_simd_lane_move_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Registers final {
	unsigned destination;
	unsigned source;
};

struct Lane final {
	unsigned element_shift;
	unsigned element_bytes;
	unsigned index;
	unsigned offset;
};

/*
 * The shared imm5 encoding selects the element width with its lowest set bit.
 * Bits above that bit are the element index. The all-zero low nibble has no
 * element width and is reserved, even when imm5<4> is set.
 */
[[nodiscard]] constexpr std::optional<Lane> decode_lane(unsigned immediate)
{
	immediate &= 31U;
	if ((immediate & 15U) == 0)
		return std::nullopt;
	const unsigned shift = std::countr_zero(immediate);
	const unsigned index = immediate >> (shift + 1U);
	const unsigned bytes = 1U << shift;
	return Lane{shift, bytes, index, index * bytes};
}

[[nodiscard]] constexpr std::uint32_t duplicate_element_instruction(
	bool scalar, bool q, unsigned immediate, Registers registers)
{
	return (scalar ? 0x5e00'0400U : 0x0e00'0400U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		((immediate & 31U) << 16) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t duplicate_general_instruction(
	bool q, unsigned immediate, Registers registers)
{
	return 0x0e00'0c00U | (q ? 0x4000'0000U : 0U) |
		((immediate & 31U) << 16) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t insert_general_instruction(
	unsigned immediate, Registers registers)
{
	return 0x4e00'1c00U | ((immediate & 31U) << 16) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t insert_element_instruction(
	unsigned immediate, unsigned source_immediate, Registers registers)
{
	return 0x6e00'0400U | ((immediate & 31U) << 16) |
		((source_immediate & 15U) << 11) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t gpr_move_instruction(
	bool signed_move, bool wide, unsigned immediate, Registers registers)
{
	return (signed_move ? 0x0e00'2c00U : 0x0e00'3c00U) |
		(wide ? 0x4000'0000U : 0U) |
		((immediate & 31U) << 16) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr bool valid_duplicate(
	bool scalar, bool q, unsigned immediate)
{
	const auto lane = decode_lane(immediate);
	return lane && (scalar || q || lane->element_shift != 3U);
}

[[nodiscard]] constexpr bool valid_insert(unsigned immediate)
{
	return decode_lane(immediate).has_value();
}

[[nodiscard]] constexpr bool valid_gpr_move(
	bool signed_move, bool wide, unsigned immediate)
{
	const auto lane = decode_lane(immediate);
	if (!lane)
		return false;
	if (signed_move)
		return lane->element_shift < (wide ? 3U : 2U);
	return wide ? lane->element_shift == 3U :
		lane->element_shift < 3U;
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed)
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
	cpu.set_mte_configuration(true, 0xa55aU);
	cpu.set_mte_generator(true, 0x1357U, 9U,
		0x3141'5926'5358'9793ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0x2468'ace0U;
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 9U;
	cpu.exclusive_address = 0x8000'1230ULL;
	cpu.exclusive_key = 0x1234U;
	cpu.exclusive_value = 0x0123'4567'89ab'cdefULL;
	cpu.exclusive_value2 = 0xfedc'ba98'7654'3210ULL;
	cpu.exclusive_version = 37U;
	cpu.exclusive_size = 8U;
	return cpu;
}

[[nodiscard]] constexpr std::uint64_t read_general(
	const arm64::CpuState &initial, unsigned index)
{
	return index < initial.registers.size() ? initial.registers[index] : 0;
}

[[nodiscard]] inline arm64::CpuState expected_duplicate_element(
	const arm64::CpuState &initial, bool scalar, bool q, unsigned immediate,
	Registers registers)
{
	const auto lane = *decode_lane(immediate);
	const auto source = initial.vectors[registers.source].bytes;
	const unsigned output_bytes =
		scalar ? lane.element_bytes : q ? 16U : 8U;
	auto expected = initial;
	auto &destination = expected.vectors[registers.destination].bytes;
	destination.fill(std::byte{});
	for (unsigned output = 0; output < output_bytes; ++output)
		destination[output] =
			source[lane.offset + output % lane.element_bytes];
	return expected;
}

[[nodiscard]] inline arm64::CpuState expected_duplicate_general(
	const arm64::CpuState &initial, bool q, unsigned immediate,
	Registers registers)
{
	const auto lane = *decode_lane(immediate);
	const auto source = read_general(initial, registers.source);
	const unsigned output_bytes = q ? 16U : 8U;
	auto expected = initial;
	auto &destination = expected.vectors[registers.destination].bytes;
	destination.fill(std::byte{});
	for (unsigned output = 0; output < output_bytes; ++output)
		destination[output] = static_cast<std::byte>(
			source >> ((output % lane.element_bytes) * 8U));
	return expected;
}

[[nodiscard]] inline arm64::CpuState expected_insert_general(
	const arm64::CpuState &initial, unsigned immediate, Registers registers)
{
	const auto lane = *decode_lane(immediate);
	const auto source = read_general(initial, registers.source);
	auto expected = initial;
	auto &destination = expected.vectors[registers.destination].bytes;
	for (unsigned byte = 0; byte < lane.element_bytes; ++byte)
		destination[lane.offset + byte] =
			static_cast<std::byte>(source >> (byte * 8U));
	return expected;
}

[[nodiscard]] inline arm64::CpuState expected_insert_element(
	const arm64::CpuState &initial, unsigned immediate,
	unsigned source_immediate, Registers registers)
{
	const auto lane = *decode_lane(immediate);
	const auto source = initial.vectors[registers.source].bytes;
	const unsigned source_offset =
		(source_immediate >> lane.element_shift) << lane.element_shift;
	auto expected = initial;
	auto &destination = expected.vectors[registers.destination].bytes;
	for (unsigned byte = 0; byte < lane.element_bytes; ++byte)
		destination[lane.offset + byte] = source[source_offset + byte];
	return expected;
}

[[nodiscard]] constexpr std::uint64_t lane_value(
	const arm64::VectorRegister &source, unsigned offset, unsigned bytes)
{
	std::uint64_t result = 0;
	for (unsigned byte = 0; byte < bytes; ++byte)
		result |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(source.bytes[offset + byte])) <<
			(byte * 8U);
	return result;
}

[[nodiscard]] constexpr std::uint64_t sign_extend(
	std::uint64_t value, unsigned bits)
{
	const auto sign = std::uint64_t{1} << (bits - 1U);
	return (value ^ sign) - sign;
}

[[nodiscard]] inline arm64::CpuState expected_gpr_move(
	const arm64::CpuState &initial, bool signed_move, bool wide,
	unsigned immediate, Registers registers)
{
	const auto lane = *decode_lane(immediate);
	auto value = lane_value(initial.vectors[registers.source],
		lane.offset, lane.element_bytes);
	if (signed_move)
		value = sign_extend(value, lane.element_bytes * 8U);
	if (!wide)
		value &= 0xffff'ffffULL;
	auto expected = initial;
	if (registers.destination < expected.registers.size())
		expected.registers[registers.destination] = value;
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::advanced_simd);
}

} // namespace arm64_simd_lane_move_test
