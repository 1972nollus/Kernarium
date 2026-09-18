// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace uml::emulator::tests::base_condition {

namespace arm64 = uml::cpu::arm64;

enum class SelectOperation : unsigned {
	select,
	increment,
	invert,
	negate,
};

struct SelectDescription final {
	SelectOperation operation;
	std::string_view id;
};

inline constexpr std::array select_descriptions{
	SelectDescription{SelectOperation::select, "BASE-0138 CSEL"},
	SelectDescription{SelectOperation::increment, "BASE-0141 CSINC"},
	SelectDescription{SelectOperation::invert, "BASE-0142 CSINV"},
	SelectDescription{SelectOperation::negate, "BASE-0143 CSNEG"},
};

[[nodiscard]] constexpr std::uint32_t encode_select(
	bool wide, SelectOperation operation, unsigned condition, unsigned rd,
	unsigned rn, unsigned rm)
{
	const auto op = static_cast<unsigned>(operation);
	return 0x1a80'0000U | (wide ? 0x8000'0000U : 0U) |
		((op & 2U) << 29U) | ((rm & 31U) << 16U) |
		((condition & 15U) << 12U) | ((op & 1U) << 10U) |
		((rn & 31U) << 5U) | (rd & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_compare(
	bool wide, bool subtract, bool immediate, unsigned condition,
	unsigned alternative, unsigned rn, unsigned operand)
{
	return 0x3a40'0000U | (wide ? 0x8000'0000U : 0U) |
		(subtract ? 0x4000'0000U : 0U) |
		((operand & 31U) << 16U) | ((condition & 15U) << 12U) |
		(immediate ? 0x0000'0800U : 0U) |
		((rn & 31U) << 5U) | (alternative & 15U);
}

} // namespace uml::emulator::tests::base_condition
