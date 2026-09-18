// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

namespace arm64_simd_pmull_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

struct Variant final {
	bool high;
	unsigned size;
	Registers registers;
};

inline constexpr std::array alias_register_cases{
	Registers{0, 1, 2},
	Registers{1, 1, 2},
	Registers{2, 1, 2},
	Registers{0, 1, 1},
	Registers{1, 1, 1},
	Registers{31, 30, 29},
	Registers{31, 31, 30},
	Registers{31, 30, 31},
	Registers{31, 30, 30},
	Registers{31, 31, 31},
};

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

/*
 * Direct transcription of the PMULL/PMULL2 encoding diagram. This is test
 * code only and deliberately does not call the production decoder.
 */
[[nodiscard]] constexpr std::uint32_t instruction(
	const Variant &variant) noexcept
{
	return 0x0e20'e000U |
		(variant.high ? 0x4000'0000U : 0U) |
		((variant.size & 3U) << 22) |
		((variant.registers.right & 31U) << 16) |
		((variant.registers.left & 31U) << 5) |
		(variant.registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint16_t carryless_byte(
	unsigned left, unsigned right) noexcept
{
	std::uint16_t result = 0;
	for (unsigned right_bit = 0; right_bit < 8U; ++right_bit)
		if ((right & (1U << right_bit)) != 0)
			result ^= static_cast<std::uint16_t>(
				(left & 0xffU) << right_bit);
	return result;
}

using WideUnsigned = unsigned __int128;

[[nodiscard]] constexpr WideUnsigned carryless_doubleword(
	std::uint64_t left, std::uint64_t right) noexcept
{
	WideUnsigned result = 0;
	for (unsigned right_bit = 0; right_bit < 64U; ++right_bit)
		if ((right & (std::uint64_t{1} << right_bit)) != 0)
			result ^= static_cast<WideUnsigned>(left) << right_bit;
	return result;
}

static_assert(carryless_byte(0x57U, 0x83U) == 0x2b79U);
static_assert(carryless_byte(0xffU, 0xffU) == 0x5555U);
static_assert(carryless_doubleword(~std::uint64_t{}, 1U) ==
	static_cast<WideUnsigned>(~std::uint64_t{}));
static_assert(carryless_doubleword(
	std::uint64_t{1} << 63U, std::uint64_t{1} << 63U) ==
	(static_cast<WideUnsigned>(1) << 126U));

} // namespace arm64_simd_pmull_test
