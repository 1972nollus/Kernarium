// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_gic_test_support.hpp"

#include <cstddef>

namespace gic = arm64_base_gic_test;
namespace engine = uml::emulator::tests;
namespace sys_test = arm64_system_test;

namespace {

void encoding_examples()
{
	gic::require(sys_test::system_instruction(true, 0, 12, 3, 0, 0) ==
		0xd528'c300U);
	gic::require(sys_test::system_instruction(true, 0, 12, 3, 1, 31) ==
		0xd528'c33fU);
}

void both_operations_and_all_destinations(engine::EngineHarness &harness)
{
	for (unsigned operation = 0; operation < 2; ++operation)
		for (unsigned destination = 0; destination < 32; ++destination) {
			const auto instruction = sys_test::system_instruction(
				true, 0, 12, 3, operation, destination);
			const gic::CaseName name{"BASE-0172 GICR op%u-t%u",
				operation, destination};
			gic::check_feature_disabled(harness, name, instruction);
		}
}

} // namespace

int main()
{
	encoding_examples();
	engine::EngineHarness harness;
	both_operations_and_all_destinations(harness);
	constexpr std::size_t cases = 2 * 32;
	gic::require_counts(harness, cases);
	harness.print_summary("arm64-base-gicr");
}
