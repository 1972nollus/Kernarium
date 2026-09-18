// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace arm64_bfloat_arithmetic_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

inline constexpr std::uint32_t fpcr_fiz = 1U << 0;
inline constexpr std::uint32_t fpcr_ah = 1U << 1;

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

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(std::uint32_t fpcr, bool extended_dot)
{
	using ::uml::cpu::ArmFeature;
	auto requirements =
		engine_tests::NativeRequirements::features(ArmFeature::bf16);
	if (extended_dot)
		requirements = requirements | ArmFeature::ebf16;
	if ((fpcr & (fpcr_fiz | fpcr_ah)) != 0U)
		requirements = requirements | ArmFeature::afp;
	return requirements;
}

} // namespace arm64_bfloat_arithmetic_test
