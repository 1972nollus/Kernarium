// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_base_bitfield_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

[[nodiscard]] constexpr std::uint64_t width_mask(bool wide)
{
	return wide ? ~std::uint64_t{0} : std::uint64_t{0xffff'ffffU};
}

[[nodiscard]] constexpr std::uint64_t low_ones(unsigned count)
{
	return count == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << count) - 1U;
}

/*
 * Test-only transcription of the three bitfield operations. It deliberately
 * does not use DecodeBitMasks() or any production rotate/mask helper.
 */
[[nodiscard]] constexpr std::uint64_t result(bool wide, unsigned operation,
	unsigned immr, unsigned imms, std::uint64_t source,
	std::uint64_t destination)
{
	const unsigned width = wide ? 64U : 32U;
	const auto mask = width_mask(wide);
	source &= mask;
	destination &= mask;

	unsigned count;
	unsigned position;
	std::uint64_t field;
	if (imms >= immr) {
		count = imms - immr + 1U;
		position = 0U;
		field = (source >> immr) & low_ones(count);
	} else {
		count = imms + 1U;
		position = width - immr;
		field = (source & low_ones(count)) << position;
	}
	if (operation == 1U) {
		const auto field_mask = low_ones(count) << position;
		return ((destination & ~field_mask) | field) & mask;
	}
	if (operation == 2U)
		return field & mask;
	const bool sign = ((source >> imms) & 1U) != 0U;
	const unsigned top = position + count;
	if (sign && top < width)
		field |= mask & ~low_ones(top);
	return field & mask;
}

[[nodiscard]] constexpr std::uint32_t instruction(bool wide,
	unsigned operation, unsigned immr, unsigned imms, unsigned source = 1U,
	unsigned destination = 7U, unsigned n_field = 2U)
{
	const unsigned n = n_field < 2U ? n_field :
		static_cast<unsigned>(wide);
	return 0x1300'0000U | (wide ? 0x8000'0000U : 0U) |
		((operation & 3U) << 29) | ((n & 1U) << 22) |
		((immr & 63U) << 16) | ((imms & 63U) << 10) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < 31U; ++index)
		cpu.registers[index] =
			0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(seed + index * 17U) *
				0x0101'0101'0101'0101ULL);
	for (unsigned reg = 0; reg < 32U; ++reg)
		for (unsigned byte = 0; byte < 16U; ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 19U + byte * 23U);
	cpu.sp = 0xfedc'ba98'7654'3210ULL ^ seed;
	cpu.nzcv = ((seed ^ 0xbU) & 15U) << 28;
	cpu.fpsr = 0x0800'0090U;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0x3141'5926'5358'9793ULL;
	cpu.exclusive_address = 0x5100'0040ULL;
	cpu.exclusive_key = 0x2468U;
	cpu.exclusive_value = 0x1122'3344'5566'7788ULL;
	cpu.exclusive_value2 = 0x8877'6655'4433'2211ULL;
	cpu.exclusive_version = 0x1020'3040'5060'7080ULL;
	cpu.exclusive_size = 8U;
	return cpu;
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, bool wide, unsigned operation,
	unsigned immr, unsigned imms, unsigned source, unsigned destination)
{
	auto expected = initial;
	const auto source_value =
		source == 31U ? 0U : initial.registers[source];
	const auto destination_value =
		destination == 31U ? 0U : initial.registers[destination];
	if (destination != 31U)
		expected.registers[destination] = result(wide, operation, immr,
			imms, source_value, destination_value);
	return expected;
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, bool wide, unsigned operation, unsigned immr,
	unsigned imms, unsigned source, unsigned destination,
	const arm64::CpuState &initial,
	engine_tests::NativeRequirements native =
		engine_tests::NativeRequirements::baseline())
{
	harness.check_one(name, instruction(wide, operation, immr, imms,
		source, destination), initial,
		expected_state(initial, wide, operation, immr, imms, source,
			destination),
		native);
}

} // namespace arm64_base_bitfield_test
