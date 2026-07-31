// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace arm64_simd_addp_scalar_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

[[nodiscard]] constexpr std::uint32_t instruction(
	unsigned destination, unsigned source)
{
	return 0x5ef1'b800U | ((source & 31U) << 5) | (destination & 31U);
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

[[nodiscard]] inline std::uint64_t element(
	const arm64::VectorRegister &vector, unsigned index)
{
	std::uint64_t value;
	std::memcpy(&value, vector.bytes.data() + index * sizeof(value),
		sizeof(value));
	return value;
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, unsigned destination, unsigned source)
{
	const std::uint64_t sum =
		element(initial.vectors[source], 0) +
		element(initial.vectors[source], 1);
	auto expected = initial;
	expected.vectors[destination] = {};
	std::memcpy(expected.vectors[destination].bytes.data(), &sum,
		sizeof(sum));
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		uml::backend::ArmFeature::advanced_simd);
}

} // namespace arm64_simd_addp_scalar_test
