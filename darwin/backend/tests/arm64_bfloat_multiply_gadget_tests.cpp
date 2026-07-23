// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_multiply_test_support.hpp"

namespace test = arm64_bfloat_multiply_test;

int main()
{
	test::common::Harness harness;
	unsigned seed = 500;
	for (const auto form : {test::Form::vector_bottom,
		test::Form::vector_top})
		for (const auto registers : test::vector_register_cases) {
			auto initial = test::common::state(++seed);
			initial.fpcr = (seed & 1U) != 0U ? 0U : 2U;
			harness.require_differential(
				test::instruction(form, 0U, registers), initial);
		}
	for (const auto form : {test::Form::indexed_bottom,
		test::Form::indexed_top})
		for (unsigned index = 0; index < 8U; ++index)
			for (const auto registers : test::indexed_register_cases) {
				auto initial = test::common::state(++seed);
				initial.fpcr = index == 0U ? 1U :
					index == 7U ? 2U : 0U;
				harness.require_differential(
					test::instruction(form, index, registers), initial);
			}
}
