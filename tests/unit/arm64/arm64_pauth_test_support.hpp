// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>
#include <string_view>

namespace arm64_pauth_test {

namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace test = uml::emulator::tests;

inline constexpr std::uint64_t pc = test::engine_test_code_base;

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

[[nodiscard]] constexpr std::uint32_t data_processing(
	unsigned operation, unsigned destination, unsigned modifier = 31) noexcept
{
	return 0xdac1'0000U | ((operation & 15U) << 10) |
		((modifier & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t hint(unsigned immediate) noexcept
{
	return 0xd503'201fU | ((immediate & 0x7fU) << 5);
}

[[nodiscard]] constexpr std::uint32_t xpac(
	bool data, unsigned destination, unsigned reserved_source = 31) noexcept
{
	return 0xdac1'4000U | (static_cast<unsigned>(data) << 10) |
		((reserved_source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t pacga(
	unsigned destination, unsigned source, unsigned modifier) noexcept
{
	return 0x9ac0'3000U | ((modifier & 31U) << 16) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t authenticated_branch(
	bool link, bool key_b, bool zero_modifier,
	unsigned target, unsigned modifier = 31) noexcept
{
	return (zero_modifier ? 0xd61f'0800U : 0xd71f'0800U) |
		(static_cast<unsigned>(link) << 21) |
		(static_cast<unsigned>(key_b) << 10) |
		((target & 31U) << 5) | (modifier & 31U);
}

[[nodiscard]] constexpr std::uint32_t authenticated_return(bool key_b) noexcept
{
	return key_b ? 0xd65f'0fffU : 0xd65f'0bffU;
}

[[nodiscard]] constexpr std::uint32_t authenticated_load(
	bool key_b, bool writeback, unsigned encoded_offset,
	unsigned base, unsigned destination) noexcept
{
	return 0xf820'0400U | (static_cast<unsigned>(key_b) << 23) |
		(((encoded_offset >> 9) & 1U) << 22) |
		((encoded_offset & 0x1ffU) << 12) |
		(static_cast<unsigned>(writeback) << 11) |
		((base & 31U) << 5) | (destination & 31U);
}

} // namespace arm64_pauth_test
