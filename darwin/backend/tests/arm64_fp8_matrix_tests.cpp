// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_test_support.hpp"

namespace test = arm64_fp8_test;

namespace {

void all_ones()
{
	test::common::Harness harness;
	for (const unsigned width : {16U, 32U}) {
		auto initial = test::common::state(300 + width);
		initial.fpmr = 0;
		initial.vectors[0].bytes.fill(std::byte{});
		initial.vectors[1].bytes.fill(static_cast<std::byte>(0x3c));
		initial.vectors[2].bytes.fill(static_cast<std::byte>(0x3c));
		const auto actual = test::run(test::matrix(width), initial, harness);
		const unsigned lanes = 128U / width;
		for (unsigned lane = 0; lane < lanes; ++lane)
			test::common::require(test::lane(actual, 0, lane, width) ==
				(width == 16U ? 0x4400U : 0x4100'0000U));
	}
}

void matrix_layout_and_scale()
{
	test::common::Harness harness;
	auto initial = test::common::state(330);
	initial.fpmr = test::fpmr(0, 0, 0, 1);
	initial.vectors[0].bytes.fill(std::byte{});
	initial.vectors[1].bytes.fill(std::byte{});
	initial.vectors[2].bytes.fill(std::byte{});
	/* First half segment: row 0 is ones, row 1 is twos.  Column group 0
	 * is ones, column group 1 is twos.  Four products are then downscaled. */
	for (unsigned i = 0; i < 4U; ++i) {
		test::set_byte(initial, 1, i, 0x3c);
		test::set_byte(initial, 1, 4U + i, 0x40);
		test::set_byte(initial, 2, i, 0x3c);
		test::set_byte(initial, 2, 4U + i, 0x40);
	}
	const auto actual = test::run(test::matrix(16), initial, harness);
	test::common::require(test::lane(actual, 0, 0, 16) == 0x4000U); // 2
	test::common::require(test::lane(actual, 0, 1, 16) == 0x4400U); // 4
	test::common::require(test::lane(actual, 0, 2, 16) == 0x4400U); // 4
	test::common::require(test::lane(actual, 0, 3, 16) == 0x4800U); // 8
}

void special_values_do_not_raise_fpsr()
{
	test::common::Harness harness;
	auto initial = test::common::state(340);
	initial.fpmr = 0;
	initial.fpsr = 0x0800'0010U;
	initial.vectors[0].bytes.fill(std::byte{});
	initial.vectors[1].bytes.fill(static_cast<std::byte>(0x3c));
	initial.vectors[2].bytes.fill(static_cast<std::byte>(0x3c));
	test::set_byte(initial, 1, 0, 0x7d);
	const auto actual = test::run(test::matrix(16), initial, harness);
	test::common::require(test::lane(actual, 0, 0, 16) == 0x7e00U);
	test::common::require(test::lane(actual, 0, 1, 16) == 0x7e00U);
	test::common::require(actual.fpsr == initial.fpsr);
}

} // namespace

int main()
{
	all_ones();
	matrix_layout_and_scale();
	special_values_do_not_raise_fpsr();
}
