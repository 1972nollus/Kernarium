// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_horizontal_test_support.hpp"

namespace test = arm64_floating_horizontal_test;

namespace {

constexpr test::Registers registers{0, 1, 2};

void add_pairwise_order_and_inactive_lanes(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.operation != test::Operation::add)
			return;
		auto initial = test::state(100U + seed++);
		initial.fpcr = 0U;
		initial.fpsr = test::basic::fpsr_input_denormal;
		const auto &format = test::basic::format_bits(form.width);
		const unsigned lanes = test::source_lanes(form);
		for (unsigned lane = 0; lane < lanes; ++lane) {
			test::set_lane_bits(initial, registers.first, lane,
				form.width, (lane & 1U) == 0U ?
					format.one : format.two);
			test::set_lane_bits(initial, registers.second, lane,
				form.width, (lane & 1U) == 0U ?
					format.four : format.one);
		}
		std::array<std::uint64_t, 8> results{};
		if (form.kind == test::Kind::scalar_pairwise) {
			results[0] = format.three;
		} else {
			for (unsigned lane = 0; lane < lanes / 2U; ++lane)
				results[lane] = format.three;
			for (unsigned lane = lanes / 2U; lane < lanes; ++lane)
				results[lane] = format.five;
		}
		const auto native = test::native_requirements(form);
		harness.check_one("FADDP pair adjacency source order and zeroing",
			test::instruction(form, registers), initial,
			test::expected_results(initial, form, registers,
				std::span{results}.first(
					test::result_lanes(form))),
			native);
		counts.add(native);
	});
}

void minmax_pairwise_order_and_nan_position(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.kind != test::Kind::scalar_pairwise &&
		    form.kind != test::Kind::vector_pairwise)
			return;
		if (form.operation == test::Operation::add)
			return;
		auto initial = test::state(500U + seed++);
		initial.fpcr = 0U;
		initial.fpsr = test::basic::fpsr_overflow;
		const auto &format = test::basic::format_bits(form.width);
		const unsigned lanes = test::source_lanes(form);
		for (unsigned lane = 0; lane < lanes; ++lane) {
			const auto first_bits = (lane & 3U) == 0U ?
				format.one :
				(lane & 3U) == 1U ? format.four :
				(lane & 3U) == 2U ? format.negative_zero :
				format.positive_zero;
			const auto second_bits = (lane & 3U) == 0U ?
				format.three :
				(lane & 3U) == 1U ? format.two :
				(lane & 3U) == 2U ? format.positive_zero :
				format.negative_zero;
			test::set_lane_bits(initial, registers.first, lane,
				form.width, first_bits);
			test::set_lane_bits(initial, registers.second, lane,
				form.width, second_bits);
		}
		/* A quiet value in the final pair distinguishes source
		 * concatenation and adjacent-pair ordering. */
		if (form.kind == test::Kind::vector_pairwise)
			test::set_lane_bits(initial, registers.second, lanes - 1U,
				form.width, format.quiet_nan);
		test::check_minmax(harness,
			"pairwise min/max concatenation order and lane pairing",
			form, registers, initial, counts);
	});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts expected;
	add_pairwise_order_and_inactive_lanes(harness, expected);
	minmax_pairwise_order_and_nan_position(harness, expected);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-floating-horizontal-pairwise");
}
