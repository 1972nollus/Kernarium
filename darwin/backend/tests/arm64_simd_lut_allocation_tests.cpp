// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_lut_test_support.hpp"

namespace test = arm64_simd_lut_test;

namespace {

void every_allocated_form_and_register_alias()
{
	test::common::Harness harness;
	unsigned seed{};
	for (const auto &variant : test::variants)
		for (const auto registers : test::register_cases)
			harness.require_differential(test::instruction(variant, registers),
				test::common::state(++seed));
}

void every_reserved_form_is_rejected_before_state_access()
{
	test::common::Harness harness;
	for (unsigned len = 0; len < 4; ++len)
		harness.require_illegal(test::instruction(2, len, 0),
			test::common::state(30U + len));
	for (const unsigned len : {0U, 2U})
		harness.require_illegal(test::instruction(1, len, 0),
			test::common::state(40U + len));
}

} // namespace

int main()
{
	every_allocated_form_and_register_alias();
	every_reserved_form_is_rejected_before_state_access();
}
