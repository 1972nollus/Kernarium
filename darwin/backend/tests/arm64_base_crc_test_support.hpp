// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace uml::backend::tests::base_crc {

namespace arm64 = uml::backend::arm64;

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

[[nodiscard]] constexpr std::uint32_t encode(
	unsigned operation, unsigned accumulator, unsigned source,
	unsigned destination, bool wrong_width = false) noexcept
{
	const bool wide = (operation & 3U) == 3U;
	return 0x1ac0'4000U | ((wide != wrong_width) ? 0x8000'0000U : 0U) |
		((operation & 7U) << 10) | ((source & 31U) << 16) |
		((accumulator & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr bool is_valid(std::uint32_t instruction) noexcept
{
	if ((instruction & 0x7fe0'e000U) != 0x1ac0'4000U)
		return false;
	const unsigned size = (instruction >> 10) & 3U;
	return ((instruction >> 31) != 0U) == (size == 3U);
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index) {
		const auto ordinal = static_cast<std::uint64_t>(index + 1U);
		cpu.registers[index] =
			(seed ^ (ordinal * 0x9e37'79b9'7f4a'7c15ULL)) +
			ordinal * 0x0102'0304'0506'0708ULL;
	}
	cpu.sp = seed ^ 0xfedc'ba98'7654'3210ULL;
	cpu.nzcv = static_cast<std::uint32_t>((seed >> 7) & 15U) << 28;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size();
		     ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 29U + byte * 17U);
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.tls = seed ^ 0x0123'4567'89ab'cdefULL;
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

[[nodiscard]] constexpr std::uint64_t reverse_bits(
	std::uint64_t value, unsigned width) noexcept
{
	std::uint64_t result = 0;
	for (unsigned bit = 0; bit < width; ++bit) {
		result = (result << 1) | (value & 1U);
		value >>= 1;
	}
	return result;
}

/*
 * Independent specification oracle. This follows the architectural
 * pseudocode literally rather than repeating the interpreter's reflected
 * byte recurrence:
 *
 *   reverse32(acc)::zeros(size) XOR reverse_size(value)::zeros(32)
 *
 * is divided as a polynomial over {0,1} by the canonical degree-32
 * polynomial, and the 32-bit remainder is reversed for Wd.
 */
[[nodiscard]] constexpr std::uint32_t oracle(
	std::uint32_t accumulator, std::uint64_t value, unsigned byte_count,
	bool alternate) noexcept
{
	using Wide = unsigned __int128;
	const unsigned value_bits = byte_count * 8U;
	const std::uint32_t polynomial =
		alternate ? 0x1edc'6f41U : 0x04c1'1db7U;
	Wide dividend =
		(static_cast<Wide>(reverse_bits(accumulator, 32U)) << value_bits) ^
		(static_cast<Wide>(reverse_bits(value, value_bits)) << 32U);
	const Wide divisor = (Wide{1} << 32U) | polynomial;
	for (unsigned degree = 32U + value_bits; degree-- > 32U;)
		if (((dividend >> degree) & 1U) != 0U)
			dividend ^= divisor << (degree - 32U);
	return static_cast<std::uint32_t>(
		reverse_bits(static_cast<std::uint32_t>(dividend), 32U));
}

static_assert(oracle(0U, 1U, 1U, false) == 0x7707'3096U);
static_assert(oracle(0x1234'5678U, 0x0123'4567'89ab'cdefULL,
	8U, false) == 0x9b62'eadfU);
static_assert(oracle(0U, 1U, 1U, true) == 0xf26b'8303U);
static_assert(oracle(0x89ab'cdefU, 0xffff'ffff'ffff'ffffULL,
	8U, true) == 0x0da9'1e67U);

[[nodiscard]] inline arm64::CpuState expected_state(
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	if (!is_valid(instruction))
		fail("checksum oracle received a reserved encoding");
	const unsigned operation = (instruction >> 10) & 7U;
	const unsigned accumulator = (instruction >> 5) & 31U;
	const unsigned source = (instruction >> 16) & 31U;
	const unsigned destination = instruction & 31U;
	const auto result = oracle(
		static_cast<std::uint32_t>(
			register_value(initial, accumulator)),
		register_value(initial, source), 1U << (operation & 3U),
		operation >= 4U);
	auto expected = initial;
	if (destination != 31U)
		expected.registers[destination] = result;
	return expected;
}

inline void check(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	require(is_valid(instruction),
		"valid checksum test constructed a reserved encoding");
	harness.check_one(name, instruction, initial,
		expected_state(instruction, initial),
		NativeRequirements::features(ArmFeature::crc32));
}

inline void check_illegal(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	require(!is_valid(instruction),
		"reserved checksum test constructed an allocated encoding");
	harness.check_illegal(name, instruction, initial,
		NativeRequirements::features(ArmFeature::crc32));
}

inline void require_engine_counts(
	const EngineHarness &harness, std::size_t cases,
	bool invalid_encoding = false)
{
	const auto &counts = harness.counts();
	require(counts.interpreter == cases,
		"checksum interpreter did not run every case");
	require(counts.gadgets == cases,
		"checksum gadgets did not run every case");
	require(counts.jit + counts.jit_skipped == cases,
		"checksum JIT neither ran nor feature-skipped every case");
	require(counts.jit_unavailable == 0U,
		"checksum JIT was unexpectedly unavailable");
	if (invalid_encoding) {
		require(counts.jit == cases &&
				counts.jit_skipped_host_feature == 0U,
			"reserved checksum JIT did not validate every encoding");
		return;
	}
	const auto native =
		NativeRequirements::features(ArmFeature::crc32);
	if (host_supports(native)) {
		require(counts.jit == cases &&
				counts.jit_skipped_host_feature == 0U,
			"supported checksum JIT did not run every case");
	} else {
		require(counts.jit == 0U &&
				counts.jit_skipped_host_feature == cases,
			"unsupported checksum JIT did not feature-skip exactly");
	}
}

} // namespace uml::backend::tests::base_crc
