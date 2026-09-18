// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_convert_down_engine_test_support.hpp"
#include "cpu/arm64/detail/fp8.hpp"

namespace test = arm64_fp8_convert_down_engine_test;

int main()
{
	static_assert(test::instruction({16U, false}) == 0x0e42'f420U);
	static_assert(test::instruction({32U, true}) == 0x4e02'f420U);
	static_assert(!uml::cpu::arm64::detail::decode_fp8(
		0x0e62'f420U).recognized);
	static_assert(!uml::cpu::arm64::detail::decode_fp8(
		0x4e22'f420U).recognized);
	return 0;
}
