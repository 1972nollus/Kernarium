// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "uml_darwin_guest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <span>
#include <string_view>
#include <vector>

namespace mops_test {

namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace engine = uml::emulator::tests;

enum class Stage : unsigned {
	prologue,
	main,
	epilogue,
};

[[nodiscard]] constexpr std::uint32_t raw_instruction(unsigned size,
	unsigned family, unsigned stage, unsigned options, unsigned destination,
	unsigned source, unsigned count) noexcept
{
	return 0x1900'0400U | ((size & 3U) << 30) |
		((family & 1U) << 26) | ((stage & 3U) << 22) |
		((source & 31U) << 16) | ((options & 15U) << 12) |
		((count & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t copy_instruction(bool general,
	Stage stage, unsigned options, unsigned destination, unsigned source,
	unsigned count) noexcept
{
	return raw_instruction(0, general, static_cast<unsigned>(stage), options,
		destination, source, count);
}

[[nodiscard]] constexpr std::uint32_t set_instruction(Stage stage,
	unsigned options, unsigned destination, unsigned count,
	unsigned source) noexcept
{
	return 0x19c0'0400U | (static_cast<unsigned>(stage) << 14) |
		((source & 31U) << 16) | ((options & 3U) << 12) |
		((count & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t set_tag_instruction(Stage stage,
	unsigned options, unsigned destination, unsigned count,
	unsigned source) noexcept
{
	return 0x1dc0'0400U | (static_cast<unsigned>(stage) << 14) |
		((source & 31U) << 16) | ((options & 3U) << 12) |
		((count & 31U) << 5) | (destination & 31U);
}

[[noreturn]] inline void fail(std::string_view message,
	const std::source_location location = std::source_location::current())
{
	std::fprintf(stderr, "MOPS test failure: %.*s at %s:%u\n",
		static_cast<int>(message.size()), message.data(),
		location.file_name(), location.line());
	std::abort();
}

inline void require(bool condition, std::string_view message,
	const std::source_location location = std::source_location::current())
{
	if (!condition)
		fail(message, location);
}

} // namespace mops_test
