// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_system_operation_test_support.hpp"

#include <cstddef>

namespace operations = arm64_base_system_operation_test;
namespace engine = uml::emulator::tests;
namespace feature_test = arm64_base_feature_system_test;
namespace sys_test = arm64_system_test;

namespace {

template<std::size_t Size>
void check_group(engine::EngineHarness &harness,
	const std::array<std::uint16_t, Size> &keys,
	operations::Requirement requirement)
{
	for (const auto key : keys) {
		const unsigned op1 = key >> 11;
		const unsigned crn = (key >> 7) & 15U;
		const unsigned crm = (key >> 3) & 15U;
		const unsigned op2 = key & 7U;
		feature_test::require(operations::named_tlbi_operation(
			op1, crn, crm, op2));
		feature_test::require(operations::tlbi_requirement(
			op1, crn, crm, op2) == requirement);
		for (unsigned source = 0; source < 32; ++source) {
			const auto instruction = sys_test::system_instruction(false,
				op1, crn, crm, op2, source);
			const feature_test::CaseName name{
				"tlbi-%04x-t%u", key, source};
			feature_test::check_profile_illegal(
				harness, name, instruction,
				operations::native_requirement(requirement));
		}
	}
}

void all_named_operations(engine::EngineHarness &harness)
{
	check_group(harness, operations::tlbi_baseline,
		operations::Requirement::baseline);
	check_group(harness, operations::tlbi_tlbios,
		operations::Requirement::tlbios);
	check_group(harness, operations::tlbi_tlbirange,
		operations::Requirement::tlbirange);
	check_group(harness, operations::tlbi_tlbiw,
		operations::Requirement::tlbiw);
	check_group(harness, operations::tlbi_rme,
		operations::Requirement::rme);
	check_group(harness, operations::tlbi_xs,
		operations::Requirement::xs);
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	all_named_operations(harness);
	constexpr std::size_t cases = 166U * 32U;
	operations::require_mandatory_jit(
		harness, cases, operations::tlbi_baseline.size() * 32U);
	harness.print_summary("arm64-base-tlbi-named");
}
