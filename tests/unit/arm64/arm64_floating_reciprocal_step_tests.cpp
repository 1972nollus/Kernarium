// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_reciprocal_test_support.hpp"

#include <array>

namespace test = arm64_floating_reciprocal_test;

namespace {

[[nodiscard]] std::array<std::uint64_t, 16> values(unsigned width)
{
	const auto value = test::format(width);
	const auto sign = test::sign_mask(value);
	const auto minimum_normal =
		test::normal_bits(value, 1U, 0U);
	const auto one =
		test::normal_bits(value, static_cast<unsigned>(value.bias), 0U);
	const auto two =
		test::normal_bits(value,
			static_cast<unsigned>(value.bias + 1), 0U);
	const auto infinity = test::infinity_bits(value);
	const auto quiet =
		infinity |
		(std::uint64_t{1} << (value.fraction_bits - 1U)) | 0x55U;
	const auto signaling = infinity | 0x55U;
	return {
		std::uint64_t{0},
		sign,
		std::uint64_t{1},
		sign | 1U,
		test::fraction_mask(value),
		minimum_normal,
		sign | minimum_normal,
		one,
		sign | one,
		one | 1U,
		two | (std::uint64_t{1} << (value.fraction_bits - 2U)),
		test::maximum_finite_bits(value),
		infinity,
		sign | infinity,
		quiet,
		signaling,
	};
}

template<typename Function>
void for_each_step_form(Function &&function)
{
	test::for_each_form([&](const test::Form &form) {
		if (form.operation == test::Operation::reciprocal_step ||
		    form.operation == test::Operation::reciprocal_sqrt_step)
			function(form);
	});
}

void special_value_matrix(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 0;
	for_each_step_form([&](const test::Form &form) {
		const auto inputs = values(form.width);
		for (unsigned first_case = 0; first_case < inputs.size();
		     ++first_case)
			for (unsigned second_case = 0;
			     second_case < inputs.size(); ++second_case) {
				const auto registers = test::register_cases[
					(first_case * inputs.size() + second_case) %
					test::register_cases.size()];
				auto initial = test::state(++seed);
				for (unsigned lane = 0;
				     lane < test::active_lanes(form); ++lane) {
					test::write_lane(
						initial.vectors[registers.first].bytes,
						lane, form.width,
						inputs[(first_case + lane) %
							inputs.size()]);
					test::write_lane(
						initial.vectors[registers.second].bytes,
						lane, form.width,
						inputs[(second_case + lane * 3U) %
							inputs.size()]);
				}
				test::check_one(
					harness, form, registers, initial);
			}
	});
}

void every_rounding_direction(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 10'000;
	constexpr test::Registers registers{0, 1, 2};
	for_each_step_form([&](const test::Form &form) {
		const auto value = test::format(form.width);
		const auto one =
			test::normal_bits(value,
				static_cast<unsigned>(value.bias),
				(test::fraction_mask(value) * 3U) / 11U);
		const auto other =
			test::normal_bits(value,
				static_cast<unsigned>(value.bias - 1),
				(test::fraction_mask(value) * 5U) / 13U);
		for (unsigned rounding = 0; rounding < 4U; ++rounding)
			for (const bool negate_first : {false, true}) {
				auto initial = test::state(++seed);
				initial.fpcr = rounding << 22U;
				for (unsigned lane = 0;
				     lane < test::active_lanes(form); ++lane) {
					test::write_lane(
						initial.vectors[registers.first].bytes,
						lane, form.width,
						one |
							(negate_first ?
							 test::sign_mask(value) :
							 0U));
					test::write_lane(
						initial.vectors[registers.second].bytes,
						lane, form.width,
						other ^ (lane == 0U ? 0U :
							test::sign_mask(value)));
				}
				test::check_one(
					harness, form, registers, initial);
			}
	});
}

[[nodiscard]] test::ExpectedEngineCounts expected_counts()
{
	test::ExpectedEngineCounts expected;
	for_each_step_form([&](const test::Form &form) {
		constexpr std::size_t special_pairs = 16U * 16U;
		constexpr std::size_t rounding_cases = 4U * 2U;
		expected.add(form, 0U, special_pairs + rounding_cases);
	});
	test::require(expected.total == 4'224U,
		"step formula omitted a behavior case");
	return expected;
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	special_value_matrix(harness);
	every_rounding_direction(harness);
	test::require_engine_counts(harness, expected_counts());
	harness.print_summary("arm64-floating-reciprocal-step");
	return 0;
}
