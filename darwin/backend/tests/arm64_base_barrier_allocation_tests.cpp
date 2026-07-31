// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_barrier_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace uml::backend::tests::base_barrier {
namespace {

struct StandardFamily final {
	std::string_view id;
	std::uint32_t base;
};

void every_allocated_field(EngineHarness &harness)
{
	constexpr std::array families{
		StandardFamily{"BASE-0150 DMB", dmb_base},
		StandardFamily{"BASE-0152 DSB", dsb_base},
		StandardFamily{"BASE-0181 ISB", isb_base},
	};
	for (const auto &family : families)
		for (unsigned option = 0; option < 16U; ++option)
			for (unsigned state_index = 0; state_index < 16U;
			     ++state_index) {
				const auto instruction =
					encode_standard(family.base, option);
				const auto initial = seeded_state(
					0x4200'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(
						instruction) << 11U) ^
					state_index);
				char name[128];
				std::snprintf(name, sizeof(name),
					"%.*s option=%u state=%u",
					static_cast<int>(family.id.size()),
					family.id.data(), option, state_index);
				check(harness, name, instruction, initial);
			}

	for (unsigned scope = 0; scope < 4U; ++scope)
		for (unsigned state_index = 0; state_index < 16U; ++state_index) {
			const auto instruction = encode_nxs(scope);
			const auto initial = seeded_state(
				0x4210'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 13U) ^
				state_index);
			char name[112];
			std::snprintf(name, sizeof(name),
				"BASE-0152 DSB nXS scope=%u state=%u",
				scope, state_index);
			check(harness, name, instruction, initial);
		}
}

} // namespace
} // namespace uml::backend::tests::base_barrier

int main()
{
	uml::backend::tests::EngineHarness harness;
	uml::backend::tests::base_barrier::every_allocated_field(harness);
	harness.print_summary("arm64-base-barrier-allocation");
	return 0;
}
