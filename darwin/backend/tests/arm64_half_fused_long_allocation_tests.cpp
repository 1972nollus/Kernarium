// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_fused_long_test_support.hpp"

namespace test = arm64_half_fused_long_test;

int main()
{
	test::common::Harness harness;
	unsigned seed{};
	for (const bool subtract : {false, true})
		for (const bool upper : {false, true})
			for (const bool wide : {false, true})
				for (const auto registers : test::vector_register_cases)
					harness.require_differential(test::instruction(
						{false, subtract, upper, wide, 0U}, registers),
						test::common::state(++seed));
	for (const bool subtract : {false, true})
		for (const bool upper : {false, true})
			for (const bool wide : {false, true})
				for (unsigned index = 0; index < 8U; ++index)
					for (const auto registers : test::indexed_register_cases)
						harness.require_differential(test::instruction(
							{true, subtract, upper, wide, index}, registers),
							test::common::state(++seed));

	using test::common::arm64::detail::decode_half_fused_long;
	test::common::require(!decode_half_fused_long(0x0e60'ec00U).recognized);
	test::common::require(!decode_half_fused_long(0x0fc0'0000U).recognized);
	test::common::require(!decode_half_fused_long(0x0f80'0400U).recognized);
}
