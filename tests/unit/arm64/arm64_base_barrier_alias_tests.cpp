// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_barrier_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace uml::emulator::tests::base_barrier {
namespace {

struct Alias final {
	std::string_view id;
	unsigned option;
};

void exact_aliases(EngineHarness &harness)
{
	constexpr std::array aliases{
		Alias{"BASE-0325 PSSBB", 4U},
		Alias{"BASE-0387 SSBB", 0U},
	};
	for (const auto &alias : aliases)
		for (unsigned state_index = 0; state_index < 32U;
		     ++state_index) {
			const auto instruction =
				encode_standard(dsb_base, alias.option);
			const auto initial = seeded_state(
				0x4240'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 7U) ^
				state_index);
			char name[112];
			std::snprintf(name, sizeof(name), "%.*s state=%u",
				static_cast<int>(alias.id.size()), alias.id.data(),
				state_index);
			check(harness, name, instruction, initial);
		}
}

} // namespace
} // namespace uml::emulator::tests::base_barrier

int main()
{
	uml::emulator::tests::EngineHarness harness;
	uml::emulator::tests::base_barrier::exact_aliases(harness);
	harness.print_summary("arm64-base-barrier-alias");
	return 0;
}
