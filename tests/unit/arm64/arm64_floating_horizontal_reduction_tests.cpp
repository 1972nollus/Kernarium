// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_horizontal_test_support.hpp"

namespace test = arm64_floating_horizontal_test;

namespace {

constexpr test::Registers registers{0, 1, 2};

void every_reduction_uses_balanced_pair_order(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.kind != test::Kind::reduction)
			return;
		auto initial = test::state(100U + seed++);
		initial.fpcr = 0U;
		initial.fpsr = test::basic::fpsr_underflow;
		const auto &format = test::basic::format_bits(form.width);
		const unsigned lanes = test::source_lanes(form);
		for (unsigned lane = 0; lane < lanes; ++lane) {
			const std::array values{
				format.one, format.four,
				format.three | format.sign, format.two,
				format.negative_zero, format.positive_zero,
				format.maximum_finite, format.one | format.sign};
			test::set_lane_bits(initial, registers.first, lane,
				form.width, values[lane]);
		}
		test::check_minmax(harness,
			"across-vector reduction balanced order and scalar zeroing",
			form, registers, initial, counts);
	});
}

void alternate_reduction_order_with_nan_and_zero_pairs(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.kind != test::Kind::reduction ||
		    (form.operation != test::Operation::maximum &&
		     form.operation != test::Operation::minimum))
			return;
		auto initial = test::state(500U + seed++);
		initial.fpcr = 2U;
		initial.fpsr = 0U;
		const auto &format = test::basic::format_bits(form.width);
		const unsigned lanes = test::source_lanes(form);
		for (unsigned lane = 0; lane < lanes; ++lane) {
			const auto bits = lane == 0U ? format.quiet_nan :
				lane == 1U ? format.one :
				(lane & 1U) == 0U ? format.negative_zero :
					format.positive_zero;
			test::set_lane_bits(initial, registers.first, lane,
				form.width, bits);
		}
		test::check_minmax(harness,
			"alternate reduction follows recursive adjacent pair order",
			form, registers, initial, counts);
	});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts expected;
	every_reduction_uses_balanced_pair_order(harness, expected);
	alternate_reduction_order_with_nan_and_zero_pairs(harness, expected);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-floating-horizontal-reduction");
}
