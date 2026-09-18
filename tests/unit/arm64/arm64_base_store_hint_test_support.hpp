// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_base_hint_test_support.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace arm64_base_store_hint_test {

namespace engine = uml::emulator::tests;

struct Variant final {
	std::string_view row;
	std::string_view name;
	unsigned immediate;
	::uml::cpu::ArmFeature feature;
};

inline constexpr std::array variants{
	Variant{"BASE-0432", "STSHH KEEP", 48,
		::uml::cpu::ArmFeature::pcdphint},
	Variant{"BASE-0432", "STSHH STRM", 49,
		::uml::cpu::ArmFeature::pcdphint},
	Variant{"BASE-0398", "STCPH", 52,
		::uml::cpu::ArmFeature::cmh},
};

} // namespace arm64_base_store_hint_test
