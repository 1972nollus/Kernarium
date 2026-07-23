// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_matrix_test_support.hpp"

namespace test = arm64_bfloat_matrix_test;

int main()
{
	test::common::Harness harness;
	unsigned seed{};
	for (const auto registers : test::register_cases)
		harness.require_differential(test::instruction(registers),
			test::common::state(++seed));
	test::common::require(!test::common::arm64::detail::
		decode_bfloat_matrix_multiply(0x2e40'ec00U));
	test::common::require(!test::common::arm64::detail::
		decode_bfloat_matrix_multiply(0x6e40'e800U));
}
