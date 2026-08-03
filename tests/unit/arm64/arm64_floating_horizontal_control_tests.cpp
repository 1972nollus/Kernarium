// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_horizontal_test_support.hpp"

namespace test = arm64_floating_horizontal_test;

namespace {

constexpr test::Registers registers{0, 1, 2};

void prepare_pairs(test::arm64::CpuState &initial, const test::Form &form,
	std::uint64_t first, std::uint64_t second)
{
	const unsigned lanes = test::source_lanes(form);
	if (form.kind == test::Kind::scalar_binary ||
	    form.kind == test::Kind::vector_binary) {
		test::fill_lanes(initial, registers.first, lanes,
			form.width, first);
		test::fill_lanes(initial, registers.second, lanes,
			form.width, second);
		return;
	}
	for (const unsigned source : {registers.first, registers.second})
		for (unsigned lane = 0; lane < lanes; ++lane)
			test::set_lane_bits(initial, source, lane, form.width,
				(lane & 1U) == 0U ? first : second);
}

void nan_priority_defaulting_and_cumulative_flags(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.operation == test::Operation::absolute_difference ||
		    form.operation == test::Operation::add)
			return;
		const auto &format = test::basic::format_bits(form.width);
		auto initial = test::state(100U + seed++);
		initial.fpcr = 0U;
		initial.fpsr = test::basic::fpsr_underflow;
		prepare_pairs(initial, form, format.quiet_nan,
			format.signaling_nan | format.sign);
		test::check_minmax(harness,
			"min/max signaling priority quieting IOC and cumulative flags",
			form, registers, initial, counts);

		initial = test::state(1000U + seed++);
		initial.fpcr = 1U << 25;
		initial.fpsr = test::basic::fpsr_inexact;
		prepare_pairs(initial, form,
			format.quiet_nan | format.sign, format.quiet_nan);
		test::check_minmax(harness,
			"min/max DN canonical result and preserved cumulative flags",
			form, registers, initial, counts);
	});
}

void alternate_nan_zero_and_number_rules(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.operation == test::Operation::maximum ||
		    form.operation == test::Operation::minimum) {
			const auto &format = test::basic::format_bits(form.width);
			auto initial = test::state(3000U + seed++);
			initial.fpcr = 2U | (1U << 25);
			initial.fpsr = 0U;
			prepare_pairs(initial, form,
				format.quiet_nan | format.sign, format.three);
			test::check_minmax(harness,
				"AH min/max returns second for quiet input and sets IOC",
				form, registers, initial, counts);

			initial = test::state(4000U + seed++);
			initial.fpcr = 2U;
			initial.fpsr = test::basic::fpsr_overflow;
			prepare_pairs(initial, form,
				format.negative_zero, format.positive_zero);
			test::check_minmax(harness,
				"AH min/max returns second signed zero",
				form, registers, initial, counts);
		} else if (form.operation == test::Operation::maximum_number ||
			   form.operation == test::Operation::minimum_number) {
			const auto &format = test::basic::format_bits(form.width);
			auto initial = test::state(5000U + seed++);
			initial.fpcr = 2U;
			initial.fpsr = 0U;
			prepare_pairs(initial, form,
				format.quiet_nan | format.sign,
				format.signaling_nan);
			const auto native =
				test::native_requirements_for_state(
					form, initial.fpcr) |
				::uml::cpu::ArmFeature::afp;
			test::check_minmax(harness,
				"number min/max AH selects the first NaN and reports IOC",
				form, registers, initial, counts, native);
		}
	});
}

void scalar_binary_nep_merges_from_first_source(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.kind != test::Kind::scalar_binary)
			return;
		auto initial = test::state(7000U + seed++);
		initial.fpcr = 4U;
		initial.fpsr = test::basic::fpsr_underflow;
		const auto &format = test::basic::format_bits(form.width);
		test::fill_lanes(initial, registers.first,
			test::source_lanes(form), form.width, format.one);
		test::fill_lanes(initial, registers.second,
			test::source_lanes(form), form.width, format.one);
		if (form.operation == test::Operation::absolute_difference)
			test::check_uniform(harness,
				"scalar FABD NEP merges inactive bits from first source",
				form, registers, initial, format.positive_zero,
				counts);
		else
			test::check_minmax(harness,
				"scalar min/max NEP merges inactive bits from first source",
				form, registers, initial, counts);
	});
}

void flush_controls_and_denormal_flags(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.operation != test::Operation::absolute_difference &&
		    form.operation != test::Operation::add)
			return;
		const auto &format = test::basic::format_bits(form.width);
		auto initial = test::state(9000U + seed++);
		initial.fpcr = form.width == 16U ?
			1U << 19 : 1U << 24;
		initial.fpsr = 0U;
		test::fill_lanes(initial, registers.first,
			test::source_lanes(form), form.width,
			format.positive_zero);
		test::fill_lanes(initial, registers.second,
			test::source_lanes(form), form.width,
			format.positive_zero);
		test::set_lane_bits(initial, registers.first, 0,
			form.width, format.minimum_subnormal);
		const auto flags = form.width == 16U ? 0U :
			test::basic::fpsr_input_denormal;
		test::check_uniform(harness,
			"arithmetic input flush controls and IDC",
			form, registers, initial, format.positive_zero,
			counts, flags);
	});

	test::for_each_form([&](const test::Form &form) {
		if (form.operation != test::Operation::maximum &&
		     form.operation != test::Operation::maximum_number &&
		     form.operation != test::Operation::minimum &&
		     form.operation != test::Operation::minimum_number)
			return;
		const auto &format = test::basic::format_bits(form.width);
		auto initial = test::state(12'000U + seed++);
		initial.fpcr = form.width == 16U ?
			1U << 19 : 1U << 24;
		initial.fpsr = 0U;
		prepare_pairs(initial, form,
			format.minimum_subnormal, format.one);
		test::check_minmax(harness,
			"min/max legacy input flush and IDC rules",
			form, registers, initial, counts);

		if (form.width != 16U) {
			initial = test::state(14'000U + seed++);
			initial.fpcr = 2U | (1U << 24);
			initial.fpsr = 0U;
			prepare_pairs(initial, form,
				format.minimum_subnormal, format.one);
			auto native =
				test::native_requirements_for_state(
					form, initial.fpcr);
			if (form.operation ==
					test::Operation::maximum_number ||
			    form.operation ==
					test::Operation::minimum_number)
				native = native |
					::uml::cpu::ArmFeature::afp;
			const bool maximum =
				form.operation == test::Operation::maximum ||
				form.operation ==
					test::Operation::maximum_number;
			const bool number_minimum =
				form.operation ==
					test::Operation::minimum_number;
			const auto result = maximum ? format.one :
				number_minimum ? format.positive_zero :
				format.minimum_subnormal;
			const auto flags =
				test::basic::fpsr_input_denormal |
				(number_minimum ?
					test::basic::fpsr_underflow |
						test::basic::fpsr_inexact :
					0U);
			test::check_uniform(harness,
				"AH min/max denormal and output-flush rules",
				form, registers, initial, result, counts,
				flags, native);
		}
	});

	test::for_each_form([&](const test::Form &form) {
		if (form.width == 16U ||
		    (form.operation != test::Operation::maximum_number &&
		     form.operation != test::Operation::minimum_number))
			return;
		const auto &format = test::basic::format_bits(form.width);
		auto initial = test::state(15'000U + seed++);
		initial.fpcr = 2U;
		initial.fpsr = test::basic::fpsr_overflow;
		prepare_pairs(initial, form, format.signaling_nan,
			format.minimum_subnormal);
		test::check_minmax(harness,
			"number min/max early NaN suppresses deferred IDC",
			form, registers, initial, counts);
	});

	const test::Form fiz_form{test::Operation::minimum,
		test::Kind::vector_binary, 32U, 16U};
	auto fiz = test::state(16'000U);
	fiz.fpcr = 1U;
	fiz.fpsr = 0U;
	const auto &single = test::basic::single_bits;
	prepare_pairs(fiz, fiz_form, single.minimum_subnormal, single.one);
	test::check_minmax(harness,
		"FIZ flushes input without IDC", fiz_form, registers, fiz, counts);

	fiz.fpcr = 1U | (1U << 24);
	fiz.fpsr = 0U;
	test::check_minmax(harness,
		"legacy FZ still reports IDC when FIZ is also set",
		fiz_form, registers, fiz, counts);
}

void arithmetic_rounding_nan_and_exception_flags(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	const test::Form add{test::Operation::add,
		test::Kind::scalar_pairwise, 32U, 16U};
	auto initial = test::state(20'000U);
	initial.fpcr = 0U;
	initial.fpsr = 0U;
	test::set_lane_bits(initial, registers.first, 0, 32U,
		test::basic::single_bits.one);
	test::set_lane_bits(initial, registers.first, 1, 32U,
		test::basic::single_bits.half_ulp);
	test::check_uniform(harness,
		"FADDP round-nearest tie and IXC", add, registers, initial,
		test::basic::single_bits.one, counts,
		test::basic::fpsr_inexact);

	initial.fpcr = 1U << 22;
	test::check_uniform(harness,
		"FADDP round-plus tie", add, registers, initial,
		test::basic::single_bits.one_next, counts,
		test::basic::fpsr_inexact);

	initial = test::state(20'001U);
	initial.fpcr = 0U;
	initial.fpsr = test::basic::fpsr_underflow;
	test::fill_lanes(initial, registers.first, 2U, 32U,
		test::basic::single_bits.maximum_finite);
	test::check_uniform(harness,
		"FADDP overflow sets OFC and IXC cumulatively",
		add, registers, initial,
		test::basic::single_bits.positive_infinity, counts,
		test::basic::fpsr_overflow | test::basic::fpsr_inexact);

	for (const auto width : test::basic::widths) {
		const test::Form difference{
			test::Operation::absolute_difference,
			test::Kind::scalar_binary, width, 16U};
		const auto &format = test::basic::format_bits(width);
		const std::uint64_t quieted = format.signaling_nan |
			(width == 16U ?
				test::float_layout<std::uint16_t>().quiet_bit :
			 width == 32U ?
				test::float_layout<std::uint32_t>().quiet_bit :
				test::float_layout<std::uint64_t>().quiet_bit);
		auto nan = test::state(21'000U + width);
		nan.fpcr = 0U;
		nan.fpsr = 0U;
		test::set_lane_bits(nan, registers.first, 0, width,
			format.signaling_nan | format.sign);
		test::set_lane_bits(nan, registers.second, 0, width,
			format.one);
		test::check_uniform(harness,
			"FABD quiets signaling input clears sign and sets IOC",
			difference, registers, nan, quieted, counts,
			test::basic::fpsr_invalid);

		nan.fpcr = 2U;
		test::check_uniform(harness,
			"FABD AH preserves exceptional result sign",
			difference, registers, nan, quieted | format.sign,
			counts, test::basic::fpsr_invalid);

		nan.fpcr = 1U << 25;
		test::check_uniform(harness,
			"FABD DN returns canonical result",
			difference, registers, nan, format.default_nan,
			counts, test::basic::fpsr_invalid);
	}

	const auto &single = test::basic::single_bits;
	auto alternate_add = test::state(22'000U);
	alternate_add.fpcr = 2U;
	alternate_add.fpsr = test::basic::fpsr_underflow;
	test::set_lane_bits(alternate_add, registers.first, 0, 32U,
		single.quiet_nan | single.sign);
	test::set_lane_bits(alternate_add, registers.first, 1, 32U,
		single.signaling_nan);
	test::check_uniform(harness,
		"FADDP AH selects the first NaN and accumulates IOC",
		add, registers, alternate_add,
		single.quiet_nan | single.sign, counts,
		test::basic::fpsr_invalid);

	alternate_add.fpcr |= 1U << 25;
	test::check_uniform(harness,
		"FADDP AH still honors default-NaN control",
		add, registers, alternate_add, single.default_nan, counts,
		test::basic::fpsr_invalid);
}

void arithmetic_input_controls(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	const std::array forms{
		test::Form{test::Operation::absolute_difference,
			test::Kind::scalar_binary, 32U, 16U},
		test::Form{test::Operation::add,
			test::Kind::scalar_pairwise, 32U, 16U}};
	for (const auto &form : forms) {
		auto initial = test::state(
			23'000U + static_cast<unsigned>(form.operation));
		initial.fpcr = 1U;
		initial.fpsr = 0U;
		prepare_pairs(initial, form,
			test::basic::single_bits.minimum_subnormal,
			test::basic::single_bits.positive_zero);
		test::check_uniform(harness,
			"arithmetic FIZ flushes inputs without IDC",
			form, registers, initial,
			test::basic::single_bits.positive_zero, counts);

		initial.fpcr = 2U;
		test::check_uniform(harness,
			"arithmetic AH preserves denormal inputs and reports IDC",
			form, registers, initial,
			test::basic::single_bits.minimum_subnormal, counts,
			test::basic::fpsr_input_denormal);

		initial.fpcr |= 1U << 24;
		test::check_uniform(harness,
			"arithmetic AH applies FZ after rounding",
			form, registers, initial,
			test::basic::single_bits.positive_zero, counts,
			test::basic::fpsr_input_denormal |
				test::basic::fpsr_underflow |
				test::basic::fpsr_inexact);
	}

	const test::Form half{
		test::Operation::absolute_difference,
		test::Kind::vector_binary, 16U, 16U};
	auto initial = test::state(23'100U);
	initial.fpcr = 1U;
	initial.fpsr = 0U;
	prepare_pairs(initial, half,
		test::basic::half_bits.one,
		test::basic::half_bits.positive_zero);
	test::check_uniform(harness,
		"half-precision arithmetic ignores FIZ without skipping native JIT",
		half, registers, initial, test::basic::half_bits.one, counts);

	unsigned half_seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.width != 16U ||
		    (form.operation != test::Operation::absolute_difference &&
		     form.operation != test::Operation::add))
			return;
		auto flushed = test::state(23'200U + half_seed++);
		flushed.fpcr = 2U | (1U << 19);
		flushed.fpsr = test::basic::fpsr_overflow;
		prepare_pairs(flushed, form,
			test::basic::half_bits.minimum_subnormal,
			test::basic::half_bits.positive_zero);
		test::check_uniform(harness,
			"half AH still applies FZ16 while unpacking inputs",
			form, registers, flushed,
			test::basic::half_bits.positive_zero, counts);
	});
	test::common::require(half_seed == 6U);

	const test::Form half_scalar{
		test::Operation::absolute_difference,
		test::Kind::scalar_binary, 16U, 16U};
	initial = test::state(23'101U);
	initial.fpcr = 2U | (1U << 19);
	initial.fpsr = 0U;
	prepare_pairs(initial, half_scalar,
		test::basic::half_bits.minimum_normal + 1U,
		test::basic::half_bits.minimum_normal);
	test::check_uniform(harness,
		"half-precision AH applies FZ16 after rounding",
		half_scalar, registers, initial,
		test::basic::half_bits.positive_zero, counts,
		test::basic::fpsr_underflow | test::basic::fpsr_inexact);
}

void irrelevant_control_bits_do_not_over_skip_jit(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.kind != test::Kind::scalar_pairwise)
			return;
		auto pairwise = test::state(29'000U + seed++);
		pairwise.fpcr = 4U;
		pairwise.fpsr = test::basic::fpsr_underflow;
		const auto &format = test::basic::format_bits(form.width);
		prepare_pairs(pairwise, form, format.one, format.one);
		test::common::require(
			test::native_requirements_for_state(form, pairwise.fpcr).
				features() ==
			test::native_requirements(form).features());
		if (form.operation == test::Operation::add)
			test::check_uniform(harness,
				"pairwise scalar NEP is ignored and inactive bits are zero",
				form, registers, pairwise, format.two, counts);
		else
			test::check_minmax(harness,
				"pairwise scalar NEP is ignored without skipping native JIT",
				form, registers, pairwise, counts);
	});

	const test::Form vector{test::Operation::maximum,
		test::Kind::vector_binary, 32U, 16U};
	auto initial = test::state(30'000U);
	initial.fpcr = 4U;
	initial.fpsr = 0U;
	prepare_pairs(initial, vector,
		test::basic::single_bits.one, test::basic::single_bits.two);
	const auto allocation = test::native_requirements(vector);
	const auto exact =
		test::native_requirements_for_state(vector, initial.fpcr);
	test::common::require(exact.features() == allocation.features());
	test::check_minmax(harness,
		"vector NEP bit is irrelevant and must not skip native JIT",
		vector, registers, initial, counts);

	const test::Form number{test::Operation::maximum_number,
		test::Kind::scalar_binary, 32U, 16U};
	initial = test::state(30'001U);
	initial.fpcr = 2U;
	initial.fpsr = 0U;
	prepare_pairs(initial, number,
		test::basic::single_bits.one, test::basic::single_bits.two);
	test::common::require(
		test::native_requirements_for_state(number, initial.fpcr).
			features() ==
		test::native_requirements(number).features());
	test::check_minmax(harness,
		"number min/max AH bit does not add an AFP requirement",
		number, registers, initial, counts);

	const test::Form half{test::Operation::absolute_difference,
		test::Kind::vector_binary, 16U, 16U};
	test::common::require(
		test::native_requirements_for_state(half, 1U).features() ==
		test::native_requirements(half).features());
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts expected;
	nan_priority_defaulting_and_cumulative_flags(harness, expected);
	alternate_nan_zero_and_number_rules(harness, expected);
	scalar_binary_nep_merges_from_first_source(harness, expected);
	flush_controls_and_denormal_flags(harness, expected);
	arithmetic_rounding_nan_and_exception_flags(harness, expected);
	arithmetic_input_controls(harness, expected);
	irrelevant_control_bits_do_not_over_skip_jit(harness, expected);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-floating-horizontal-control");
}
