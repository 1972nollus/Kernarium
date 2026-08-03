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
inline constexpr std::size_t register_cases = 32U * 32U;

[[nodiscard]] constexpr std::uint32_t instruction(
	std::uint32_t base, unsigned transfer, unsigned address)
{
	return base | ((address & 31U) << 5) | (transfer & 31U);
}

inline void check_register_matrix(engine::EngineHarness &harness,
	std::string_view row, std::string_view operation, std::uint32_t base)
{
	const auto native = engine::NativeRequirements::features(
		::uml::cpu::ArmFeature::mte2);
	for (unsigned address = 0; address < 32U; ++address)
		for (unsigned transfer = 0; transfer < 32U; ++transfer) {
			char name[144];
			std::snprintf(name, sizeof(name),
				"%.*s %.*s Rt=%u Rn=%u EL0 undefined",
				static_cast<int>(row.size()), row.data(),
				static_cast<int>(operation.size()), operation.data(),
				transfer, address);
			feature_test::check_profile_illegal(harness, name,
				instruction(base, transfer, address), native);
		}
}

} // namespace arm64_base_mte_multiple_test
