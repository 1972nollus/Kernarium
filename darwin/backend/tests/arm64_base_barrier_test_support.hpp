// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace uml::backend::tests::base_barrier {

namespace arm64 = uml::backend::arm64;

inline constexpr std::uint32_t dsb_base = 0xd503'309fU;
inline constexpr std::uint32_t dmb_base = 0xd503'30bfU;
inline constexpr std::uint32_t isb_base = 0xd503'30dfU;
inline constexpr std::uint32_t dsb_nxs_base = 0xd503'323fU;
inline constexpr std::uint32_t sb_base = 0xd503'30ffU;
inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

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

[[nodiscard]] constexpr std::uint32_t encode_standard(
	std::uint32_t base, unsigned option) noexcept
{
	return base | ((option & 15U) << 8U);
}

[[nodiscard]] constexpr std::uint32_t encode_nxs(unsigned scope) noexcept
{
	return dsb_nxs_base | ((scope & 3U) << 10U);
}

[[nodiscard]] constexpr bool is_standard(
	std::uint32_t instruction) noexcept
{
	const auto fixed = instruction & 0xffff'f0ffU;
	return fixed == dsb_base || fixed == dmb_base || fixed == isb_base;
}

[[nodiscard]] constexpr bool is_nxs(std::uint32_t instruction) noexcept
{
	return (instruction & 0xffff'f3ffU) == dsb_nxs_base;
}

[[nodiscard]] inline NativeRequirements native_requirements(
	std::uint32_t instruction) noexcept
{
	return is_nxs(instruction) ?
		NativeRequirements::features(ArmFeature::xs) :
		NativeRequirements::baseline();
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
	cpu.nzcv = static_cast<std::uint32_t>((seed >> 5U) & 15U) << 28U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.fpmr = seed ^ 0x0f1e'2d3c'4b5a'6978ULL;
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
	return cpu;
}

inline void check(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	require(is_standard(instruction) || is_nxs(instruction),
		"barrier oracle received an unallocated encoding");
	harness.check_one(name, instruction, initial, initial,
		native_requirements(instruction));
}

} // namespace uml::backend::tests::base_barrier
