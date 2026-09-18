// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace arm64_base_cache_zero_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

[[nodiscard]] constexpr std::uint32_t instruction(unsigned source)
{
	return 0xd50b'7420U | (source & 31U);
}

} // namespace arm64_base_cache_zero_test
