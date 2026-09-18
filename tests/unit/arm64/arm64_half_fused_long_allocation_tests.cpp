// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_fused_long_test_support.hpp"

namespace test = arm64_half_fused_long_test;

int main()
{
	using test::arm64::detail::decode_half_fused_long;
	test::require(
		decode_half_fused_long(test::instruction(
			test::vector_forms[0])).recognized,
		"vector allocation was not recognized");
	test::require(
		decode_half_fused_long(test::instruction(
			test::indexed_form(false, false, false, 7U))).recognized,
		"indexed allocation was not recognized");
	test::require(
		!decode_half_fused_long(0x0e60'ec00U).recognized,
		"reserved vector size was recognized");
	test::require(
		!decode_half_fused_long(0x0fc0'0000U).recognized,
		"reserved indexed size was recognized");
	test::require(
		!decode_half_fused_long(0x0f80'0400U).recognized,
		"reserved indexed opcode was recognized");
}
