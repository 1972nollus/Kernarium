// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_absolute_minmax_test_support.hpp"

namespace test = arm64_floating_absolute_minmax_test;

namespace {

constexpr test::Registers registers{0, 1, 2};

void finite_magnitude_order_sign_clearing_and_inactive_lanes(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		auto initial = test::state(100U + seed++);
		initial.fpcr = 0U;
		initial.fpsr = test::basic::fpsr_inexact;
		const auto &format = test::basic::format_bits(form.width);
		const unsigned lanes =
			form.vector_bytes / (form.width / 8U);
		for (unsigned lane = 0; lane < lanes; ++lane) {
			const std::array first{
				format.three | format.sign,
				format.one,
				format.negative_zero,
				format.minimum_subnormal | format.sign,
				format.positive_infinity,
				format.two,
				format.one | format.sign,
				format.maximum_finite};
			const std::array second{
				format.two,
				format.four | format.sign,
				format.positive_zero,
				format.minimum_normal,
				format.maximum_finite | format.sign,
				format.two | format.sign,
				format.three,
				format.positive_infinity};
			test::set_lane_bits(initial, registers.first, lane,
				form.width, first[lane]);
			test::set_lane_bits(initial, registers.second, lane,
				form.width, second[lane]);
		}
		test::check(harness,
			"absolute minmax finite magnitude order positive results",
			form, registers, initial, counts);
	});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts expected;
	finite_magnitude_order_sign_clearing_and_inactive_lanes(
		harness, expected);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-floating-absolute-minmax-behavior");
}
