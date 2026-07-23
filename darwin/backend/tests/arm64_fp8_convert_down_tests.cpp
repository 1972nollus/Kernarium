// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_test_support.hpp"

namespace test = arm64_fp8_test;

namespace {

void placement_and_exact_values()
{
	test::common::Harness harness;
	for (const unsigned width : {16U, 32U})
		for (const bool upper : {false, true}) {
			auto initial = test::common::state(width + upper);
			initial.fpmr = 0;
			const unsigned lanes = width == 16U ? (upper ? 8U : 4U) : 4U;
			for (unsigned lane = 0; lane < lanes; ++lane) {
				test::set_lane(initial, 1, lane, width,
					width == 16U ? 0x3c00U : 0x3f80'0000U);
				test::set_lane(initial, 2, lane, width,
					width == 16U ? 0xc000U : 0xc000'0000U);
			}
			const auto actual = test::run(test::convert_down(width, upper),
				initial, harness);
			const unsigned output = width == 32U && upper ? 8U : 0U;
			for (unsigned lane = 0; lane < lanes; ++lane) {
				test::common::require(test::byte(actual, 0, output + lane) == 0x3cU);
				test::common::require(test::byte(actual, 0,
					output + lanes + lane) == 0xc0U);
			}
			if (width == 32U && !upper)
				for (unsigned byte = 8U; byte < 16U; ++byte)
					test::common::require(test::byte(actual, 0, byte) == 0U);
		}
}

void nearest_even_and_scale()
{
	test::common::Harness harness;
	auto initial = test::common::state(52);
	initial.fpmr = 0;
	initial.fpsr = 0;
	test::set_lane(initial, 1, 0, 16, 0x3c80); // 1.125 -> 1.0 E5M2
	test::set_lane(initial, 1, 1, 16, 0x3d80); // 1.375 -> 1.5 E5M2
	const auto e5 = test::run(test::convert_down(16, false), initial, harness);
	test::common::require(test::byte(e5, 0, 0) == 0x3cU);
	test::common::require(test::byte(e5, 0, 1) == 0x3eU);
	test::common::require((e5.fpsr & 16U) != 0);

	initial.fpmr = test::fpmr(0, 0, 1);
	initial.fpsr = 0;
	test::set_lane(initial, 1, 0, 16, 0x3c40); // 1.0625 -> 1.0 E4M3
	test::set_lane(initial, 1, 1, 16, 0x3cc0); // 1.1875 -> 1.25
	const auto e4 = test::run(test::convert_down(16, false), initial, harness);
	test::common::require(test::byte(e4, 0, 0) == 0x38U);
	test::common::require(test::byte(e4, 0, 1) == 0x3aU);

	initial.fpmr = test::fpmr(0, 0, 0, 0, 1);
	initial.fpsr = 0;
	test::set_lane(initial, 1, 0, 16, 0x3c00);
	const auto scaled = test::run(test::convert_down(16, false), initial, harness);
	test::common::require(test::byte(scaled, 0, 0) == 0x40U); // 2.0
}

void special_values_saturation_and_flags()
{
	test::common::Harness harness;
	auto initial = test::common::state(61);
	initial.fpmr = 0;
	initial.fpsr = 0;
	test::set_lane(initial, 1, 0, 16, 0x7d01); // SNaN
	test::set_lane(initial, 1, 1, 16, 0x7e01); // QNaN
	test::set_lane(initial, 1, 2, 16, 0x7c00); // infinity
	const auto special = test::run(test::convert_down(16, false), initial, harness);
	test::common::require(test::byte(special, 0, 0) == 0x7eU);
	test::common::require(test::byte(special, 0, 1) == 0x7eU);
	test::common::require(test::byte(special, 0, 2) == 0x7cU);
	test::common::require((special.fpsr & 1U) != 0);

	initial.fpmr = test::fpmr(0, 0, 0, 0, 0, false, false);
	initial.fpsr = 0;
	test::set_lane(initial, 1, 0, 16, 0x7bff);
	const auto overflow = test::run(test::convert_down(16, false), initial, harness);
	test::common::require(test::byte(overflow, 0, 0) == 0x7cU);
	test::common::require((overflow.fpsr & 0x14U) == 0x14U);

	initial.fpmr = test::fpmr(0, 0, 0, 0, 0, false, true);
	initial.fpsr = 0;
	const auto saturated = test::run(test::convert_down(16, false), initial, harness);
	test::common::require(test::byte(saturated, 0, 0) == 0x7bU);
	test::common::require((saturated.fpsr & 0x14U) == 0x14U);

	initial.fpmr = test::fpmr(0, 0, 2);
	initial.fpsr = 0;
	const auto unsupported = test::run(test::convert_down(16, false), initial, harness);
	for (unsigned byte = 0; byte < 8U; ++byte)
		test::common::require(test::byte(unsupported, 0, byte) == 0xffU);
	test::common::require((unsupported.fpsr & 1U) != 0);
}

} // namespace

int main()
{
	placement_and_exact_values();
	nearest_even_and_scale();
	special_values_saturation_and_flags();
}
