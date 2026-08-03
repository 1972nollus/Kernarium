// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_simd_reduce_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Registers final {
	unsigned destination;
	unsigned source;
};

[[nodiscard]] constexpr std::uint32_t instruction(bool q,
	bool unsigned_group, unsigned size, unsigned opcode, Registers registers)
{
	return 0x0e30'0800U | (q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((opcode & 31U) << 12) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr bool valid(bool q, bool unsigned_group,
	unsigned size, unsigned opcode)
{
	const bool valid_shape = size < 3U && (q || size < 2U);
	const bool valid_operation = opcode == 3U || opcode == 10U ||
		opcode == 26U || (opcode == 27U && !unsigned_group);
	return valid_shape && valid_operation;
}

[[nodiscard]] constexpr std::string_view operation_name(
	bool unsigned_group, unsigned opcode)
{
	if (opcode == 3U)
		return unsigned_group ? "unsigned-add-long-across-vector" :
			"signed-add-long-across-vector";
	if (opcode == 10U)
		return unsigned_group ? "unsigned-maximum-across-vector" :
			"signed-maximum-across-vector";
	if (opcode == 26U)
		return unsigned_group ?
			"FPSIMD-0428 UMINV unsigned-minimum-across-vector" :
			"FPSIMD-0321 SMINV signed-minimum-across-vector";
	return "add-across-vector";
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
	cpu.fpsr = 0x0800'0011U | (seed & 0x4U);
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	cpu.set_mte_configuration(true, 0xa55aU);
	cpu.set_mte_generator(true, 0x1357U, 9U,
		0x3141'5926'5358'9793ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0x89ab'cdefU;
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_sm = true;
	cpu.pstate_za = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xbU;
	cpu.exclusive_address = 0x1234'5000ULL;
	cpu.exclusive_key = 0x2468U;
	cpu.exclusive_value = 0x1122'3344'5566'7788ULL;
	cpu.exclusive_value2 = 0x8877'6655'4433'2211ULL;
	cpu.exclusive_version = 0x1020'3040'5060'7080ULL;
	cpu.exclusive_size = 8;
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
			std::to_integer<unsigned>(bytes[offset + byte])) <<
			(byte * 8U);
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

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, bool q, bool unsigned_group,
	unsigned size, unsigned opcode, Registers registers)
{
	const auto source = initial.vectors[registers.source].bytes;
	const std::size_t lane_size = std::size_t{1} << size;
	const unsigned lane_bits = static_cast<unsigned>(lane_size * 8U);
	const std::size_t vector_size = q ? 16U : 8U;
	const bool widening_add = opcode == 3U;
	const unsigned result_bits =
		widening_add ? lane_bits * 2U : lane_bits;
	__int128 result = unsigned_group ?
		static_cast<__int128>(read_lane(source, 0, lane_size)) :
		signed_value(read_lane(source, 0, lane_size), lane_bits);

	for (std::size_t offset = lane_size; offset < vector_size;
	     offset += lane_size) {
		const auto raw = read_lane(source, offset, lane_size);
		const auto value = unsigned_group ?
			static_cast<__int128>(raw) : signed_value(raw, lane_bits);
		if (opcode == 3U || opcode == 27U)
			result += value;
		else if ((opcode == 10U && value > result) ||
			 (opcode == 26U && value < result))
			result = value;
	}

	std::array<std::byte, 16> destination{};
	write_lane(destination, 0, result_bits / 8U,
		static_cast<std::uint64_t>(
			static_cast<unsigned __int128>(result)) & mask(result_bits));
	auto expected = initial;
	expected.vectors[registers.destination].bytes = destination;
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::advanced_simd);
}

} // namespace arm64_simd_reduce_test
