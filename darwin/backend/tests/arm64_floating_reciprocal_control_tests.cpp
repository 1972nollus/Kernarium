// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_reciprocal_test_support.hpp"

#include <cmath>
#include <limits>

namespace test = arm64_floating_reciprocal_test;

namespace {

constexpr test::Registers registers{0, 1, 2};

template<typename Floating>
void check_nep(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	auto initial = test::common::state(seed);
	initial.fpcr = 4U;
	test::basic::set_lane(initial, registers.first, 0,
		test::first_input<Floating>(form.operation));
	test::basic::set_lane(initial, registers.second, 0,
		test::second_input<Floating>(form.operation));
	const auto upper = (test::unary(form.operation) ?
		initial.vectors[registers.destination] :
		initial.vectors[registers.first]).bytes;
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	for (unsigned byte = sizeof(Floating); byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			upper[byte]);
	harness.require_differential(test::instruction(form, registers), initial);
}

void scalar_nep_uses_architectural_merge_source()
{
	test::common::Harness harness;
	unsigned seed = 200;
	test::for_each_form([&](const test::Form &form) {
		if (!form.scalar)
			return;
		if (form.width == 16U)
			check_nep<_Float16>(harness, form, ++seed);
		else if (form.width == 32U)
			check_nep<float>(harness, form, ++seed);
		else
			check_nep<double>(harness, form, ++seed);
	});
}

void zero_negative_and_nan_exception_rules()
{
	test::common::Harness harness;
	auto initial = test::common::state(300);
	initial.fpsr = 0;
	const test::Form reciprocal{test::Operation::reciprocal_estimate,
		true, 32U, 16U};
	test::basic::set_lane(initial, registers.first, 0, 0.0F);
	auto actual = harness.run_interpreter(
		test::instruction(reciprocal, registers), initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0x7f80'0000U);
	test::common::require((actual.fpsr & 2U) != 0U);

	const test::Form square_root{test::Operation::reciprocal_sqrt_estimate,
		true, 32U, 16U};
	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.first, 0, -4.0F);
	actual = harness.run_interpreter(
		test::instruction(square_root, registers), initial);
	test::common::require(std::isnan(std::bit_cast<float>(
		test::basic::lane_bits<float>(actual, registers.destination, 0))));
	test::common::require((actual.fpsr & 1U) != 0U);

	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.first, 0,
		std::bit_cast<float>(0x7f80'0001U));
	actual = harness.run_interpreter(
		test::instruction(reciprocal, registers), initial);
	test::common::require(std::isnan(std::bit_cast<float>(
		test::basic::lane_bits<float>(actual, registers.destination, 0))));
	test::common::require((actual.fpsr & 1U) != 0U);
}

void fused_step_zero_times_infinity_special_case()
{
	test::common::Harness harness;
	auto initial = test::common::state(310);
	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.first, 0, 0.0F);
	test::basic::set_lane(initial, registers.second, 0,
		std::numeric_limits<float>::infinity());
	const test::Form reciprocal{test::Operation::reciprocal_step,
		true, 32U, 16U};
	auto actual = harness.run_interpreter(
		test::instruction(reciprocal, registers), initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == std::bit_cast<std::uint32_t>(2.0F));
	test::common::require((actual.fpsr & 1U) == 0U);

	const test::Form square_root{test::Operation::reciprocal_sqrt_step,
		true, 32U, 16U};
	actual = harness.run_interpreter(
		test::instruction(square_root, registers), initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == std::bit_cast<std::uint32_t>(1.5F));
	test::common::require((actual.fpsr & 1U) == 0U);
}

} // namespace

int main()
{
	scalar_nep_uses_architectural_merge_source();
	zero_negative_and_nan_exception_rules();
	fused_step_zero_times_infinity_special_case();
}
