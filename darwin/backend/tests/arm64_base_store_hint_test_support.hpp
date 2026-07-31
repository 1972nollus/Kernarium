// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_base_hint_test_support.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace arm64_base_store_hint_test {

namespace engine = uml::backend::tests;

struct Variant final {
	std::string_view row;
	std::string_view name;
	unsigned immediate;
	uml::backend::ArmFeature feature;
};

inline constexpr std::array variants{
	Variant{"BASE-0432", "STSHH KEEP", 48,
		uml::backend::ArmFeature::pcdphint},
	Variant{"BASE-0432", "STSHH STRM", 49,
		uml::backend::ArmFeature::pcdphint},
	Variant{"BASE-0398", "STCPH", 52,
		uml::backend::ArmFeature::cmh},
};

[[nodiscard]] inline engine::NativeRequirements native(
	const Variant &variant)
{
	return engine::NativeRequirements::features(variant.feature);
}

inline void check(arm64_hint_test::Harness &harness,
	arm64_hint_test::ExpectedCounts &expected, std::string_view name,
	const Variant &variant, std::uint64_t seed)
{
	const auto requirements = native(variant);
	const auto initial = arm64_hint_test::seeded_state(seed);
	harness.check(name,
		arm64_hint_test::primary_instruction(variant.immediate),
		initial, initial, requirements);
	expected.add(requirements);
}

} // namespace arm64_base_store_hint_test
