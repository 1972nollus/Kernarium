// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_system_operation_test_support.hpp"

#include <cstddef>

namespace operations = arm64_base_system_operation_test;
namespace engine = uml::emulator::tests;
namespace sys_test = arm64_system_test;

namespace {

void every_unallocated_tuple(engine::EngineHarness &harness)
{
	for (unsigned crn = 8; crn <= 9; ++crn)
		for (unsigned op1 = 0; op1 < 8; ++op1)
			for (unsigned crm = 0; crm < 16; ++crm)
				for (unsigned op2 = 0; op2 < 8; ++op2) {
					if (operations::named_tlbi_operation(
						    op1, crn, crm, op2))
						continue;
					for (const unsigned source : {0U, 31U})
						harness.check_illegal(
							"tlbi-reserved",
							sys_test::system_instruction(
								false, op1, crn,
								crm, op2, source),
							sys_test::state());
				}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	every_unallocated_tuple(harness);
	constexpr std::size_t cases = (2048U - 166U) * 2U;
	operations::require_invalid_counts(harness, cases);
	harness.print_summary("arm64-base-tlbi-reserved");
}
