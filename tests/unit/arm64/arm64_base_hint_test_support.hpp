// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_system_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

namespace arm64_hint_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

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

[[nodiscard]] constexpr std::uint32_t primary_instruction(
	unsigned immediate)
{
	return 0xd503'201fU | ((immediate & 0x7fU) << 5);
}

[[nodiscard]] constexpr std::uint32_t gsb_instruction(
	unsigned operation, unsigned transfer)
{
	return 0xd508'c000U | ((operation & 7U) << 5) |
		(transfer & 31U);
}

/*
 * These primary-HINT encodings are delegated by the architecture to the
 * pointer-authentication instruction descriptions. They have architectural
 * register effects and are covered by that family's stateful tests rather
 * than by the stateless HINT/event family.
 */
[[nodiscard]] constexpr bool is_delegated_stateful_hint(unsigned immediate)
{
	return immediate == 7U || immediate == 8U || immediate == 10U ||
		immediate == 12U || immediate == 14U ||
		(immediate >= 24U && immediate <= 31U) ||
		immediate == 39U;
}

[[nodiscard]] constexpr bool is_owned_primary_hint(unsigned immediate)
{
	return immediate < 128U &&
		immediate != 40U &&
		immediate != 48U &&
		immediate != 49U &&
		immediate != 52U &&
		!is_delegated_stateful_hint(immediate);
}

} // namespace arm64_hint_test
