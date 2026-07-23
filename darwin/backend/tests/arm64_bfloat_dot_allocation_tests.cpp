// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_dot_test_support.hpp"

namespace test = arm64_bfloat_dot_test;

int main()
{
	test::common::Harness harness;
	unsigned seed{};
	for (const bool wide : {false, true})
		for (const auto registers : test::register_cases)
			harness.require_differential(test::instruction({false, wide, 0U},
				registers), test::common::state(++seed));
	for (const bool wide : {false, true})
		for (unsigned index = 0; index < 4U; ++index)
			for (const auto registers : test::register_cases)
				harness.require_differential(test::instruction(
					{true, wide, index}, registers),
					test::common::state(++seed));

	using test::common::arm64::detail::decode_bfloat_dot;
	test::common::require(!decode_bfloat_dot(0x2e40'f800U).recognized);
	test::common::require(!decode_bfloat_dot(0x0f40'e000U).recognized);
	test::common::require(!decode_bfloat_dot(0x0f40'f400U).recognized);
}
