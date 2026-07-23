// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_test_support.hpp"
#include "cpu/arm64/detail/instruction_helpers.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace arm64_simd_fmov_test {

namespace base = arm64_simd_test;

[[nodiscard]] constexpr unsigned type_for_bytes(unsigned bytes)
{
	return bytes == 2U ? 3U : bytes == 4U ? 0U : 1U;
}

[[nodiscard]] constexpr std::uint32_t register_instruction(unsigned bytes,
	unsigned destination, unsigned source)
{
	return 0x1e20'4000U | (type_for_bytes(bytes) << 22) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t immediate_instruction(unsigned bytes,
	std::uint8_t immediate, unsigned destination)
{
	return 0x1e20'1000U | (type_for_bytes(bytes) << 22) |
		(static_cast<std::uint32_t>(immediate) << 13) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint64_t expand_immediate(
	std::uint8_t immediate, unsigned bits)
{
	const unsigned exponent_bits = bits == 16U ? 5U : bits == 32U ? 8U : 11U;
	const unsigned fraction_bits = bits - exponent_bits - 1U;
	const auto sign = static_cast<std::uint64_t>(immediate >> 7) << (bits - 1U);
	const auto control = static_cast<std::uint64_t>((immediate >> 6) & 1U);
	const auto exponent_top = (control ^ 1U) << (bits - 2U);
	const auto exponent_repeat = control == 0U ? 0U :
		((std::uint64_t{1} << (exponent_bits - 3U)) - 1U) <<
			(fraction_bits + 2U);
	const auto payload = static_cast<std::uint64_t>(immediate & 0x3fU) <<
		(fraction_bits - 4U);
	return sign | exponent_top | exponent_repeat | payload;
}

inline void write_bits(std::array<std::byte, 16> &bytes, unsigned offset,
	unsigned width, std::uint64_t value)
{
	uml::backend::arm64::detail::encode_little_endian(value,
		std::span{bytes}.subspan(offset, width));
}

[[nodiscard]] inline std::uint64_t read_bits(
	const std::array<std::byte, 16> &bytes, unsigned offset, unsigned width)
{
	return uml::backend::arm64::detail::decode_little_endian(
		std::span{bytes}.subspan(offset, width));
}

inline void require_scalar_result(std::uint32_t encoded, unsigned bytes,
	unsigned destination, std::uint64_t expected_bits,
	const uml::backend::arm64::CpuState &initial, base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	expected.vectors[destination].bytes = {};
	write_bits(expected.vectors[destination].bytes, 0U, bytes, expected_bits);
	base::require_architecture_equal(expected,
		harness.run_interpreter(encoded, initial));
	harness.require_differential(encoded, initial);
}

} // namespace arm64_simd_fmov_test
