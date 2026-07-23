// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_test_support.hpp"

namespace test = arm64_fp8_test;

namespace {

void every_selection_and_form()
{
	test::common::Harness harness;
	unsigned seed = 200;
	for (const unsigned width : {16U, 32U}) {
		const unsigned selections = width == 16U ? 2U : 4U;
		for (const bool indexed : {false, true})
			for (unsigned selection = 0; selection < selections; ++selection) {
				const unsigned indices = indexed ? 16U : 1U;
				for (unsigned index = 0; index < indices; ++index) {
					auto initial = test::common::state(++seed);
					initial.fpmr = test::fpmr(seed & 1U, (seed >> 1) & 1U,
						0, seed & 31U);
					test::run(test::multiply(width, indexed, selection, index),
						initial, harness);
				}
			}
	}
}

void selected_bytes_only()
{
	test::common::Harness harness;
	for (const unsigned width : {16U, 32U}) {
		const unsigned selections = width == 16U ? 2U : 4U;
		const unsigned lanes = 128U / width;
		const unsigned stride = width / 8U;
		for (unsigned selection = 0; selection < selections; ++selection) {
			auto initial = test::common::state(230 + width + selection);
			initial.fpmr = 0;
			for (unsigned i = 0; i < 16U; ++i) {
				test::set_byte(initial, 1, i, 0x40); // 2
				test::set_byte(initial, 2, i, 0x40);
			}
			for (unsigned lane = 0; lane < lanes; ++lane) {
				test::set_lane(initial, 0, lane, width, 0);
				test::set_byte(initial, 1, lane * stride + selection, 0x3c);
				test::set_byte(initial, 2, lane * stride + selection, 0x3c);
			}
			const auto actual = test::run(test::multiply(width, false,
				selection), initial, harness);
			for (unsigned lane = 0; lane < lanes; ++lane)
				test::common::require(test::lane(actual, 0, lane, width) ==
					(width == 16U ? 0x3c00U : 0x3f80'0000U));
		}
	}
}

void indexed_byte()
{
	test::common::Harness harness;
	auto initial = test::common::state(260);
	initial.fpmr = 0;
	for (unsigned i = 0; i < 16U; ++i) {
		test::set_byte(initial, 1, i, 0x3c);
		test::set_byte(initial, 2, i, 0);
	}
	test::set_byte(initial, 2, 13, 0x40); // 2
	for (unsigned lane = 0; lane < 8U; ++lane)
		test::set_lane(initial, 0, lane, 16, 0);
	const auto actual = test::run(test::multiply(16, true, 1, 13),
		initial, harness);
	for (unsigned lane = 0; lane < 8U; ++lane)
		test::common::require(test::lane(actual, 0, lane, 16) == 0x4000U);
}

} // namespace

int main()
{
	every_selection_and_form();
	selected_bytes_only();
	indexed_byte();
}
