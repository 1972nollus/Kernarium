// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_system_operation_test_support.hpp"

namespace operations = arm64_base_system_operation_test;
namespace test = arm64_system_test;

namespace {

void cache_instruction_aliases(test::Harness &harness)
{
	/* Exhaust the shared AT/BRB/DC/IC/prediction/GCS/TRCIT C7 namespace.
	 * Every named alias has a dedicated suite. This sweep exclusively owns
	 * the unallocated neighbors and forces them through JIT as invalid. */
	for (unsigned op1 = 0; op1 < 8; ++op1)
		for (unsigned crm = 0; crm < 16; ++crm)
			for (unsigned op2 = 0; op2 < 8; ++op2)
				for (const unsigned transfer : {0U, 31U}) {
					if (operations::dedicated_scalar_alias(
						    false, op1, 7, crm, op2))
						continue;
					harness.require_illegal(
						test::system_instruction(false,
							op1, 7, crm, op2,
							transfer));
				}
}

void generic_interrupt_controller_neighbors(test::Harness &harness)
{
	/* Named FEAT_GCIE operations have dedicated exact-feature-gated suites.
	 * Retain the surrounding SYS allocation here so reserved tuples still
	 * execute through every engine as invalid encodings. */
	for (unsigned op1 = 0; op1 < 8; ++op1)
		for (const unsigned crm : {1U, 2U})
			for (unsigned op2 = 0; op2 < 8; ++op2) {
				if (operations::named_gic_operation(op1, crm, op2))
					continue;
				for (const unsigned transfer : {0U, 31U})
					harness.require_illegal(test::system_instruction(false,
						op1, 12, crm, op2, transfer));
			}
}

void generic_gsb_neighbors(test::Harness &harness)
{
	for (unsigned op1 = 0; op1 < 8; ++op1)
		for (unsigned op2 = 0; op2 < 8; ++op2) {
			if (operations::named_gsb_operation(op1, 0, op2))
				continue;
			for (const unsigned transfer : {0U, 31U})
				harness.require_illegal(test::system_instruction(false,
					op1, 12, 0, op2, transfer));
		}
}

} // namespace

int main()
{
	test::Harness harness;
	cache_instruction_aliases(harness);
	generic_gsb_neighbors(harness);
	generic_interrupt_controller_neighbors(harness);
	constexpr std::size_t c7_reserved = (1024U - 73U) * 2U;
	constexpr std::size_t gsb_reserved = (64U - 2U) * 2U;
	constexpr std::size_t gic_reserved = (128U - 25U) * 2U;
	operations::require_invalid_counts(harness.engines(),
		c7_reserved + gsb_reserved + gic_reserved);
	harness.print_summary("arm64-base-system-aliases");
	return 0;
}
