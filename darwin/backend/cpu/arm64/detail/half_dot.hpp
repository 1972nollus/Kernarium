// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "floating_fused.hpp"
#include "half_matrix.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::backend::arm64::detail {

struct HalfDot final {
	bool recognized{};
	bool by_element{};
	bool wide{};
	unsigned index{};
};

[[nodiscard]] constexpr HalfDot decode_half_dot(
	std::uint32_t instruction) noexcept
{
	if ((instruction & 0xbfe0'fc00U) == 0x0e80'fc00U)
		return {true, false, (instruction & 0x4000'0000U) != 0U, 0U};
	if ((instruction & 0xbfc0'f400U) == 0x0f40'9000U) {
		const unsigned index = ((instruction >> 21) & 1U) |
			((instruction >> 10) & 2U);
		return {true, true, (instruction & 0x4000'0000U) != 0U, index};
	}
	return {};
}

[[nodiscard]] constexpr bool reserved_half_dot_scalar_class(
	std::uint32_t instruction) noexcept
{
	/*
	 * Bit 28 is fixed low in the vector family, but setting it does not make
	 * every otherwise matching word unallocated. Indexed narrow forms then
	 * enter the scalar three-source class, where the same fields encode an
	 * allocated fused operation. Let that complete decoder own both its valid
	 * and reserved precision cells.
	 */
	return (instruction & 0x1000'0000U) != 0U &&
		decode_half_dot(instruction & ~0x1000'0000U).recognized &&
		!decode_fused_floating_arithmetic(instruction).recognized;
}

[[nodiscard]] inline HalfMatrixResult half_dot_add(
	const std::array<std::byte, 16> &addends,
	const std::array<std::byte, 16> &left,
	const std::array<std::byte, 16> &right, const HalfDot &decoded,
	std::uint32_t fpcr) noexcept
{
	using namespace half_matrix_detail;
	HalfMatrixResult result{};
	const unsigned lanes = decoded.wide ? 4U : 2U;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const unsigned first_base = lane * 2U;
		const unsigned second_base = decoded.by_element ?
			decoded.index * 2U : first_base;
		const auto product = half_dot(read_half(left, first_base),
			read_half(left, first_base + 1U),
			read_half(right, second_base),
			read_half(right, second_base + 1U), fpcr);
		const auto sum = single_add(read_single(addends, lane),
			static_cast<std::uint32_t>(product.bits), fpcr);
		encode_little_endian(sum.bits, std::span{result.bytes}.subspan(
			lane * 4U, 4U));
		result.exceptions |= product.exceptions | sum.exceptions;
	}
	return result;
}

} // namespace uml::backend::arm64::detail
