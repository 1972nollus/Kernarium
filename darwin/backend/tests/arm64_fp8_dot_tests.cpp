// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_test_support.hpp"

namespace test = arm64_fp8_test;

namespace {

void all_forms_and_indices()
{
	test::common::Harness harness;
	unsigned seed = 100;
	for (const unsigned width : {16U, 32U})
		for (const bool indexed : {false, true})
			for (const bool wide : {false, true}) {
				const unsigned indices = indexed ? (width == 16U ? 8U : 4U) : 1U;
				for (unsigned index = 0; index < indices; ++index) {
					auto initial = test::common::state(++seed);
					initial.fpmr = test::fpmr(seed & 1U, (seed >> 1) & 1U,
						0, seed & 7U, 0, (seed & 8U) != 0);
					test::run(test::dot(width, indexed, wide, index),
						initial, harness);
				}
			}
}

void simple_values_and_scaling()
{
	test::common::Harness harness;
	for (const unsigned width : {16U, 32U}) {
		auto initial = test::common::state(121 + width);
		initial.fpmr = 0;
		initial.fpsr = 0x0800'0000U;
		const unsigned lanes = width == 16U ? 4U : 2U;
		const unsigned count = width / 8U;
		for (unsigned lane = 0; lane < lanes; ++lane) {
			test::set_lane(initial, 0, lane, width,
				width == 16U ? 0x3c00U : 0x3f80'0000U);
			for (unsigned i = 0; i < count; ++i) {
				test::set_byte(initial, 1, lane * count + i, 0x3c);
				test::set_byte(initial, 2, lane * count + i, 0x3c);
			}
		}
		const auto actual = test::run(test::dot(width, false, false), initial, harness);
		for (unsigned lane = 0; lane < lanes; ++lane)
			test::common::require(test::lane(actual, 0, lane, width) ==
				(width == 16U ? 0x4200U : 0x40a0'0000U)); // 3 or 5
		test::common::require(actual.fpsr == initial.fpsr);

		initial.fpmr = test::fpmr(0, 0, 0, 1);
		const auto scaled = test::run(test::dot(width, false, false), initial, harness);
		for (unsigned lane = 0; lane < lanes; ++lane)
			test::common::require(test::lane(scaled, 0, lane, width) ==
				(width == 16U ? 0x4000U : 0x4040'0000U)); // 2 or 3
	}
}

void indexed_selection()
{
	test::common::Harness harness;
	auto initial = test::common::state(140);
	initial.fpmr = 0;
	for (unsigned i = 0; i < 16U; ++i) {
		test::set_byte(initial, 1, i, 0x3c);
		test::set_byte(initial, 2, i, 0x00);
	}
	test::set_byte(initial, 2, 10, 0x3c);
	test::set_byte(initial, 2, 11, 0x3c);
	for (unsigned lane = 0; lane < 8U; ++lane)
		test::set_lane(initial, 0, lane, 16, 0);
	const auto half = test::run(test::dot(16, true, true, 5), initial, harness);
	for (unsigned lane = 0; lane < 8U; ++lane)
		test::common::require(test::lane(half, 0, lane, 16) == 0x4000U);

	for (unsigned i = 0; i < 16U; ++i)
		test::set_byte(initial, 2, i, 0);
	for (unsigned i = 12; i < 16U; ++i)
		test::set_byte(initial, 2, i, 0x3c);
	for (unsigned lane = 0; lane < 4U; ++lane)
		test::set_lane(initial, 0, lane, 32, 0);
	const auto single = test::run(test::dot(32, true, true, 3), initial, harness);
	for (unsigned lane = 0; lane < 4U; ++lane)
		test::common::require(test::lane(single, 0, lane, 32) == 0x4080'0000U);
}

void special_values_zero_and_saturation()
{
	test::common::Harness harness;
	auto initial = test::common::state(151);
	initial.fpmr = 0;
	initial.fpsr = 0x0800'0010U;
	for (unsigned i = 0; i < 8U; ++i) {
		test::set_byte(initial, 1, i, 0x80);
		test::set_byte(initial, 2, i, 0x3c);
	}
	test::set_lane(initial, 0, 0, 16, 0x8000);
	test::set_byte(initial, 1, 0, 0x80); // all products are -0
	const auto zero = test::run(test::dot(16, false, false), initial, harness);
	test::common::require(test::lane(zero, 0, 0, 16) == 0x8000U);
	test::common::require(zero.fpsr == initial.fpsr);

	initial.fpmr = 0;
	test::set_lane(initial, 0, 0, 16, 0);
	test::set_byte(initial, 1, 0, 0x7c);
	test::set_byte(initial, 2, 0, 0x00);
	const auto invalid = test::run(test::dot(16, false, false), initial, harness);
	test::common::require(test::lane(invalid, 0, 0, 16) == 0x7e00U);
	test::common::require(invalid.fpsr == initial.fpsr);

	initial.fpmr = test::fpmr(0, 0, 0, 0, 0, true);
	for (unsigned i = 0; i < 8U; ++i) {
		test::set_byte(initial, 1, i, 0);
		test::set_byte(initial, 2, i, 0);
	}
	test::set_lane(initial, 0, 0, 16, 0x7bff);
	test::set_byte(initial, 1, 0, 0x7b);
	test::set_byte(initial, 2, 0, 0x3c);
	const auto saturated = test::run(test::dot(16, false, false), initial, harness);
	test::common::require(test::lane(saturated, 0, 0, 16) == 0x7bffU);
	test::common::require(saturated.fpsr == initial.fpsr);
}

void exact_cancellation_keeps_distant_term()
{
	test::common::Harness harness;
	auto initial = test::common::state(170);
	initial.fpmr = 0;
	initial.vectors[0].bytes.fill(std::byte{});
	initial.vectors[1].bytes.fill(std::byte{});
	initial.vectors[2].bytes.fill(std::byte{});
	/* +1 + -1 cancels exactly. The remaining product is 2^-16 * 2^-16;
	 * a staged or bounded-alignment implementation would commonly lose it. */
	test::set_byte(initial, 1, 0, 0x3c);
	test::set_byte(initial, 2, 0, 0x3c);
	test::set_byte(initial, 1, 1, 0xbc);
	test::set_byte(initial, 2, 1, 0x3c);
	test::set_byte(initial, 1, 2, 0x01);
	test::set_byte(initial, 2, 2, 0x01);
	const auto actual = test::run(test::dot(32, false, false), initial, harness);
	test::common::require(test::lane(actual, 0, 0, 32) == 0x2f80'0000U);
}

} // namespace

int main()
{
	all_forms_and_indices();
	simple_values_and_scaling();
	indexed_selection();
	special_values_zero_and_saturation();
	exact_cancellation_keeps_distant_term();
}
