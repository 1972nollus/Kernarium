// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace arm64_simd_immediate_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

struct Decoded final {
	std::uint64_t pattern;
	unsigned action;
};

[[nodiscard]] constexpr std::uint32_t instruction(bool q, bool op,
	unsigned mode, bool o2, unsigned immediate, unsigned destination = 0)
{
	return 0x0f00'0400U | (q ? 0x4000'0000U : 0U) |
		(op ? 0x2000'0000U : 0U) | ((immediate & 0xe0U) << 11) |
		((mode & 0xfU) << 12) | (o2 ? 0x800U : 0U) |
		((immediate & 0x1fU) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr bool valid(bool q, bool op, unsigned mode, bool o2)
{
	if (o2)
		return !op && mode == 15U;
	return !op || mode != 15U || q;
}

[[nodiscard]] constexpr std::uint64_t replicate_32(std::uint32_t value)
{
	return value | (static_cast<std::uint64_t>(value) << 32);
}

/*
 * Independent architectural expansion model. Keep this separate from the
 * production decoder so all three engines are checked against the specified
 * bit-pattern construction rather than against one another.
 */
[[nodiscard]] constexpr Decoded decode(bool op, unsigned mode, bool o2,
	unsigned immediate)
{
	if (o2) {
		const std::uint64_t sign = immediate >> 7;
		const std::uint64_t exponent_bit = (immediate >> 6) & 1U;
		const std::uint64_t value = (sign << 15) |
			((1U - exponent_bit) << 14) |
			(exponent_bit * 0x3000U) |
			(static_cast<std::uint64_t>(immediate & 0x3fU) << 6);
		return {value * 0x0001'0001'0001'0001ULL, 0U};
	}

	const bool combine = (mode & 1U) != 0 && mode != 13U && mode != 15U;
	std::uint64_t pattern = 0;
	if (mode <= 7U) {
		const unsigned base = combine ? mode - 1U : mode;
		const auto element = static_cast<std::uint64_t>(immediate) <<
			((base >> 1) * 8);
		pattern = replicate_32(static_cast<std::uint32_t>(element));
	} else if (mode <= 11U) {
		const unsigned base = combine ? mode - 1U : mode;
		const auto element = static_cast<std::uint64_t>(immediate) <<
			(((base >> 1) & 1U) * 8);
		pattern = element | (element << 16) | (element << 32) |
			(element << 48);
	} else if (mode == 12U) {
		const auto element =
			(static_cast<std::uint64_t>(immediate) << 8) | 0xffU;
		pattern = element | (element << 32);
	} else if (mode == 13U) {
		const auto element =
			(static_cast<std::uint64_t>(immediate) << 16) | 0xffffU;
		pattern = element | (element << 32);
	} else if (mode == 14U && !op) {
		pattern = static_cast<std::uint64_t>(immediate) *
			0x0101'0101'0101'0101ULL;
	} else if (mode == 14U) {
		for (unsigned index = 0; index < 8; ++index)
			if ((immediate & (1U << index)) != 0)
				pattern |= 0xffULL << (index * 8);
	} else if (!op) {
		const std::uint32_t sign = immediate >> 7;
		const std::uint32_t exponent_bit = (immediate >> 6) & 1U;
		const std::uint32_t fraction = immediate & 0x3fU;
		const std::uint32_t exponent = ((1U - exponent_bit) << 7) |
			(exponent_bit * 0x7cU) | ((fraction >> 4) & 3U);
		const std::uint32_t value = (sign << 31) | (exponent << 23) |
			((fraction & 15U) << 19);
		pattern = replicate_32(value);
	} else {
		const std::uint64_t sign = immediate >> 7;
		const std::uint64_t exponent_bit = (immediate >> 6) & 1U;
		const std::uint64_t fraction = immediate & 0x3fU;
		const std::uint64_t exponent = ((1U - exponent_bit) << 10) |
			(exponent_bit * 0x3fcU) | ((fraction >> 4) & 3U);
		pattern = (sign << 63) | (exponent << 52) |
			((fraction & 15U) << 48);
	}
	if (!combine && op && mode != 14U && mode != 15U)
		pattern = ~pattern;
	return {pattern, combine ? (op ? 2U : 1U) : 0U};
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

[[nodiscard]] inline std::uint64_t vector_half(
	const arm64::VectorRegister &vector, unsigned index)
{
	std::uint64_t value;
	std::memcpy(&value, vector.bytes.data() + index * sizeof(value),
		sizeof(value));
	return value;
}

inline void set_vector_half(
	arm64::VectorRegister &vector, unsigned index, std::uint64_t value)
{
	std::memcpy(vector.bytes.data() + index * sizeof(value), &value,
		sizeof(value));
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, bool q, bool op, unsigned mode, bool o2,
	unsigned immediate, unsigned destination)
{
	const auto expanded = decode(op, mode, o2, immediate);
	const auto old_low = vector_half(initial.vectors[destination], 0);
	const auto old_high = vector_half(initial.vectors[destination], 1);
	const auto apply = [&](std::uint64_t old) {
		if (expanded.action == 1U)
			return old | expanded.pattern;
		if (expanded.action == 2U)
			return old & ~expanded.pattern;
		return expanded.pattern;
	};
	auto expected = initial;
	set_vector_half(expected.vectors[destination], 0, apply(old_low));
	set_vector_half(
		expected.vectors[destination], 1, q ? apply(old_high) : 0U);
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(bool o2)
{
	auto requirements = engine_tests::NativeRequirements::features(
		uml::backend::ArmFeature::advanced_simd);
	if (o2)
		requirements = requirements | uml::backend::ArmFeature::fp16;
	return requirements;
}

} // namespace arm64_simd_immediate_test
