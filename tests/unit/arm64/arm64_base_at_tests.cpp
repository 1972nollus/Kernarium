// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_feature_system_test_support.hpp"

#include <array>
#include <cstddef>

namespace feature_test = arm64_base_feature_system_test;
namespace engine = uml::emulator::tests;
namespace sys_test = arm64_system_test;
using ::uml::cpu::ArmFeature;

namespace {

enum class Requirement {
	baseline,
	pan2,
	ats1a,
};

struct Operation final {
	const char *name;
	unsigned op1;
	unsigned crm;
	unsigned op2;
	Requirement requirement;
};

constexpr std::array operations{
	Operation{"s1e1r", 0, 8, 0, Requirement::baseline},
	Operation{"s1e1w", 0, 8, 1, Requirement::baseline},
	Operation{"s1e0r", 0, 8, 2, Requirement::baseline},
	Operation{"s1e0w", 0, 8, 3, Requirement::baseline},
	Operation{"s1e1rp", 0, 9, 0, Requirement::pan2},
	Operation{"s1e1wp", 0, 9, 1, Requirement::pan2},
	Operation{"s1e1a", 0, 9, 2, Requirement::ats1a},
	Operation{"s1e2r", 4, 8, 0, Requirement::baseline},
	Operation{"s1e2w", 4, 8, 1, Requirement::baseline},
	Operation{"s12e1r", 4, 8, 4, Requirement::baseline},
	Operation{"s12e1w", 4, 8, 5, Requirement::baseline},
	Operation{"s12e0r", 4, 8, 6, Requirement::baseline},
	Operation{"s12e0w", 4, 8, 7, Requirement::baseline},
	Operation{"s1e2a", 4, 9, 2, Requirement::ats1a},
	Operation{"s1e3r", 6, 8, 0, Requirement::baseline},
	Operation{"s1e3w", 6, 8, 1, Requirement::baseline},
	Operation{"s1e3a", 6, 9, 2, Requirement::ats1a},
};

static_assert(operations.size() == 17);

[[nodiscard]] engine::NativeRequirements
native_requirement(Requirement requirement)
{
	switch (requirement) {
	case Requirement::baseline:
		return engine::NativeRequirements::baseline();
	case Requirement::pan2:
		return engine::NativeRequirements::features(ArmFeature::pan2);
	case Requirement::ats1a:
		return engine::NativeRequirements::features(ArmFeature::ats1a);
	}
	std::abort();
}

void encoding_examples()
{
	feature_test::require(
		sys_test::system_instruction(false, 0, 7, 8, 0, 0) ==
		0xd508'7800U);
	feature_test::require(
		sys_test::system_instruction(false, 4, 7, 8, 7, 31) ==
		0xd50c'78ffU);
	feature_test::require(
		sys_test::system_instruction(false, 6, 7, 9, 2, 31) ==
		0xd50e'795fU);
}

void all_operations_and_sources(engine::EngineHarness &harness)
{
	for (const auto &operation : operations)
		for (unsigned source = 0; source < 32; ++source) {
			const auto instruction = sys_test::system_instruction(
				false, operation.op1, 7, operation.crm,
				operation.op2, source);
			const feature_test::CaseName name{"at-%s-t%u",
				operation.name, source};
			feature_test::check_profile_illegal(harness, name,
				instruction,
				native_requirement(operation.requirement));
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
	harness.print_summary("arm64-base-at");
}
