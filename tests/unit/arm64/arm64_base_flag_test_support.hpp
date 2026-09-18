// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace uml::emulator::tests::base_flag {

namespace arm64 = uml::cpu::arm64;

inline constexpr std::uint32_t cfinv_encoding = 0xd500'401fU;
inline constexpr std::uint32_t xaflag_encoding = 0xd500'403fU;
inline constexpr std::uint32_t axflag_encoding = 0xd500'405fU;

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

[[nodiscard]] constexpr std::uint32_t encode_rmif(
	unsigned source, unsigned rotation, unsigned mask) noexcept
{
	return 0xba00'0400U | ((rotation & 63U) << 15U) |
		((source & 31U) << 5U) | (mask & 15U);
}

[[nodiscard]] constexpr std::uint32_t encode_setf(
	bool sixteen, unsigned source) noexcept
{
	return 0x3a00'080dU | (sixteen ? 0x4000U : 0U) |
		((source & 31U) << 5U);
}

} // namespace uml::emulator::tests::base_flag
