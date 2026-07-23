// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_test_support.hpp"

namespace test = arm64_fp8_test;

namespace {

void ah_nan_sign_and_underflow_timing()
{
	test::common::Harness harness;
	auto initial = test::common::state(500);
	initial.fpcr = 2U; // AH
	initial.fpmr = 0;
	initial.fpsr = 0;
	initial.vectors[1].bytes.fill(std::byte{});
	initial.vectors[2].bytes.fill(std::byte{});
	test::set_lane(initial, 1, 0, 16, 0x7e01);
	const auto nan = test::run(test::convert_down(16, false), initial, harness);
	test::common::require(test::byte(nan, 0, 0) == 0xfeU);
	test::common::require(nan.fpsr == 0);

	initial.fpcr = 0;
	initial.fpsr = 0;
	test::set_lane(initial, 1, 0, 16, 0x0001); // 2^-24, below FP8 range
	const auto standard = test::run(test::convert_down(16, false), initial, harness);
	test::common::require(test::byte(standard, 0, 0) == 0U);
	test::common::require((standard.fpsr & 0x18U) == 0x18U);
	initial.fpcr = 2U;
	initial.fpsr = 0;
	const auto alternate = test::run(test::convert_down(16, false), initial, harness);
	test::common::require(test::byte(alternate, 0, 0) == 0U);
	test::common::require((alternate.fpsr & 0x18U) == 0x10U);
}

void scale_field_widths()
{
	test::common::Harness harness;
	auto initial = test::common::state(501);
	initial.fpmr = test::fpmr(0, 0, 0, 0, 0x20);
	test::set_lane(initial, 1, 0, 16, 0x3c00);
	const auto half = test::run(test::convert_down(16, false), initial, harness);
	test::common::require(test::byte(half, 0, 0) == 0x3cU); // low five bits only
	test::set_lane(initial, 1, 0, 32, 0x3f80'0000);
	const auto single = test::run(test::convert_down(32, false), initial, harness);
	test::common::require(test::byte(single, 0, 0) == 0x7cU); // full +32 overflows

	initial = test::common::state(502);
	initial.fpmr = test::fpmr(0, 0, 0, 16);
	initial.vectors[0].bytes.fill(std::byte{});
	initial.vectors[1].bytes.fill(static_cast<std::byte>(0x3c));
	initial.vectors[2].bytes.fill(static_cast<std::byte>(0x3c));
	const auto half_dot = test::run(test::dot(16, false, false), initial, harness);
	test::common::require(test::lane(half_dot, 0, 0, 16) == 0x4000U);
	const auto single_dot = test::run(test::dot(32, false, false), initial, harness);
	test::common::require(test::lane(single_dot, 0, 0, 32) == 0x3880'0000U);

	initial = test::common::state(503);
	initial.fpmr = test::fpmr(0, 0, 0, 0, 0, false, false, 33);
	test::set_byte(initial, 1, 0, 0x3c);
	const auto half_source2 = test::run(test::convert_up(false, true, false),
		initial, harness);
	test::common::require(test::lane(half_source2, 0, 0, 16) == 0x3800U);
	const auto bfloat_source2 = test::run(test::convert_up(true, true, false),
		initial, harness);
	test::common::require(test::lane(bfloat_source2, 0, 0, 16) == 0x2f00U);
}

void fixed_rounding_ignores_rmode_and_flush_controls()
{
	test::common::Harness harness;
	auto initial = test::common::state(504);
	initial.fpmr = 0;
	test::set_lane(initial, 1, 0, 16, 0x3c80); // tie, must round to even
	for (unsigned mode = 0; mode < 4U; ++mode) {
		initial.fpcr = (mode << 22) | (1U << 24) | (1U << 19) | 1U;
		const auto actual = test::run(test::convert_down(16, false),
			initial, harness);
		test::common::require(test::byte(actual, 0, 0) == 0x3cU);
	}
}

} // namespace

int main()
{
	ah_nan_sign_and_underflow_timing();
	scale_field_widths();
	fixed_rounding_ignores_rmode_and_flush_controls();
}
