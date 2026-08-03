// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace arm64_simd_shift_left_long_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

[[nodiscard]] constexpr std::uint32_t instruction(bool high,
	bool unsigned_source, unsigned encoded, unsigned destination,
	unsigned source)
{
	return 0x0f00'a400U | (high ? 0x4000'0000U : 0U) |
		(unsigned_source ? 0x2000'0000U : 0U) |
		((encoded & 127U) << 16) | ((source & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr bool valid(unsigned encoded)
{
	/* immh==0000 belongs to a different allocation. */
	return encoded >= 8U && encoded < 64U;
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
	return cpu;
}

[[nodiscard]] inline std::uint64_t read_lane(
	const std::array<std::byte, 16> &bytes, unsigned offset,
	unsigned lane_bytes)
{
	std::uint64_t value = 0;
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(bytes[offset + byte])) <<
			(byte * 8U);
	return value;
}

inline void write_lane(std::array<std::byte, 16> &bytes, unsigned offset,
	unsigned lane_bytes, std::uint64_t value)
{
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		bytes[offset + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

[[nodiscard]] constexpr __int128 sign_extend(
	std::uint64_t value, unsigned bits)
{
	const auto sign = static_cast<unsigned __int128>(1) << (bits - 1U);
	return static_cast<__int128>(
		(static_cast<unsigned __int128>(value) ^ sign) - sign);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, bool high, bool unsigned_source,
	unsigned source_bits, unsigned shift, unsigned destination,
	unsigned source)
{
	const unsigned source_bytes = source_bits / 8U;
	const unsigned destination_bytes = source_bytes * 2U;
	const auto old_source = initial.vectors[source].bytes;
	auto expected = initial;
	expected.vectors[destination] = {};
	for (unsigned input = high ? 8U : 0U, output = 0; output < 16U;
	     input += source_bytes, output += destination_bytes) {
		const auto raw = read_lane(old_source, input, source_bytes);
		const __int128 value =
			unsigned_source ? raw : sign_extend(raw, source_bits);
		write_lane(expected.vectors[destination].bytes, output,
			destination_bytes,
			static_cast<std::uint64_t>(
				value * (static_cast<__int128>(1) << shift)));
	}
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::advanced_simd);
}

} // namespace arm64_simd_shift_left_long_test
