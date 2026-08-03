// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace arm64_simd_extract_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

[[nodiscard]] constexpr std::uint32_t instruction(bool q, unsigned offset,
	Registers registers)
{
	return 0x2e00'0000U | (q ? 0x4000'0000U : 0U) |
		((registers.right & 31U) << 16) | ((offset & 15U) << 11) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr bool valid(bool q, unsigned offset)
{
	return q || offset < 8U;
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

/*
 * Independent byte-concatenation model. Both inputs are copied before the
 * destination changes so all destructive register aliases are modeled.
 */
[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, bool q, unsigned offset,
	Registers registers)
{
	const unsigned bytes = q ? 16U : 8U;
	const auto left = initial.vectors[registers.left].bytes;
	const auto right = initial.vectors[registers.right].bytes;
	auto expected = initial;
	auto &destination = expected.vectors[registers.destination].bytes;
	for (unsigned output = 0; output < bytes; ++output) {
		const unsigned source = offset + output;
		destination[output] = source < bytes ? left[source] :
			right[source - bytes];
	}
	std::fill(destination.begin() + bytes, destination.end(), std::byte{});
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::advanced_simd);
}

} // namespace arm64_simd_extract_test
