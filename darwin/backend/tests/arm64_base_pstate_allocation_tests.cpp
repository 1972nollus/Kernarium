// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_pstate_test_support.hpp"

namespace uml::backend::tests::base_pstate {
namespace {

void exhaustive_allocation(EngineHarness &harness)
{
	for (unsigned op1 = 0; op1 < 8U; ++op1)
		for (unsigned op2 = 0; op2 < 8U; ++op2)
			for (unsigned immediate = 0; immediate < 16U; ++immediate) {
				const auto decoded = decode(op1, immediate, op2);
				if (decoded.field == Field::flag_alias)
					continue;
				const auto instruction = encode(op1, immediate, op2);
				const auto initial = seeded_state(
					instruction, decoded.field ==
						Field::interrupt_mask_set ||
					decoded.field ==
						Field::interrupt_mask_clear);
				if (!decoded.allocated) {
					check_reserved(harness,
						"base-pstate-reserved-allocation",
						instruction, initial);
				} else if (!decoded.el0_accessible) {
					check_trap(harness,
						"base-pstate-privileged-allocation",
						instruction, initial, decoded.native);
				} else {
					check_allocated(harness,
						"base-pstate-el0-allocation",
						instruction, initial, decoded,
						immediate);
				}
			}
}

} // namespace
} // namespace uml::backend::tests::base_pstate

int main()
{
	uml::backend::tests::EngineHarness harness;
	uml::backend::tests::base_pstate::exhaustive_allocation(harness);
	harness.print_summary("arm64-base-pstate-allocation");
	return 0;
}
