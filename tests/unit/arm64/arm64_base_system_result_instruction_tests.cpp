// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_system_operation_test_support.hpp"

namespace operations = arm64_base_system_operation_test;
namespace test = arm64_system_test;

namespace {

void complete_numeric_allocation(test::Harness &harness)
{
	/*
	 * SYSL has a complete numeric selector. The two guarded-control-stack
	 * result operations and two interrupt-controller reads have dedicated
	 * exact-feature suites; every other tuple is profile-illegal at EL0.
	 */
	for (unsigned op1 = 0; op1 < 8; ++op1)
		for (unsigned crn = 0; crn < 16; ++crn)
			for (unsigned crm = 0; crm < 16; ++crm)
				for (unsigned op2 = 0; op2 < 8; ++op2) {
					if (operations::dedicated_scalar_alias(
						    true, op1, crn, crm, op2))
						continue;
					for (const unsigned transfer : {0U, 31U})
						harness.require_illegal(
							test::system_instruction(
								true, op1,
								crn, crm,
								op2,
								transfer));
				}
}

} // namespace

int main()
{
	test::Harness harness;
	complete_numeric_allocation(harness);
	constexpr std::size_t cases =
		(8U * 16U * 16U * 8U - 4U) * 2U;
	operations::require_invalid_counts(harness.engines(), cases);
	harness.print_summary("arm64-base-sysl-allocation");
	return 0;
}
