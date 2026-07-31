// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_ic_test_support.hpp"

#include <cstdio>

namespace ic = arm64_base_ic_test;
namespace feature_test = arm64_base_feature_system_test;
namespace engine = uml::backend::tests;
namespace sys_test = arm64_system_test;

namespace {

void every_source_register(engine::EngineHarness &harness)
{
	const auto &operation = ic::operations[2];
	feature_test::require(operation.accessible_at_el0);
	for (unsigned source = 0; source < 32U; ++source) {
		const auto initial = sys_test::state();
		char name[112];
		std::snprintf(name, sizeof(name),
			"BASE-0179 IC IVAU Rt=%u", source);
		harness.check_one(name, ic::instruction(operation, source),
			initial, initial, engine::NativeRequirements::baseline());
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	every_source_register(harness);
	feature_test::require_counts(harness, 32U);
	harness.print_summary("arm64-base-ic-ivau");
}
