// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace uml::emulator::tests::base_barrier {

namespace arm64 = uml::cpu::arm64;

inline constexpr std::uint32_t dsb_base = 0xd503'309fU;
inline constexpr std::uint32_t dmb_base = 0xd503'30bfU;
inline constexpr std::uint32_t isb_base = 0xd503'30dfU;
inline constexpr std::uint32_t dsb_nxs_base = 0xd503'323fU;
inline constexpr std::uint32_t sb_base = 0xd503'30ffU;
inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

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

[[nodiscard]] constexpr std::uint32_t encode_standard(
	std::uint32_t base, unsigned option) noexcept
{
	return base | ((option & 15U) << 8U);
}

[[nodiscard]] constexpr std::uint32_t encode_nxs(unsigned scope) noexcept
{
	return dsb_nxs_base | ((scope & 3U) << 10U);
}

} // namespace uml::emulator::tests::base_barrier
