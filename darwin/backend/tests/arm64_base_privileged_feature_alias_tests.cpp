// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_feature_system_test_support.hpp"

#include <array>
#include <cstddef>

namespace feature_test = arm64_base_feature_system_test;
namespace engine = uml::backend::tests;
namespace sys_test = arm64_system_test;
using uml::backend::ArmFeature;

namespace {

struct Operation final {
	const char *name;
	unsigned op1;
	unsigned crm;
	unsigned op2;
	ArmFeature feature;
};

constexpr std::array operations{
	Operation{"apas", 6, 0, 0, ArmFeature::rme_gpc3},
	Operation{"brb-iall", 1, 2, 4, ArmFeature::brbe},
	Operation{"brb-inj", 1, 2, 5, ArmFeature::brbe},
	Operation{"trcit", 3, 2, 7, ArmFeature::ite},
};

void encoding_examples()
{
	feature_test::require(
		sys_test::system_instruction(false, 6, 7, 0, 0, 0) ==
		0xd50e'7000U);
	feature_test::require(
		sys_test::system_instruction(false, 1, 7, 2, 4, 31) ==
		0xd509'729fU);
	feature_test::require(
		sys_test::system_instruction(false, 3, 7, 2, 7, 31) ==
		0xd50b'72ffU);
}

void all_operations_and_sources(engine::EngineHarness &harness)
{
	for (const auto &operation : operations)
		for (unsigned source = 0; source < 32; ++source) {
			const auto instruction = sys_test::system_instruction(
				false, operation.op1, 7, operation.crm,
				operation.op2, source);
			const feature_test::CaseName name{"%s-t%u",
				operation.name, source};
			feature_test::check_feature_disabled(
				harness, name, instruction, operation.feature);
		}
}

} // namespace

int main()
{
	encoding_examples();
	engine::EngineHarness harness;
	all_operations_and_sources(harness);
	constexpr std::size_t cases = operations.size() * 32;
	feature_test::require_counts(harness, cases);
	harness.print_summary("arm64-base-privileged-feature-alias");
}
