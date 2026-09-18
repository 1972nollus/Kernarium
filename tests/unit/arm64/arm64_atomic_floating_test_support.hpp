// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_atomic_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

namespace arm64_atomic_floating_test {

namespace arm64 = uml::cpu::arm64;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

enum class Operation : unsigned {
	add,
	maximum,
	maximum_number,
	minimum,
	minimum_number,
};

inline constexpr std::array operations{
	Operation::add,
	Operation::maximum,
	Operation::maximum_number,
	Operation::minimum,
	Operation::minimum_number,
};

struct Width final {
	unsigned size_code;
	std::size_t bytes;
	std::uint64_t one;
	std::uint64_t two;
	std::uint64_t three;
	std::string_view name;
};

inline constexpr std::array widths{
	Width{0, 2, 0x3f80, 0x4000, 0x4040, "bfloat16"},
	Width{1, 2, 0x3c00, 0x4000, 0x4200, "binary16"},
	Width{2, 4, 0x3f80'0000, 0x4000'0000, 0x4040'0000, "binary32"},
	Width{3, 8, 0x3ff0'0000'0000'0000,
		0x4000'0000'0000'0000, 0x4008'0000'0000'0000, "binary64"},
};

[[noreturn]] inline void fail(std::string_view message)
{
	std::fprintf(stderr, "atomic floating-memory test failure: %.*s\n",
		static_cast<int>(message.size()), message.data());
	std::abort();
}

inline void require(bool condition, std::string_view message)
{
	if (!condition)
		fail(message);
}

[[nodiscard]] constexpr unsigned opcode(Operation operation) noexcept
{
	switch (operation) {
	case Operation::add: return 0U;
	case Operation::maximum: return 4U;
	case Operation::maximum_number: return 6U;
	case Operation::minimum: return 5U;
	case Operation::minimum_number: return 7U;
	}
	return 0U;
}

[[nodiscard]] constexpr std::uint32_t raw(
	unsigned size_code, bool acquire, bool release, bool store_only,
	unsigned encoded_opcode, unsigned rn, unsigned rs, unsigned rt) noexcept
{
	/* Independent transcription of the FEAT_LSFE scalar atomic diagram:
	 * VR=1, o3 selects the store-only form, and opc selects the arithmetic
	 * operation. */
	return 0x3c20'0000U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) | ((rs & 31U) << 16) |
		(static_cast<unsigned>(store_only) << 15) |
		((encoded_opcode & 7U) << 12) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t instruction(
	unsigned size_code, Operation operation, bool acquire, bool release,
	bool store_only, unsigned rn, unsigned rs, unsigned rt = 32U) noexcept
{
	return raw(size_code, acquire, release, store_only, opcode(operation),
		rn, rs, rt == 32U ? (store_only ? 31U : 4U) : rt);
}

static_assert(instruction(
	0, Operation::add, false, false, false, 2, 3, 4) == 0x3c23'0044U);
static_assert(instruction(
	2, Operation::maximum, true, true, false, 2, 3, 4) == 0xbce3'4044U);
static_assert(instruction(
	3, Operation::minimum_number, false, true, true, 2, 3) ==
	0xfc63'f05fU);

[[nodiscard]] inline engine::NativeRequirements native()
{
	return engine::NativeRequirements::features(
		::uml::cpu::ArmFeature::lsfe);
}

} // namespace arm64_atomic_floating_test
