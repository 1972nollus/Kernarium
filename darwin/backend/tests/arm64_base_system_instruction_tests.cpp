// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_system_operation_test_support.hpp"

namespace test = arm64_system_test;

namespace {

[[nodiscard]] constexpr bool scalar_alias_namespace(
	unsigned crn, unsigned crm) noexcept
{
	return crn == 7U || crn == 8U || crn == 9U ||
		(crn == 12U && crm <= 2U);
}

void complete_numeric_allocation(test::Harness &harness)
{
	/*
	 * Exhaust the non-alias SYS allocation with a real source and XZR. C7,
	 * C8/C9, and the C12 alias groups are owned exclusively by their split
	 * reserved/named suites.
	 */
	for (unsigned op1 = 0; op1 < 8; ++op1)
		for (unsigned crn = 0; crn < 16; ++crn)
			for (unsigned crm = 0; crm < 16; ++crm) {
				if (scalar_alias_namespace(crn, crm))
					continue;
				for (unsigned op2 = 0; op2 < 8; ++op2)
					for (const unsigned transfer : {0U, 31U})
						harness.require_illegal(
							test::system_instruction(
								false, op1,
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
		(8U * 16U * 16U * 8U -
		 8U * 8U * (16U + 16U + 16U + 3U)) * 2U;
	arm64_base_system_operation_test::require_invalid_counts(
		harness.engines(), cases);
	harness.print_summary("arm64-base-sys-allocation");
	return 0;
}
