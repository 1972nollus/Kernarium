// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace uml::emulator::tests::base_crc {

namespace arm64 = uml::cpu::arm64;

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

} // namespace uml::emulator::tests::base_crc
