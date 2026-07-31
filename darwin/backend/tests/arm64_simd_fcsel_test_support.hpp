// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_simd_fmov_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace arm64_simd_fcsel_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

[[nodiscard]] constexpr unsigned type_for_bytes(unsigned bytes)
{
	return bytes == 2U ? 3U : bytes == 4U ? 0U : 1U;
}

[[nodiscard]] constexpr std::uint32_t raw_instruction(unsigned type,
	unsigned condition, unsigned destination, unsigned first, unsigned second)
{
	return 0x1e20'0c00U | ((type & 3U) << 22) |
		((second & 31U) << 16) | ((condition & 15U) << 12) |
		((first & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t instruction(unsigned bytes,
	unsigned condition, unsigned destination, unsigned first, unsigned second)
{
	return raw_instruction(type_for_bytes(bytes), condition, destination,
		first, second);
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
	cpu.fpsr = 0x0800'0090U | (seed & 1U);
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	return cpu;
}

/*
 * Independent implementation of the architectural ConditionHolds()
 * predicate. Conditions 1110 and 1111 both select the first operand.
 */
[[nodiscard]] constexpr bool condition_holds(
	std::uint32_t nzcv, unsigned condition)
{
	const bool negative = (nzcv & 0x8000'0000U) != 0U;
	const bool zero = (nzcv & 0x4000'0000U) != 0U;
	const bool carry = (nzcv & 0x2000'0000U) != 0U;
	const bool overflow = (nzcv & 0x1000'0000U) != 0U;
	bool result;
	switch ((condition >> 1U) & 7U) {
	case 0U: result = zero; break;
	case 1U: result = carry; break;
	case 2U: result = negative; break;
	case 3U: result = overflow; break;
	case 4U: result = carry && !zero; break;
	case 5U: result = negative == overflow; break;
	case 6U: result = !zero && negative == overflow; break;
	default: result = true; break;
	}
	if ((condition & 1U) != 0U && (condition & 14U) != 14U)
		result = !result;
	return result;
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, unsigned bytes, unsigned condition,
	unsigned destination, unsigned first, unsigned second)
{
	auto expected = initial;
	const unsigned selected = condition_holds(initial.nzcv, condition) ?
		first : second;
	const auto source = initial.vectors[selected].bytes;
	expected.vectors[destination].bytes = {};
	std::copy_n(source.begin(), bytes,
		expected.vectors[destination].bytes.begin());
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements(
	unsigned bytes)
{
	return bytes == 2U ?
		engine_tests::NativeRequirements::features({
			uml::backend::ArmFeature::floating_point,
			uml::backend::ArmFeature::fp16,
		}) :
		engine_tests::NativeRequirements::features(
			uml::backend::ArmFeature::floating_point);
}

} // namespace arm64_simd_fcsel_test
