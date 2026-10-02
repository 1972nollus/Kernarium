// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace uml::emulator::tests::base_min_max {

namespace arm64 = uml::cpu::arm64;

enum class Operation : unsigned {
	signed_maximum,
	unsigned_maximum,
	signed_minimum,
	unsigned_minimum,
};

struct Description final {
	Operation operation;
	std::string_view immediate_id;
	std::string_view register_id;
};

inline constexpr std::array descriptions{
	Description{Operation::signed_maximum, "BASE-0376 SMAX (immediate)",
		"BASE-0377 SMAX (register)"},
	Description{Operation::unsigned_maximum, "BASE-0500 UMAX (immediate)",
		"BASE-0501 UMAX (register)"},
	Description{Operation::signed_minimum, "BASE-0379 SMIN (immediate)",
		"BASE-0380 SMIN (register)"},
	Description{Operation::unsigned_minimum, "BASE-0502 UMIN (immediate)",
		"BASE-0503 UMIN (register)"},
};

[[nodiscard]] constexpr std::uint32_t encode_immediate(
	bool wide, Operation operation, unsigned immediate, unsigned rd,
	unsigned rn)
{
	return 0x11c0'0000U | (wide ? 0x8000'0000U : 0U) |
		(static_cast<unsigned>(operation) << 18U) |
		((immediate & 0xffU) << 10U) | ((rn & 31U) << 5U) |
		(rd & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_register(
	bool wide, Operation operation, unsigned rd, unsigned rn, unsigned rm)
{
	return 0x1ac0'6000U | (wide ? 0x8000'0000U : 0U) |
		((rm & 31U) << 16U) |
		(static_cast<unsigned>(operation) << 10U) |
		((rn & 31U) << 5U) | (rd & 31U);
}

inline void require(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "%s\n", message);
		std::abort();
	}
}

} // namespace uml::emulator::tests::base_min_max
