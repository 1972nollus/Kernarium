// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_complex_test_support.hpp"

namespace test = arm64_floating_complex_test;

namespace {

constexpr test::Registers registers{0, 1, 2};

void multiply_accumulate_has_one_rounding()
{
	test::common::Harness harness;
	const test::Form form{test::Operation::multiply_accumulate, false,
		32U, 16U, 0U, 0U};
	auto initial = test::common::state(300);
	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.destination, 0,
		std::bit_cast<float>(0xbf80'0002U));
	test::basic::set_lane(initial, registers.destination, 1, 0.0F);
	test::basic::set_lane(initial, registers.first, 0,
		std::bit_cast<float>(0x3f80'0001U));
	test::basic::set_lane(initial, registers.first, 1, 0.0F);
	test::basic::set_lane(initial, registers.second, 0,
		std::bit_cast<float>(0x3f80'0001U));
	test::basic::set_lane(initial, registers.second, 1, 0.0F);
	const auto encoded = test::instruction(form, registers);
	const auto actual = harness.run_interpreter(encoded, initial);
	test::common::require(
		test::basic::lane_bits<float>(actual, registers.destination, 0) ==
		0x2880'0000U);
	harness.require_differential(encoded, initial);
}

void signaling_nan_sets_ioc_and_preserves_prior_flags()
{
	test::common::Harness harness;
	const test::Form form{test::Operation::add, false, 32U, 16U, 1U, 0U};
	auto initial = test::common::state(301);
	initial.fpsr = 1U << 4;
	test::basic::set_lane(initial, registers.first, 0,
		std::bit_cast<float>(0x7f80'0001U));
	test::basic::set_lane(initial, registers.first, 1, 1.0F);
	test::basic::set_lane(initial, registers.second, 0, 2.0F);
	test::basic::set_lane(initial, registers.second, 1, 3.0F);
	const auto encoded = test::instruction(form, registers);
	const auto actual = harness.run_interpreter(encoded, initial);
	test::common::require((actual.fpsr & 1U) != 0U);
	test::common::require((actual.fpsr & (1U << 4)) != 0U);
	harness.require_differential(encoded, initial);
}

void default_nan_control_canonicalizes_payload()
{
	test::common::Harness harness;
	const test::Form form{test::Operation::add, false, 32U, 16U, 1U, 0U};
	auto initial = test::common::state(302);
	initial.fpcr = 1U << 25;
	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.first, 0,
		std::bit_cast<float>(0x7fc1'2345U));
	test::basic::set_lane(initial, registers.first, 1, 1.0F);
	test::basic::set_lane(initial, registers.second, 0, 2.0F);
	test::basic::set_lane(initial, registers.second, 1, 3.0F);
	const auto encoded = test::instruction(form, registers);
	const auto actual = harness.run_interpreter(encoded, initial);
	test::common::require(
		test::basic::lane_bits<float>(actual, registers.destination, 0) ==
		0x7fc0'0000U);
	test::common::require((actual.fpsr & 1U) == 0U);
	harness.require_differential(encoded, initial);
}

} // namespace

int main()
{
	multiply_accumulate_has_one_rounding();
	signaling_nan_sets_ioc_and_preserves_prior_flags();
	default_nan_control_canonicalizes_payload();
}
