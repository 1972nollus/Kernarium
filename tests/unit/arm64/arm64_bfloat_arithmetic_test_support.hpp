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
native_requirements(bool extended_dot)
{
	using ::uml::cpu::ArmFeature;
	auto requirements =
		engine_tests::NativeRequirements::features(ArmFeature::bf16);
	if (extended_dot)
		requirements = requirements | ArmFeature::ebf16;
	return requirements;
}

} // namespace arm64_bfloat_arithmetic_test
