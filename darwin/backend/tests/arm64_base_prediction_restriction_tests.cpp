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
	unsigned op2;
	ArmFeature feature;
};

constexpr std::array operations{
	Operation{"cfp", 4, ArmFeature::specres},
	Operation{"dvp", 5, ArmFeature::specres},
	Operation{"cosp", 6, ArmFeature::specres2},
	Operation{"cpp", 7, ArmFeature::specres},
};

void encoding_examples()
{
	feature_test::require(
		sys_test::system_instruction(false, 3, 7, 3, 4, 0) ==
		0xd50b'7380U);
	feature_test::require(
		sys_test::system_instruction(false, 3, 7, 3, 7, 31) ==
		0xd50b'73ffU);
}

void all_operations_and_sources(engine::EngineHarness &harness)
{
	for (const auto &operation : operations)
		for (unsigned source = 0; source < 32; ++source) {
			const auto instruction = sys_test::system_instruction(
				false, 3, 7, 3, operation.op2, source);
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
	harness.print_summary("arm64-base-prediction-restriction");
}
