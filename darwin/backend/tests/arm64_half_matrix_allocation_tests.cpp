// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_matrix_test_support.hpp"

namespace test = arm64_half_matrix_test;

int main()
{
	test::common::Harness harness;
	unsigned seed{};
	for (const bool widening : {false, true})
		for (const auto registers : test::register_cases)
			harness.require_differential(test::instruction(widening, registers),
				test::common::state(++seed));

	using test::common::arm64::detail::decode_half_matrix;
	test::common::require(
		test::instruction(false, {0, 0, 0}) == 0x4ec0'ec00U);
	test::common::require(
		test::instruction(true, {0, 0, 0}) == 0x4e40'ec00U);
	test::common::require(decode_half_matrix(0x4ec2'ec20U).valid);
	test::common::require(!decode_half_matrix(0x0ec2'ec20U).valid);
	test::common::require(decode_half_matrix(0x2ec2'ec20U).recognized);
	test::common::require(!decode_half_matrix(0x2ec2'ec20U).valid);
	test::common::require(decode_half_matrix(0x6ec2'ec20U).recognized);
	test::common::require(!decode_half_matrix(0x6ec2'ec20U).valid);
	test::common::require(decode_half_matrix(0x4e45'ec83U).widening);
	test::common::require(!decode_half_matrix(0x0e45'ec83U).valid);
	test::common::require(!decode_half_matrix(0x6e40'ec00U).recognized);
	harness.require_illegal(0x0ec2'ec20U);
	harness.require_illegal(0x2ec2'ec20U);
	harness.require_illegal(0x6ec2'ec20U);
	harness.require_illegal(0x0e45'ec83U);
}
