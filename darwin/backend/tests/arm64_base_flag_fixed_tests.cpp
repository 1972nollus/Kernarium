// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_flag_test_support.hpp"

#include <array>
#include <cstdint>

namespace uml::backend::tests::base_flag {
namespace {

struct FixedDescription final {
	std::uint32_t instruction;
	const char *id;
};

constexpr std::array fixed_descriptions{
	FixedDescription{cfinv_encoding, "BASE-0083 CFINV"},
	FixedDescription{axflag_encoding, "BASE-0033 AXFLAG"},
	FixedDescription{xaflag_encoding, "BASE-0514 XAFLAG"},
};

void every_flag_input(EngineHarness &harness)
{
	for (const auto &description : fixed_descriptions)
		for (unsigned flags = 0; flags < 16U; ++flags) {
			auto initial = seeded_state(
				0x3200'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(
					description.instruction) << 17U) ^
				flags);
			initial.nzcv = flags << 28U;
			char name[112];
			std::snprintf(name, sizeof(name), "%s nzcv=%x",
				description.id, flags);
			check(harness, name, description.instruction, initial);
		}
}

void unaffected_state_is_distinct(EngineHarness &harness)
{
	for (const auto &description : fixed_descriptions)
		for (unsigned seed = 0; seed < 8U; ++seed) {
			auto initial = seeded_state(
				0x3210'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(seed) << 40U));
			initial.nzcv = ((seed * 7U) & 15U) << 28U;
			check(harness, description.id, description.instruction,
				initial);
		}
}

} // namespace
} // namespace uml::backend::tests::base_flag

int main()
{
	uml::backend::tests::EngineHarness harness;
	uml::backend::tests::base_flag::every_flag_input(harness);
	uml::backend::tests::base_flag::unaffected_state_is_distinct(harness);
	harness.print_summary("arm64-base-flag-fixed");
	return 0;
}
