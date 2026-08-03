// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_absolute_minmax_test_support.hpp"

namespace test = arm64_floating_absolute_minmax_test;

namespace {

constexpr test::Registers registers{0, 1, 2};

void fill_uniform(test::arm64::CpuState &initial, const test::Form &form,
	std::uint64_t first, std::uint64_t second)
{
	const unsigned lanes = form.vector_bytes / (form.width / 8U);
	test::horizontal::fill_lanes(initial, registers.first, lanes,
		form.width, first);
	test::horizontal::fill_lanes(initial, registers.second, lanes,
		form.width, second);
}

void nan_priority_defaulting_and_ioc(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		const auto &format = test::basic::format_bits(form.width);
		auto initial = test::state(100U + seed++);
		initial.fpcr = 0U;
		initial.fpsr = test::basic::fpsr_inexact;
		fill_uniform(initial, form,
			format.quiet_nan | format.sign,
			format.signaling_nan);
		test::check(harness,
			"absolute minmax signaling priority quieting and IOC",
			form, registers, initial, counts);

		initial = test::state(500U + seed++);
		initial.fpcr = 1U << 25;
		initial.fpsr = test::basic::fpsr_underflow;
		fill_uniform(initial, form,
			format.signaling_nan | format.sign, format.one);
		test::check(harness,
			"absolute minmax DN positive canonical result",
			form, registers, initial, counts);

		initial = test::state(900U + seed++);
		initial.fpcr = 0U;
		initial.fpsr = test::basic::fpsr_overflow;
		fill_uniform(initial, form,
			format.quiet_nan | format.sign, format.one);
		test::check(harness,
			"absolute minmax quiet payload and sign propagation",
			form, registers, initial, counts);
	});
}

void alternate_and_flush_controls_are_ignored(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		const auto &format = test::basic::format_bits(form.width);
		auto initial = test::state(2000U + seed++);
		initial.fpcr = 3U | (1U << 19) | (1U << 24);
		initial.fpsr = 0U;
		fill_uniform(initial, form,
			format.minimum_subnormal | format.sign,
			format.minimum_subnormal);
		test::check(harness,
			"absolute minmax ignores AH FIZ FZ FZ16 and IDC",
			form, registers, initial, counts);
	});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts expected;
	nan_priority_defaulting_and_ioc(harness, expected);
	alternate_and_flush_controls_are_ignored(harness, expected);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-floating-absolute-minmax-control");
}
