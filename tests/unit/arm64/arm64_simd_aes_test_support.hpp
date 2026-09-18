// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace arm64_simd_aes_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	encrypt_round = 4,
	decrypt_round = 5,
	mix_columns = 6,
	inverse_mix_columns = 7,
};

struct Registers final {
	unsigned destination;
	unsigned source;
};

struct Variant final {
	Operation operation;
	Registers registers;
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
 * Direct transcription of the four encoding diagrams. The operation field
 * occupies bits 14:12; values 0..3 are reserved by this allocation.
 */
[[nodiscard]] constexpr std::uint32_t instruction(
	const Variant &variant) noexcept
{
	return 0x4e28'0800U |
		((static_cast<unsigned>(variant.operation) & 7U) << 12) |
		((variant.registers.source & 31U) << 5) |
		(variant.registers.destination & 31U);
}

} // namespace arm64_simd_aes_test
