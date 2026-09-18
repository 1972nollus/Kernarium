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

[[nodiscard]] constexpr std::uint64_t reverse_bits(
	std::uint64_t value, unsigned width) noexcept
{
	std::uint64_t result = 0;
	for (unsigned bit = 0; bit < width; ++bit) {
		result = (result << 1) | (value & 1U);
		value >>= 1;
	}
	return result;
}

/*
 * Independent specification oracle. This follows the architectural
 * pseudocode literally rather than repeating the interpreter's reflected
 * byte recurrence:
 *
 *   reverse32(acc)::zeros(size) XOR reverse_size(value)::zeros(32)
 *
 * is divided as a polynomial over {0,1} by the canonical degree-32
 * polynomial, and the 32-bit remainder is reversed for Wd.
 */
[[nodiscard]] constexpr std::uint32_t oracle(
	std::uint32_t accumulator, std::uint64_t value, unsigned byte_count,
	bool alternate) noexcept
{
	using Wide = unsigned __int128;
	const unsigned value_bits = byte_count * 8U;
	const std::uint32_t polynomial =
		alternate ? 0x1edc'6f41U : 0x04c1'1db7U;
	Wide dividend =
		(static_cast<Wide>(reverse_bits(accumulator, 32U)) << value_bits) ^
		(static_cast<Wide>(reverse_bits(value, value_bits)) << 32U);
	const Wide divisor = (Wide{1} << 32U) | polynomial;
	for (unsigned degree = 32U + value_bits; degree-- > 32U;)
		if (((dividend >> degree) & 1U) != 0U)
			dividend ^= divisor << (degree - 32U);
	return static_cast<std::uint32_t>(
		reverse_bits(static_cast<std::uint32_t>(dividend), 32U));
}

static_assert(oracle(0U, 1U, 1U, false) == 0x7707'3096U);
static_assert(oracle(0x1234'5678U, 0x0123'4567'89ab'cdefULL,
	8U, false) == 0x9b62'eadfU);
static_assert(oracle(0U, 1U, 1U, true) == 0xf26b'8303U);
static_assert(oracle(0x89ab'cdefU, 0xffff'ffff'ffff'ffffULL,
	8U, true) == 0x0da9'1e67U);

} // namespace uml::emulator::tests::base_crc
