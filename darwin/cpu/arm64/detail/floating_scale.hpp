// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu::arm64::detail {

struct FloatingScale final {
	bool recognized{};
	bool valid{};
	unsigned width{};
	unsigned vector_bytes{};
};

[[nodiscard]] constexpr FloatingScale decode_floating_scale(
	std::uint32_t instruction) noexcept
{
	const bool q = (instruction & 0x4000'0000U) != 0U;
	if ((instruction & 0xbfe0'fc00U) == 0x2ec0'3c00U)
		return {true, true, 16U, q ? 16U : 8U};
	if ((instruction & 0xbfa0'fc00U) == 0x2ea0'fc00U) {
		const bool is_double = (instruction & 0x0040'0000U) != 0U;
		return {true, !is_double || q, is_double ? 64U : 32U,
			q ? 16U : 8U};
	}
	return {};
}

[[nodiscard]] constexpr std::uintptr_t floating_scale_operand(
	std::uint32_t instruction, const FloatingScale &) noexcept
{
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
		(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16);
}

} // namespace uml::cpu::arm64::detail
