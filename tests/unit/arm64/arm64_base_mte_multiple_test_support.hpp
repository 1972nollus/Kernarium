// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_base_feature_system_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace arm64_base_mte_multiple_test {

namespace engine = uml::emulator::tests;
namespace feature_test = arm64_base_feature_system_test;

inline constexpr std::uint32_t ldgm_base = 0xd9e0'0000U;
inline constexpr std::uint32_t stgm_base = 0xd9a0'0000U;
inline constexpr std::uint32_t stzgm_base = 0xd920'0000U;

[[nodiscard]] constexpr std::uint32_t instruction(
	std::uint32_t base, unsigned transfer, unsigned address)
{
	return base | ((address & 31U) << 5) | (transfer & 31U);
}

} // namespace arm64_base_mte_multiple_test
