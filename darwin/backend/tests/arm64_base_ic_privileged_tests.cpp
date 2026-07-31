// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_ic_test_support.hpp"

#include <cstdio>

namespace ic = arm64_base_ic_test;
namespace feature_test = arm64_base_feature_system_test;
namespace engine = uml::backend::tests;

namespace {

void every_privileged_operation_and_source(engine::EngineHarness &harness)
{
	const auto native = engine::NativeRequirements::baseline();
	for (const auto &operation : ic::operations) {
		if (operation.accessible_at_el0)
			continue;
		for (unsigned source = 0; source < 32U; ++source) {
			char name[128];
			std::snprintf(name, sizeof(name),
				"BASE-0179 IC %s Rt=%u EL0 undefined",
				operation.name, source);
			feature_test::check_profile_illegal(
				harness, name, ic::instruction(operation, source),
				native);
		}
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	every_privileged_operation_and_source(harness);
	feature_test::require_counts(harness, 2U * 32U);
	harness.print_summary("arm64-base-ic-privileged");
}
