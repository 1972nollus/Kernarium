// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_system_operation_test_support.hpp"

#include <cstddef>

namespace operations = arm64_base_system_operation_test;
namespace engine = uml::emulator::tests;

namespace {

void non_alias_allocation(engine::EngineHarness &harness)
{
	for (unsigned op1 = 0; op1 < 8; ++op1)
		for (unsigned crn = 0; crn < 16; ++crn) {
			if (crn == 8U || crn == 9U)
				continue;
			for (unsigned crm = 0; crm < 16; ++crm)
				for (unsigned op2 = 0; op2 < 8; ++op2)
					for (const unsigned first :
					     {0U, 1U, 31U})
						harness.check_illegal(
							"sysp-unallocated",
							operations::
								system_instruction_pair(
									op1, crn,
									crm, op2,
									first),
							arm64_system_test::state());
		}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	non_alias_allocation(harness);
	constexpr std::size_t cases = 8U * 14U * 16U * 8U * 3U;
	operations::require_invalid_counts(harness, cases);
	harness.print_summary("arm64-base-sysp-allocation");
}
