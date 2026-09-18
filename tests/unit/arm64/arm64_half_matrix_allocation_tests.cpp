// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_matrix_test_support.hpp"

namespace test = arm64_half_matrix_test;

int main()
{
	using test::arm64::detail::decode_half_matrix;
	test::require(test::instruction(false, {0U, 0U, 0U}) ==
		0x4ec0'ec00U, "wrong non-widening encoding");
	test::require(test::instruction(true, {0U, 0U, 0U}) ==
		0x4e40'ec00U, "wrong widening encoding");
	test::require(decode_half_matrix(0x4ec2'ec20U).valid,
		"valid non-widening allocation was rejected");
	test::require(!decode_half_matrix(0x0ec2'ec20U).valid,
		"Q=0 non-widening allocation was accepted");
	test::require(decode_half_matrix(0x2ec2'ec20U).recognized &&
		!decode_half_matrix(0x2ec2'ec20U).valid,
		"U=1 non-widening allocation was not reserved");
	test::require(decode_half_matrix(0x4e45'ec83U).widening,
		"valid widening allocation was rejected");
	test::require(!decode_half_matrix(0x0e45'ec83U).valid,
		"Q=0 widening allocation was accepted");
}
