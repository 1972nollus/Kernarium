// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace uml::emulator::tests::base_flag {

namespace arm64 = uml::cpu::arm64;

inline constexpr std::uint32_t cfinv_encoding = 0xd500'401fU;
inline constexpr std::uint32_t xaflag_encoding = 0xd500'403fU;
inline constexpr std::uint32_t axflag_encoding = 0xd500'405fU;

[[noreturn]] inline void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

inline void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			(seed ^ (0x9e37'79b9'7f4a'7c15ULL *
				static_cast<std::uint64_t>(index + 1U))) +
			0x0102'0304'0506'0708ULL * index;
	cpu.sp = seed ^ 0xfedc'ba98'7654'3210ULL;
	cpu.nzcv = static_cast<std::uint32_t>((seed >> 5) & 15U) << 28U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.tls = seed ^ 0x0123'4567'89ab'cdefULL;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size();
		     ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 29U + byte * 17U);
	cpu.set_mte_configuration(true, 0x5a5aU);
	cpu.set_mte_generator(true, 0x1357U, 9U,
		seed ^ 0x6a09'e667'f3bc'c909ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = static_cast<std::uint32_t>(seed);
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_sm = true;
	cpu.pstate_za = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xbU;
	cpu.exclusive_address = seed ^ 0x8877'6655'4433'2210ULL;
	cpu.exclusive_key = static_cast<std::uintptr_t>(
		seed ^ 0x1020'3040'5060'7080ULL);
	cpu.exclusive_value = seed ^ 0x1111'2222'3333'4444ULL;
	cpu.exclusive_value2 = seed ^ 0x5555'6666'7777'8888ULL;
	cpu.exclusive_version = seed ^ 0x9999'aaaa'bbbb'ccccULL;
	cpu.exclusive_size = 8;
	cpu.fpmr = seed ^ 0x0f1e'2d3c'4b5a'6978ULL;
	return cpu;
}

[[nodiscard]] constexpr std::uint64_t register_value(
	const arm64::CpuState &cpu, unsigned index) noexcept
{
	return index == 31U ? 0U : cpu.registers[index];
}

[[nodiscard]] constexpr std::uint32_t encode_rmif(
	unsigned source, unsigned rotation, unsigned mask) noexcept
{
	return 0xba00'0400U | ((rotation & 63U) << 15U) |
		((source & 31U) << 5U) | (mask & 15U);
}

[[nodiscard]] constexpr std::uint32_t encode_setf(
	bool sixteen, unsigned source) noexcept
{
	return 0x3a00'080dU | (sixteen ? 0x4000U : 0U) |
		((source & 31U) << 5U);
}

[[nodiscard]] constexpr bool is_rmif(std::uint32_t instruction) noexcept
{
	return (instruction & 0xffe0'7c10U) == 0xba00'0400U;
}

[[nodiscard]] constexpr bool is_setf(std::uint32_t instruction) noexcept
{
	return (instruction & 0xffff'bc1fU) == 0x3a00'080dU;
}

[[nodiscard]] constexpr bool is_fixed(std::uint32_t instruction) noexcept
{
	return instruction == cfinv_encoding ||
		instruction == xaflag_encoding ||
		instruction == axflag_encoding;
}

[[nodiscard]] inline NativeRequirements native_requirements(
	std::uint32_t instruction) noexcept
{
	return NativeRequirements::features(
		instruction == xaflag_encoding || instruction == axflag_encoding ?
			ArmFeature::flag_m2 : ArmFeature::flag_m);
}

[[nodiscard]] constexpr std::uint32_t fixed_oracle(
	std::uint32_t instruction, std::uint32_t nzcv) noexcept
{
	const bool z = (nzcv & 0x4000'0000U) != 0;
	const bool c = (nzcv & 0x2000'0000U) != 0;
	const bool v = (nzcv & 0x1000'0000U) != 0;
	if (instruction == cfinv_encoding)
		return nzcv ^ 0x2000'0000U;
	if (instruction == axflag_encoding)
		return ((z || v) ? 0x4000'0000U : 0U) |
			((c && !v) ? 0x2000'0000U : 0U);
	return ((!c && !z) ? 0x8000'0000U : 0U) |
		((z && c) ? 0x4000'0000U : 0U) |
		((c || z) ? 0x2000'0000U : 0U) |
		((!c && z) ? 0x1000'0000U : 0U);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	auto expected = initial;
	if (is_fixed(instruction)) {
		expected.nzcv = fixed_oracle(instruction, initial.nzcv);
		return expected;
	}
	if (is_rmif(instruction)) {
		const unsigned source = (instruction >> 5U) & 31U;
		const unsigned rotation = (instruction >> 15U) & 63U;
		const std::uint32_t mask = (instruction & 15U) << 28U;
		const auto low = static_cast<std::uint32_t>(
			std::rotr(register_value(initial, source), rotation) & 15U);
		expected.nzcv =
			(initial.nzcv & ~mask) | ((low << 28U) & mask);
		return expected;
	}
	if (is_setf(instruction)) {
		const unsigned width = (instruction & 0x4000U) != 0 ? 16U : 8U;
		const unsigned source = (instruction >> 5U) & 31U;
		const auto value = static_cast<std::uint32_t>(
			register_value(initial, source));
		const bool n = ((value >> (width - 1U)) & 1U) != 0;
		const bool z = (value & ((1U << width) - 1U)) == 0;
		const bool v =
			(((value >> width) ^ (value >> (width - 1U))) & 1U) != 0;
		expected.nzcv = (initial.nzcv & 0x2000'0000U) |
			(n ? 0x8000'0000U : 0U) |
			(z ? 0x4000'0000U : 0U) |
			(v ? 0x1000'0000U : 0U);
		return expected;
	}
	fail("flag oracle received an unallocated encoding");
}

inline void check(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	require(is_fixed(instruction) || is_rmif(instruction) ||
			is_setf(instruction),
		"allocated flag test constructed an unallocated encoding");
	harness.check_one(name, instruction, initial,
		expected_state(instruction, initial),
		native_requirements(instruction));
}

} // namespace uml::emulator::tests::base_flag
