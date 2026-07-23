// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_multiply_test_support.hpp"

namespace test = arm64_bfloat_multiply_test;

int main()
{
	test::common::Harness harness;
	unsigned seed{};
	for (const auto form : {test::Form::vector_bottom,
		test::Form::vector_top})
		for (const auto registers : test::vector_register_cases)
			harness.require_differential(
				test::instruction(form, 0U, registers),
				test::common::state(++seed));
	for (const auto form : {test::Form::indexed_bottom,
		test::Form::indexed_top})
		for (unsigned index = 0; index < 8U; ++index)
			for (const auto registers : test::indexed_register_cases)
				harness.require_differential(
					test::instruction(form, index, registers),
					test::common::state(++seed));

	using test::common::arm64::detail::decode_bfloat_multiply_long;
	test::common::require(!decode_bfloat_multiply_long(0x2ec0'f800U).recognized);
	test::common::require(!decode_bfloat_multiply_long(0x0fc0'e000U).recognized);
	test::common::require(!decode_bfloat_multiply_long(0x0fc0'f400U).recognized);
}
