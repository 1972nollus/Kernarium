// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_lsui_pair_test_support.hpp"

#include <cstdio>

namespace lsui = arm64_base_lsui_pair_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

namespace {

void duplicate_load_destinations(
	engine::EngineHarness &harness, lsui::ExpectedCounts &counts)
{
	auto initial = atomic::state(0x8100);
	initial.registers[20] = lsui::base_address;
	for (const auto &variant : lsui::variants) {
		if (!variant.load)
			continue;
		for (unsigned destination = 0; destination < 32; ++destination) {
			for (const unsigned imm7 : {0U, 0x3fU, 0x40U, 0x7fU}) {
				char name[128];
				std::snprintf(name, sizeof(name),
					"%.*s duplicate destination=%u imm7=%u",
					static_cast<int>(variant.mnemonic.size()),
					variant.mnemonic.data(), destination, imm7);
				lsui::check_illegal(harness, counts, name,
					lsui::instruction(variant.load, variant.mode, imm7,
						20, destination, destination),
					initial);
			}
		}
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	lsui::ExpectedCounts counts;
	duplicate_load_destinations(harness, counts);
	lsui::require_counts(harness, counts);
	harness.print_summary("arm64-base-lsui-pair-allocation");
}
