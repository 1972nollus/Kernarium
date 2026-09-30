// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu::arm64::detail {

enum class SimdFloatingCompareOperation : std::uint8_t {
	equal,
	greater_equal,
	greater,
	less_equal,
	less,
	absolute_greater_equal,
	absolute_greater,
};

struct SimdFloatingCompareDecode final {
	bool recognized{};
	bool valid{};
	bool zero{};
	bool scalar{};
	unsigned width{};
	unsigned vector_bytes{};
	unsigned shape{};
	SimdFloatingCompareOperation operation{};
};

[[nodiscard]] constexpr SimdFloatingCompareDecode decode_simd_floating_compare(
	std::uint32_t instruction) noexcept
{
	SimdFloatingCompareDecode decoded{};
	const auto match = [&](std::uint32_t mask, std::uint32_t value,
		SimdFloatingCompareOperation operation, bool zero, bool scalar,
		unsigned width, unsigned shape) constexpr {
		if (decoded.recognized || (instruction & mask) != value)
			return;
		decoded = {true, true, zero, scalar, width,
			scalar ? width / 8U :
				(instruction & 0x4000'0000U) != 0U ? 16U : 8U,
			shape, operation};
	};
	/* Register forms: scalar H, scalar S/D, vector H, vector S/D. */
#define UML_MATCH_REGISTER(operation, half_base, ordinary_base) \
	match(0xffe0'fc00U, half_base | 0x5000'0000U, operation, false, true, 16U, 0U); \
	match(0xffa0'fc00U, ordinary_base | 0x5000'0000U, operation, false, true, \
		(instruction & 0x0040'0000U) != 0U ? 64U : 32U, \
		(instruction & 0x0040'0000U) != 0U ? 2U : 1U); \
	match(0xbfe0'fc00U, half_base, operation, false, false, 16U, \
		(instruction & 0x4000'0000U) != 0U ? 4U : 3U); \
	match(0xbfa0'fc00U, ordinary_base, operation, false, false, \
		(instruction & 0x0040'0000U) != 0U ? 64U : 32U, \
		(instruction & 0x0040'0000U) != 0U ? 7U : \
			(instruction & 0x4000'0000U) != 0U ? 6U : 5U)
	UML_MATCH_REGISTER(SimdFloatingCompareOperation::equal,
		0x0e40'2400U, 0x0e20'e400U);
	UML_MATCH_REGISTER(SimdFloatingCompareOperation::greater_equal,
		0x2e40'2400U, 0x2e20'e400U);
	UML_MATCH_REGISTER(SimdFloatingCompareOperation::greater,
		0x2ec0'2400U, 0x2ea0'e400U);
	UML_MATCH_REGISTER(SimdFloatingCompareOperation::absolute_greater_equal,
		0x2e40'2c00U, 0x2e20'ec00U);
	UML_MATCH_REGISTER(SimdFloatingCompareOperation::absolute_greater,
		0x2ec0'2c00U, 0x2ea0'ec00U);
#undef UML_MATCH_REGISTER

	/* Zero forms use the same four shape classes. */
#define UML_MATCH_ZERO(operation, half_base, ordinary_base) \
	match(0xffff'fc00U, half_base | 0x5000'0000U, operation, true, true, 16U, 0U); \
	match(0xffbf'fc00U, ordinary_base | 0x5000'0000U, operation, true, true, \
		(instruction & 0x0040'0000U) != 0U ? 64U : 32U, \
		(instruction & 0x0040'0000U) != 0U ? 2U : 1U); \
	match(0xbfff'fc00U, half_base, operation, true, false, 16U, \
		(instruction & 0x4000'0000U) != 0U ? 4U : 3U); \
	match(0xbfbf'fc00U, ordinary_base, operation, true, false, \
		(instruction & 0x0040'0000U) != 0U ? 64U : 32U, \
		(instruction & 0x0040'0000U) != 0U ? 7U : \
			(instruction & 0x4000'0000U) != 0U ? 6U : 5U)
	UML_MATCH_ZERO(SimdFloatingCompareOperation::equal,
		0x0ef8'd800U, 0x0ea0'd800U);
	UML_MATCH_ZERO(SimdFloatingCompareOperation::greater_equal,
		0x2ef8'c800U, 0x2ea0'c800U);
	UML_MATCH_ZERO(SimdFloatingCompareOperation::greater,
		0x0ef8'c800U, 0x0ea0'c800U);
	UML_MATCH_ZERO(SimdFloatingCompareOperation::less_equal,
		0x2ef8'd800U, 0x2ea0'd800U);
	UML_MATCH_ZERO(SimdFloatingCompareOperation::less,
		0x0ef8'e800U, 0x0ea0'e800U);
#undef UML_MATCH_ZERO
	if (decoded.recognized && !decoded.scalar && decoded.width == 64U &&
	    decoded.vector_bytes != 16U)
		decoded.valid = false;
	return decoded;
}

[[nodiscard]] constexpr std::uintptr_t simd_floating_compare_operand(
	std::uint32_t instruction, const SimdFloatingCompareDecode &decoded) noexcept
{
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
		(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16) |
		(static_cast<std::uintptr_t>(decoded.shape) << 24) |
		(static_cast<std::uintptr_t>(decoded.operation) << 28) |
		(static_cast<std::uintptr_t>(decoded.zero) << 32) |
		(static_cast<std::uintptr_t>(decoded.scalar) << 33);
}

} // namespace uml::cpu::arm64::detail
