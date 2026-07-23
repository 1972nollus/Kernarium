// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_dot_test_support.hpp"

namespace test = arm64_half_dot_test;

int main()
{
	test::common::Harness harness;
	unsigned seed{};
	for (const bool wide : {false, true}) {
		for (const auto registers : test::register_cases) {
			harness.require_differential(
				test::instruction({false, wide, 0}, registers),
				test::common::state(++seed));
			for (unsigned index = 0; index < 4U; ++index)
				harness.require_differential(
					test::instruction({true, wide, index}, registers),
					test::common::state(++seed));
		}
	}
	using test::common::arm64::detail::decode_half_dot;
	test::common::require(decode_half_dot(0x0e81'fc02U).recognized);
	test::common::require(decode_half_dot(0x4f7f'99e2U).recognized);
	test::common::require(!decode_half_dot(0x0e81'f802U).recognized);
	test::common::require(!decode_half_dot(0x0f41'9402U).recognized);
}
