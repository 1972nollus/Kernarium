// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

namespace arm64_simd_sha3_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	three_way_exclusive,
	clear_and_exclusive,
	rotate_one_and_exclusive,
	exclusive_and_rotate,
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
	unsigned third;
};

struct Variant final {
	Operation operation;
	Registers registers;
	unsigned rotate{};
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

[[nodiscard]] constexpr bool four_sources(Operation operation) noexcept
{
	return operation == Operation::three_way_exclusive ||
		operation == Operation::clear_and_exclusive;
}

/*
 * Direct transcription of the four encoding diagrams. The oracle below does
 * not include or call production decode or execution helpers.
 */
[[nodiscard]] constexpr std::uint32_t instruction(
	const Variant &variant) noexcept
{
	const auto &registers = variant.registers;
	std::uint32_t result;
	switch (variant.operation) {
	case Operation::three_way_exclusive:
		result = 0xce00'0000U |
			((registers.third & 31U) << 10);
		break;
	case Operation::clear_and_exclusive:
		result = 0xce20'0000U |
			((registers.third & 31U) << 10);
		break;
	case Operation::rotate_one_and_exclusive:
		result = 0xce60'8c00U;
		break;
	case Operation::exclusive_and_rotate:
		result = 0xce80'0000U |
			((variant.rotate & 63U) << 10);
		break;
	}
	return result | ((registers.second & 31U) << 16) |
		((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

} // namespace arm64_simd_sha3_test
