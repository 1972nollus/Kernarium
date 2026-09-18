// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace uml::emulator::tests::base_multiply {

namespace arm64 = uml::cpu::arm64;

struct FormCase final {
	bool wide;
	unsigned operation;
	bool subtract;
	std::string_view name;
};

inline constexpr std::array legal_forms{
	FormCase{false, 0, false, "MADD W"},
	FormCase{false, 0, true, "MSUB W"},
	FormCase{true, 0, false, "MADD X"},
	FormCase{true, 0, true, "MSUB X"},
	FormCase{true, 1, false, "SMADDL"},
	FormCase{true, 1, true, "SMSUBL"},
	FormCase{true, 5, false, "UMADDL"},
	FormCase{true, 5, true, "UMSUBL"},
	FormCase{true, 2, false, "SMULH"},
	FormCase{true, 6, false, "UMULH"},
};

[[nodiscard]] constexpr bool is_high(const FormCase &form) noexcept
{
	return form.operation == 2U || form.operation == 6U;
}

[[nodiscard]] constexpr std::uint32_t encode(
	bool wide, unsigned op54, unsigned operation, bool subtract,
	unsigned right, unsigned addend, unsigned left,
	unsigned destination) noexcept
{
	return 0x1b00'0000U | (wide ? 0x8000'0000U : 0U) |
		((op54 & 3U) << 29) | ((operation & 7U) << 21) |
		((right & 31U) << 16) | (subtract ? 0x0000'8000U : 0U) |
		((addend & 31U) << 10) | ((left & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode(
	const FormCase &form, unsigned left, unsigned right,
	unsigned addend, unsigned destination) noexcept
{
	return encode(form.wide, 0, form.operation, form.subtract,
		right, addend, left, destination);
}

} // namespace uml::emulator::tests::base_multiply
