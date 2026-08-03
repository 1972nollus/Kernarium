// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_system_operation_test_support.hpp"

#include <cstddef>

namespace operations = arm64_base_system_operation_test;
namespace engine = uml::emulator::tests;

namespace {

void every_unallocated_tuple(engine::EngineHarness &harness)
{
	for (unsigned crn = 8; crn <= 9; ++crn)
		for (unsigned op1 = 0; op1 < 8; ++op1)
			for (unsigned crm = 0; crm < 16; ++crm)
				for (unsigned op2 = 0; op2 < 8; ++op2) {
					if (operations::named_tlbip_operation(
						    op1, crn, crm, op2))
						continue;
					for (const unsigned first : {0U, 1U, 31U})
						harness.check_illegal(
							"tlbip-reserved",
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
	every_unallocated_tuple(harness);
	constexpr std::size_t cases = (2048U - 120U) * 3U;
	operations::require_invalid_counts(harness, cases);
	harness.print_summary("arm64-base-tlbip-reserved");
}
