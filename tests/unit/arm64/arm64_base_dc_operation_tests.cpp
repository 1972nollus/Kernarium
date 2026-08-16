// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_system_operation_test_support.hpp"

#include <cstddef>

namespace operations = arm64_base_system_operation_test;
namespace engine = uml::emulator::tests;
namespace feature_test = arm64_base_feature_system_test;
namespace sys_test = arm64_system_test;

namespace {

void encoding_examples()
{
	feature_test::require(
		sys_test::system_instruction(false, 0, 7, 6, 1, 0) ==
		0xd508'7620U);
	feature_test::require(
		sys_test::system_instruction(false, 3, 7, 4, 1, 31) ==
		0xd50b'743fU);
	feature_test::require(
		sys_test::system_instruction(false, 6, 7, 14, 5, 31) ==
		0xd50e'7ebfU);
}

void all_named_operations_and_sources(engine::EngineHarness &harness)
{
	for (const auto &operation : operations::dc_operations) {
		/* DC ZVA owns comprehensive behavior, alignment, permission, and
		 * fault suites. Avoid counting those semantic cases twice here. */
		if (operation.op1 == 3U && operation.crm == 4U &&
		    operation.op2 == 1U)
			continue;
		for (unsigned source = 0; source < 32; ++source) {
			const auto instruction = sys_test::system_instruction(false,
				operation.op1, 7, operation.crm, operation.op2,
				source);
			const feature_test::CaseName name{"dc-%s-t%u",
				operation.name, source};
			if (operation.accessible_at_el0) {
				const auto initial = sys_test::state();
				harness.check({
					.name = name,
					.instruction = instruction,
					.initial = initial,
					.expected = initial,
					.native = operations::native_requirement(
						operation.requirement),
				});
			} else {
				feature_test::check_profile_illegal(
					harness, name, instruction,
					operations::native_requirement(
						operation.requirement));
			}
		}
	}
}

} // namespace

int main()
{
	encoding_examples();
	engine::EngineHarness harness;
	all_named_operations_and_sources(harness);
	constexpr std::size_t cases =
		(operations::dc_operations.size() - 1U) * 32U;
	/* Eight non-ZVA baseline operations cannot be feature-skipped. */
	operations::require_mandatory_jit(harness, cases, 8U * 32U);
	harness.print_summary("arm64-base-dc-operations");
}
