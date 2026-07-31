// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_simd_estimate_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

struct Registers final {
	unsigned destination;
	unsigned source;
};

[[nodiscard]] constexpr std::uint32_t instruction(bool q,
	bool reciprocal_sqrt, bool reserved_size, Registers registers)
{
	return 0x0ea1'c800U | (q ? 0x4000'0000U : 0U) |
		(reciprocal_sqrt ? 0x2000'0000U : 0U) |
		(reserved_size ? 0x0040'0000U : 0U) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::string_view operation_name(bool reciprocal_sqrt)
{
	return reciprocal_sqrt ? "unsigned-root-estimate" :
		"unsigned-inverse-estimate";
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

[[nodiscard]] inline std::uint32_t read_word(
	const std::array<std::byte, 16> &bytes, unsigned lane)
{
	std::uint32_t value = 0;
	for (unsigned byte = 0; byte < 4; ++byte)
		value |= std::to_integer<std::uint32_t>(
			bytes[lane * 4U + byte]) << (byte * 8U);
	return value;
}

inline void write_word(std::array<std::byte, 16> &bytes, unsigned lane,
	std::uint32_t value)
{
	for (unsigned byte = 0; byte < 4; ++byte)
		bytes[lane * 4U + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

/*
 * Independent transcription of UnsignedRecipEstimate{32}. Only the most
 * significant nine input bits participate in the specified fixed-point
 * calculation; integer division implements the pseudocode's DIVRM operation
 * because every operand is nonnegative.
 */
[[nodiscard]] constexpr std::uint32_t inverse_estimate(std::uint32_t operand)
{
	if ((operand & 0x8000'0000U) == 0)
		return 0xffff'ffffU;
	const auto rounded_input = ((operand >> 23) * 2U) + 1U;
	const auto quotient = (std::uint32_t{1} << 19) / rounded_input;
	return ((quotient + 1U) / 2U) << 23;
}

/*
 * Independent transcription of UnsignedRSqrtEstimate{32}. The two input
 * ranges use the distinct normalization and rounding steps in the shared
 * pseudocode before the bounded estimate search.
 */
[[nodiscard]] constexpr std::uint32_t root_estimate(std::uint32_t operand)
{
	if ((operand & 0xc000'0000U) == 0)
		return 0xffff'ffffU;
	auto rounded_input = operand >> 23;
	if (rounded_input < 256U)
		rounded_input = rounded_input * 2U + 1U;
	else
		rounded_input = ((rounded_input >> 1) * 2U + 1U) * 2U;
	auto estimate = 512U;
	while (static_cast<std::uint64_t>(rounded_input) *
		       (estimate + 1U) * (estimate + 1U) <
	       (std::uint64_t{1} << 28))
		++estimate;
	return ((estimate + 1U) / 2U) << 23;
}

static_assert(inverse_estimate(0x7fff'ffffU) == 0xffff'ffffU);
static_assert(inverse_estimate(0x8000'0000U) == 0xff80'0000U);
static_assert(inverse_estimate(0xbfff'ffffU) == 0xab00'0000U);
static_assert(inverse_estimate(0xffff'ffffU) == 0x8000'0000U);
static_assert(root_estimate(0x3fff'ffffU) == 0xffff'ffffU);
static_assert(root_estimate(0x4000'0000U) == 0xff80'0000U);
static_assert(root_estimate(0x7fff'ffffU) == 0xb500'0000U);
static_assert(root_estimate(0x8000'0000U) == 0xb480'0000U);
static_assert(root_estimate(0xffff'ffffU) == 0x8000'0000U);

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, bool q, bool reciprocal_sqrt,
	Registers registers)
{
	const auto source = initial.vectors[registers.source].bytes;
	std::array<std::byte, 16> result{};
	const unsigned lanes = q ? 4U : 2U;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto input = read_word(source, lane);
		write_word(result, lane, reciprocal_sqrt ?
			root_estimate(input) : inverse_estimate(input));
	}
	auto expected = initial;
	expected.vectors[registers.destination].bytes = result;
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		uml::backend::ArmFeature::advanced_simd);
}

} // namespace arm64_simd_estimate_test
