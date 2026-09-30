// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_bfloat_arithmetic_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace arm64_half_dot_test {
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Form final {
	bool by_element;
	bool wide;
	unsigned index;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array forms{
	Form{false, false, 0U}, Form{false, true, 0U},
	Form{true, false, 0U}, Form{true, false, 1U},
	Form{true, false, 2U}, Form{true, false, 3U},
	Form{true, true, 0U}, Form{true, true, 1U},
	Form{true, true, 2U}, Form{true, true, 3U},
};

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

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	Registers registers = {0U, 1U, 2U}) noexcept
{
	const std::uint32_t base =
		form.by_element ? 0x0f40'9000U : 0x0e80'fc00U;
	const std::uint32_t index = form.by_element ?
		((form.index & 1U) << 21U) |
			((form.index & 2U) << 10U) : 0U;
	return base |
		(static_cast<std::uint32_t>(form.wide) << 30U) |
		index | ((registers.second & 31U) << 16U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements() noexcept
{
	return engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::f16f32dot);
}

} // namespace arm64_half_dot_test
