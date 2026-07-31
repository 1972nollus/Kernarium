// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_horizontal_test_support.hpp"

namespace test = arm64_floating_horizontal_test;

namespace {

constexpr test::Registers registers{0, 1, 2};

void absolute_difference_exact_results(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.operation != test::Operation::absolute_difference)
			return;
		auto initial = test::state(100U + seed++);
		initial.fpcr = 0U;
		initial.fpsr = test::basic::fpsr_underflow;
		const auto &format = test::basic::format_bits(form.width);
		std::array<std::uint64_t, 8> results{};
		for (unsigned lane = 0; lane < test::source_lanes(form); ++lane) {
			const bool reverse = (lane & 1U) != 0U;
			test::set_lane_bits(initial, registers.first, lane,
				form.width, reverse ? format.one :
					format.five_and_half);
			test::set_lane_bits(initial, registers.second, lane,
				form.width, reverse ? format.three :
					format.two_and_quarter);
			results[lane] = reverse ? format.two :
				format.three_and_quarter;
		}
		const auto native = test::native_requirements(form);
		harness.check_one("FABD exact lane results and inactive zeroing",
			test::instruction(form, registers), initial,
			test::expected_results(initial, form, registers,
				std::span{results}.first(
					test::result_lanes(form))),
			native);
		counts.add(native);
	});
}

void minmax_binary_finite_and_zero_results(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if ((form.kind != test::Kind::scalar_binary &&
		     form.kind != test::Kind::vector_binary) ||
		    form.operation == test::Operation::absolute_difference)
			return;
		auto initial = test::state(500U + seed++);
		initial.fpcr = 0U;
		initial.fpsr = test::basic::fpsr_inexact;
		const auto &format = test::basic::format_bits(form.width);
		for (unsigned lane = 0; lane < test::source_lanes(form); ++lane) {
			switch (lane & 3U) {
			case 0:
				test::set_lane_bits(initial, registers.first, lane,
					form.width, format.one);
				test::set_lane_bits(initial, registers.second, lane,
					form.width, format.three);
				break;
			case 1:
				test::set_lane_bits(initial, registers.first, lane,
					form.width, format.three | format.sign);
				test::set_lane_bits(initial, registers.second, lane,
					form.width, format.one | format.sign);
				break;
			case 2:
				test::set_lane_bits(initial, registers.first, lane,
					form.width, format.negative_zero);
				test::set_lane_bits(initial, registers.second, lane,
					form.width, format.positive_zero);
				break;
			default:
				test::set_lane_bits(initial, registers.first, lane,
					form.width, format.positive_infinity);
				test::set_lane_bits(initial, registers.second, lane,
					form.width, format.maximum_finite);
				break;
			}
		}
		test::check_minmax(harness,
			"binary min/max finite ordering signed zeros and inactive lanes",
			form, registers, initial, counts);
	});
}

void reported_scalar_double_encoding(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	const test::Form form{test::Operation::absolute_difference,
		test::Kind::scalar_binary, 64U, 16U};
	constexpr test::Registers reported{30, 15, 30};
	const auto encoded = test::instruction(form, reported);
	test::common::require(encoded == 0x7efe'd5feU);
	auto initial = test::state(0x74e0);
	initial.fpcr = 0U;
	initial.fpsr = 0U;
	test::set_lane_bits(initial, reported.first, 0, 64U,
		std::bit_cast<std::uint64_t>(-17.25));
	test::set_lane_bits(initial, reported.second, 0, 64U,
		std::bit_cast<std::uint64_t>(4.5));
	const auto native = test::native_requirements(form);
	harness.check_one("reported scalar double FABD encoding", encoded,
		initial, test::expected_uniform(initial, form, reported,
			std::bit_cast<std::uint64_t>(21.75)),
		native);
	counts.add(native);
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts expected;
	absolute_difference_exact_results(harness, expected);
	minmax_binary_finite_and_zero_results(harness, expected);
	reported_scalar_double_encoding(harness, expected);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-floating-horizontal-binary");
}
