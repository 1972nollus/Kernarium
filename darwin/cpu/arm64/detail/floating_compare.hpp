// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu::arm64::detail {

enum class FloatingClass : std::uint8_t {
	zero,
	denormal,
	nonzero,
	infinity,
	quiet_nan,
	signaling_nan,
};

struct FloatingComparison final {
	std::uint32_t nzcv;
	std::uint32_t exceptions;
};

struct UnpackedFloating final {
	std::uint64_t bits;
	FloatingClass type;
};

[[nodiscard]] constexpr UnpackedFloating unpack_floating_for_compare(
	std::uint64_t bits, unsigned width, std::uint32_t fpcr,
	std::uint32_t &exceptions) noexcept
{
	const unsigned exponent_bits = width == 16U ? 5U : width == 32U ? 8U : 11U;
	const unsigned fraction_bits = width - exponent_bits - 1U;
	const auto value_mask = width == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << width) - 1U;
	const auto fraction_mask = (std::uint64_t{1} << fraction_bits) - 1U;
	const auto exponent_mask = ((std::uint64_t{1} << exponent_bits) - 1U) <<
		fraction_bits;
	const auto sign_mask = std::uint64_t{1} << (width - 1U);
	bits &= value_mask;
	const auto exponent = bits & exponent_mask;
	const auto fraction = bits & fraction_mask;
	if (exponent == 0U) {
		if (fraction == 0U)
			return {bits, FloatingClass::zero};
		if (width == 16U) {
			if ((fpcr & (1U << 19)) != 0U)
				return {bits & sign_mask, FloatingClass::zero};
			return {bits, FloatingClass::denormal};
		}
		const bool alternative_handling = (fpcr & 2U) != 0U;
		const bool flush_input = (fpcr & 1U) != 0U;
		const bool flush_to_zero = (fpcr & (1U << 24)) != 0U &&
			!alternative_handling;
		if (flush_to_zero || flush_input) {
			if (flush_to_zero)
				exceptions |= 1U << 7;
			return {bits & sign_mask, FloatingClass::zero};
		}
		return {bits, FloatingClass::denormal};
	}
	if (exponent != exponent_mask)
		return {bits, FloatingClass::nonzero};
	if (fraction == 0U)
		return {bits, FloatingClass::infinity};
	return {bits, (fraction & (std::uint64_t{1} << (fraction_bits - 1U))) != 0U ?
		FloatingClass::quiet_nan : FloatingClass::signaling_nan};
}

[[nodiscard]] constexpr FloatingComparison compare_floating_bits(
	std::uint64_t left_bits, std::uint64_t right_bits, unsigned width,
	bool signal_all_nans, std::uint32_t fpcr) noexcept
{
	std::uint32_t exceptions{};
	const auto left = unpack_floating_for_compare(
		left_bits, width, fpcr, exceptions);
	const auto right = unpack_floating_for_compare(
		right_bits, width, fpcr, exceptions);
	const auto is_nan = [](FloatingClass type) constexpr {
		return type == FloatingClass::quiet_nan ||
			type == FloatingClass::signaling_nan;
	};
	if (is_nan(left.type) || is_nan(right.type)) {
		if (signal_all_nans || left.type == FloatingClass::signaling_nan ||
		    right.type == FloatingClass::signaling_nan)
			exceptions |= 1U;
		return {0x3000'0000U, exceptions};
	}
	if (width != 16U && (fpcr & 2U) != 0U &&
	    (left.type == FloatingClass::denormal ||
	     right.type == FloatingClass::denormal))
		exceptions |= 1U << 7;

	const auto magnitude_mask = (std::uint64_t{1} << (width - 1U)) - 1U;
	const auto sign_mask = std::uint64_t{1} << (width - 1U);
	const auto left_magnitude = left.bits & magnitude_mask;
	const auto right_magnitude = right.bits & magnitude_mask;
	if ((left_magnitude | right_magnitude) == 0U || left.bits == right.bits)
		return {0x6000'0000U, exceptions};
	const bool left_negative = (left.bits & sign_mask) != 0U;
	const bool right_negative = (right.bits & sign_mask) != 0U;
	const bool less = left_negative != right_negative ? left_negative :
		left_negative ? left_magnitude > right_magnitude :
			left_magnitude < right_magnitude;
	return {less ? 0x8000'0000U : 0x2000'0000U, exceptions};
}

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
