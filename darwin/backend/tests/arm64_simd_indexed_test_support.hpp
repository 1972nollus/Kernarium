// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_test_support.hpp"

#include <array>
#include <cstdint>

namespace arm64_simd_indexed_test {

namespace base = arm64_simd_test;
namespace arm64 = uml::backend::arm64;

[[nodiscard]] constexpr std::uint32_t instruction(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned operation,
	unsigned destination, unsigned source, unsigned element_register,
	unsigned element_index)
{
	std::uint32_t encoded = (scalar ? 0x5f00'0000U : 0x0f00'0000U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((operation & 15U) << 12) |
		((element_register & 15U) << 16) | ((source & 31U) << 5) |
		(destination & 31U);
	if (size == 1U) {
		encoded |= (element_index & 1U) << 20;
		encoded |= (element_index & 2U) << 20;
		encoded |= (element_index & 4U) << 9;
	} else if (size == 2U) {
		encoded |= ((element_register >> 4) & 1U) << 20;
		encoded |= (element_index & 1U) << 21;
		encoded |= (element_index & 2U) << 10;
	}
	return encoded;
}

[[nodiscard]] inline std::uint64_t read_lane(
	const std::array<std::byte, 16> &bytes, unsigned offset,
	unsigned lane_bytes)
{
	std::uint64_t value = 0;
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(bytes[offset + byte])) << (byte * 8U);
	return value;
}

inline void write_lane(std::array<std::byte, 16> &bytes, unsigned offset,
	unsigned lane_bytes, std::uint64_t value)
{
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		bytes[offset + byte] = static_cast<std::byte>(value >> (byte * 8U));
}

[[nodiscard]] constexpr unsigned __int128 mask(unsigned bits)
{
	return bits == 64U ? static_cast<unsigned __int128>(~std::uint64_t{0}) :
		(static_cast<unsigned __int128>(1) << bits) - 1U;
}

[[nodiscard]] constexpr __int128 signed_value(
	std::uint64_t value, unsigned bits)
{
	const auto sign = static_cast<unsigned __int128>(1) << (bits - 1U);
	return static_cast<__int128>(
		(static_cast<unsigned __int128>(value) ^ sign) - sign);
}

[[nodiscard]] inline std::uint64_t saturate_signed(__int128 value,
	unsigned bits, bool &saturated)
{
	const __int128 minimum = -(static_cast<__int128>(1) << (bits - 1U));
	const __int128 maximum =
		(static_cast<__int128>(1) << (bits - 1U)) - 1;
	if (value < minimum) {
		saturated = true;
		value = minimum;
	} else if (value > maximum) {
		saturated = true;
		value = maximum;
	}
	return static_cast<std::uint64_t>(value) &
		static_cast<std::uint64_t>(mask(bits));
}

inline void set_all_lanes(std::array<std::byte, 16> &bytes,
	unsigned lane_bytes, std::uint64_t value)
{
	for (unsigned offset = 0; offset < 16U; offset += lane_bytes)
		write_lane(bytes, offset, lane_bytes, value);
}

} // namespace arm64_simd_indexed_test
