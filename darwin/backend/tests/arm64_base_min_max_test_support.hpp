// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace uml::backend::tests::base_min_max {

namespace arm64 = uml::backend::arm64;

enum class Operation : unsigned {
	signed_maximum,
	unsigned_maximum,
	signed_minimum,
	unsigned_minimum,
};

struct Description final {
	Operation operation;
	std::string_view immediate_id;
	std::string_view register_id;
};

inline constexpr std::array descriptions{
	Description{Operation::signed_maximum, "BASE-0376 SMAX (immediate)",
		"BASE-0377 SMAX (register)"},
	Description{Operation::unsigned_maximum, "BASE-0500 UMAX (immediate)",
		"BASE-0501 UMAX (register)"},
	Description{Operation::signed_minimum, "BASE-0379 SMIN (immediate)",
		"BASE-0380 SMIN (register)"},
	Description{Operation::unsigned_minimum, "BASE-0502 UMIN (immediate)",
		"BASE-0503 UMIN (register)"},
};

[[nodiscard]] inline NativeRequirements native()
{
	return NativeRequirements::features(ArmFeature::cssc);
}

[[nodiscard]] inline arm64::CpuState state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			seed ^ (0x91a2'b3c4'd5e6'f708ULL +
				index * 0x0102'0408'1020'4081ULL);
	cpu.sp = seed ^ 0x1234'5678'9abc'def0ULL;
	cpu.nzcv = static_cast<std::uint32_t>(seed) & 0xf000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.tls = seed ^ 0xfedc'ba98'7654'3210ULL;
	for (unsigned index = 0; index < cpu.vectors.size(); ++index)
		for (unsigned byte = 0;
		     byte < cpu.vectors[index].bytes.size(); ++byte)
			cpu.vectors[index].bytes[byte] =
				static_cast<std::byte>(
					seed + index * 13U + byte);
	return cpu;
}

[[nodiscard]] constexpr std::uint32_t encode_immediate(
	bool wide, Operation operation, unsigned immediate, unsigned rd,
	unsigned rn)
{
	return 0x11c0'0000U | (wide ? 0x8000'0000U : 0U) |
		(static_cast<unsigned>(operation) << 18U) |
		((immediate & 0xffU) << 10U) | ((rn & 31U) << 5U) |
		(rd & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_register(
	bool wide, Operation operation, unsigned rd, unsigned rn, unsigned rm)
{
	return 0x1ac0'6000U | (wide ? 0x8000'0000U : 0U) |
		((rm & 31U) << 16U) |
		(static_cast<unsigned>(operation) << 10U) |
		((rn & 31U) << 5U) | (rd & 31U);
}

[[nodiscard]] constexpr std::uint64_t read_zero_register(
	const arm64::CpuState &cpu, unsigned index)
{
	return index == 31U ? 0U : cpu.registers[index];
}

[[nodiscard]] constexpr std::int64_t signed_value(
	std::uint64_t value, bool wide)
{
	if (wide)
		return std::bit_cast<std::int64_t>(value);
	return std::bit_cast<std::int32_t>(
		static_cast<std::uint32_t>(value));
}

[[nodiscard]] constexpr std::uint64_t result(
	std::uint64_t left_bits, std::uint64_t right_bits, bool wide,
	Operation operation)
{
	const auto mask =
		wide ? ~std::uint64_t{0} : std::uint64_t{0xffff'ffffU};
	left_bits &= mask;
	right_bits &= mask;
	bool take_left;
	switch (operation) {
	case Operation::signed_maximum:
		take_left = signed_value(left_bits, wide) >=
			signed_value(right_bits, wide);
		break;
	case Operation::unsigned_maximum:
		take_left = left_bits >= right_bits;
		break;
	case Operation::signed_minimum:
		take_left = signed_value(left_bits, wide) <=
			signed_value(right_bits, wide);
		break;
	case Operation::unsigned_minimum:
		take_left = left_bits <= right_bits;
		break;
	}
	return (take_left ? left_bits : right_bits) & mask;
}

[[nodiscard]] constexpr std::uint64_t immediate_bits(
	Operation operation, unsigned encoded, bool wide)
{
	const bool signed_operation =
		operation == Operation::signed_maximum ||
		operation == Operation::signed_minimum;
	std::uint64_t value = encoded & 0xffU;
	if (signed_operation && (value & 0x80U) != 0)
		value |= ~std::uint64_t{0xffU};
	return wide ? value : value & 0xffff'ffffU;
}

[[nodiscard]] inline arm64::CpuState expected(
	const arm64::CpuState &initial, std::uint32_t instruction,
	bool immediate)
{
	const bool wide = (instruction & 0x8000'0000U) != 0;
	const auto operation = static_cast<Operation>(immediate ?
		(instruction >> 18U) & 3U :
		(instruction >> 10U) & 3U);
	const unsigned rd = instruction & 31U;
	const unsigned rn = (instruction >> 5U) & 31U;
	const auto left = read_zero_register(initial, rn);
	const auto right = immediate ?
		immediate_bits(operation, (instruction >> 10U) & 0xffU, wide) :
		read_zero_register(initial, (instruction >> 16U) & 31U);
	auto output = initial;
	if (rd != 31U)
		output.registers[rd] =
			result(left, right, wide, operation);
	return output;
}

inline void check(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, bool immediate,
	const arm64::CpuState &initial)
{
	harness.check_one(name, instruction, initial,
		expected(initial, instruction, immediate), native());
}

inline void require(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "%s\n", message);
		std::abort();
	}
}

inline void require_counts(const EngineHarness &harness, std::size_t cases,
	const char *suite)
{
	const auto &counts = harness.counts();
	require(counts.interpreter == cases && counts.gadgets == cases,
		"min/max case was not executed by both software engines");
	require(counts.jit_unavailable == 0,
		"native JIT unexpectedly unavailable in min/max suite");
	if (host_supports(native())) {
		require(counts.jit == cases &&
				counts.jit_skipped_host_feature == 0,
			"host-supported min/max case did not execute in JIT");
	} else {
		require(counts.jit == 0 &&
				counts.jit_skipped_host_feature == cases,
			"host-unsupported min/max case was not feature-skipped");
	}
	harness.print_summary(suite);
}

} // namespace uml::backend::tests::base_min_max
