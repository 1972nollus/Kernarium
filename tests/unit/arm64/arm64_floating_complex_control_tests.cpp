// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_complex_test_support.hpp"

namespace test = arm64_floating_complex_test;

namespace {

constexpr test::Registers registers{0, 1, 2};

void clear_operands(uml::cpu::arm64::CpuState &cpu,
	const test::Form &form)
{
	const auto zero = test::basic::format_bits(form.width).positive_zero;
	for (const unsigned reg : {
		registers.destination, registers.first, registers.second,
	})
		for (unsigned lane = 0; lane < 128U / form.width; ++lane)
			test::set_lane_bits(cpu, reg, lane, form.width, zero);
}

void prepare_rounding_tie(uml::cpu::arm64::CpuState &cpu,
	const test::Form &form)
{
	clear_operands(cpu, form);
	const auto &format = test::basic::format_bits(form.width);
	const unsigned pairs = test::active_lanes(form) / 2U;
	if (form.operation == test::Operation::add) {
		for (unsigned pair = 0; pair < pairs; ++pair) {
			test::set_lane_bits(cpu, registers.first, pair * 2U,
				form.width, format.one);
			test::set_lane_bits(cpu, registers.second,
				pair * 2U + 1U, form.width,
				format.half_ulp |
				(form.rotation == 1U ? format.sign : 0U));
		}
		return;
	}

	for (unsigned pair = 0; pair < pairs; ++pair) {
		test::set_lane_bits(cpu, registers.destination, pair * 2U,
			form.width, format.one);
		const bool imaginary_first =
			form.rotation == 1U || form.rotation == 3U;
		test::set_lane_bits(cpu, registers.first,
			pair * 2U + static_cast<unsigned>(imaginary_first),
			form.width, format.one);
	}
	const unsigned source_pair = form.by_element ? form.index : 0U;
	const bool imaginary_second =
		form.rotation == 1U || form.rotation == 3U;
	const bool negative_second =
		form.rotation == 1U || form.rotation == 2U;
	test::set_lane_bits(cpu, registers.second,
		source_pair * 2U + static_cast<unsigned>(imaginary_second),
		form.width,
		format.half_ulp |
			(negative_second ? format.sign : 0U));
	if (!form.by_element)
		for (unsigned pair = 1; pair < pairs; ++pair)
			test::set_lane_bits(cpu, registers.second,
				pair * 2U +
					static_cast<unsigned>(imaginary_second),
				form.width,
				format.half_ulp |
					(negative_second ?
						format.sign : 0U));
}

void every_rounding_mode_for_every_form(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned form_index{};
	test::for_each_form([&](const test::Form &form) {
		const auto &format = test::basic::format_bits(form.width);
		for (unsigned rounding_mode = 0;
		     rounding_mode < 4U; ++rounding_mode) {
			auto initial = test::state(
				1000U + form_index * 7U + rounding_mode);
			initial.fpcr = rounding_mode << 22;
			initial.fpsr = 1U << 27;
			prepare_rounding_tie(initial, form);
			std::array<std::uint64_t, 8> results{};
			for (unsigned pair = 0;
			     pair < test::active_lanes(form) / 2U; ++pair) {
				results[pair * 2U] = format.one +
					(rounding_mode == 1U ? 1U : 0U);
				const bool negative_zero =
					rounding_mode == 2U &&
					(form.rotation == 3U ||
					 (form.operation ==
					  test::Operation::
						  multiply_accumulate &&
					  form.rotation == 2U));
				results[pair * 2U + 1U] =
					negative_zero ?
						format.negative_zero :
						format.positive_zero;
			}
			auto expected = test::expected_lanes(
				initial, form, registers,
				std::span<const std::uint64_t>{results}.first(
					test::active_lanes(form)),
				test::basic::fpsr_inexact);
			char name[176];
			std::snprintf(name, sizeof(name),
				"complex rounding form=%u mode=%u",
				form_index, rounding_mode);
			test::check(harness, counts, name, form, registers,
				initial, expected);
		}
		++form_index;
	});
	test::common::require(form_index == 62U);
}

void every_form_default_result_and_invalid_status(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned form_index{};
	test::for_each_form([&](const test::Form &form) {
		const auto &format = test::basic::format_bits(form.width);
		for (const bool signaling : {false, true}) {
			auto initial = test::state(
				3000U + form_index * 3U + signaling);
			initial.fpcr = 1U << 25;
			initial.fpsr = 1U << 27;
			clear_operands(initial, form);
			test::fill_active_lanes(initial, registers.first, form,
				signaling ? format.signaling_nan :
					format.quiet_nan);
			for (unsigned lane = 0;
			     lane < 128U / form.width; ++lane)
				test::set_lane_bits(initial, registers.second,
					lane, form.width, format.one);
			const auto expected = test::expected_uniform(
				initial, form, registers, format.default_nan,
				signaling ? test::basic::fpsr_invalid : 0U);
			char name[176];
			std::snprintf(name, sizeof(name),
				"complex default-result form=%u signaling=%u",
				form_index, signaling);
			test::check(harness, counts, name, form, registers,
				initial, expected);
		}
		++form_index;
	});
}

void output_flushing_for_every_multiply_form(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned form_index{};
	test::for_each_form([&](const test::Form &form) {
		if (form.operation !=
		    test::Operation::multiply_accumulate)
			return;
		const auto &format = test::basic::format_bits(form.width);
		auto initial = test::state(5000U + form_index);
		initial.fpcr = form.width == 16U ?
			1U << 19 : 1U << 24;
		initial.fpsr = 1U << 27;
		clear_operands(initial, form);
		test::fill_active_lanes(initial, registers.first, form,
			format.minimum_normal);
		for (unsigned lane = 0; lane < 128U / form.width; ++lane)
			test::set_lane_bits(initial, registers.second, lane,
				form.width, format.half);

		std::array<std::uint64_t, 8> results{};
		for (unsigned pair = 0;
		     pair < test::active_lanes(form) / 2U; ++pair) {
			results[pair * 2U] =
				(form.rotation == 1U ||
				 form.rotation == 2U) ?
					format.negative_zero :
					format.positive_zero;
			results[pair * 2U + 1U] =
				(form.rotation == 2U ||
				 form.rotation == 3U) ?
					format.negative_zero :
					format.positive_zero;
		}
		const auto expected = test::expected_lanes(
			initial, form, registers,
			std::span<const std::uint64_t>{results}.first(
				test::active_lanes(form)),
			test::basic::fpsr_underflow);
		char name[176];
		std::snprintf(name, sizeof(name),
			"complex output flush form=%u", form_index);
		test::check(harness, counts, name, form, registers,
			initial, expected);
		++form_index;
	});
	test::common::require(form_index == 52U);
}

void input_flushing_for_every_add_form(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned form_index{};
	test::for_each_form([&](const test::Form &form) {
		if (form.operation != test::Operation::add)
			return;
		const auto &format = test::basic::format_bits(form.width);
		auto initial = test::state(7000U + form_index);
		initial.fpcr = form.width == 16U ?
			1U << 19 : 1U << 24;
		initial.fpsr = 1U << 27;
		clear_operands(initial, form);
		test::fill_active_lanes(initial, registers.second, form,
			format.minimum_subnormal);
		test::check(harness, counts,
			"complex input flush sets cumulative input status",
			form, registers, initial,
			test::expected_uniform(initial, form, registers,
				format.positive_zero,
				form.width == 16U ? 0U :
					test::basic::fpsr_input_denormal));
		++form_index;
	});
	test::common::require(form_index == 10U);
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts expected;
	every_rounding_mode_for_every_form(harness, expected);
	every_form_default_result_and_invalid_status(harness, expected);
	output_flushing_for_every_multiply_form(harness, expected);
	input_flushing_for_every_add_form(harness, expected);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-floating-complex-control");
}
